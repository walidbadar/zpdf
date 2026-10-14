/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Baseline JPEG: Huffman decoding, dequantisation, separable integer IDCT,
 * chroma upsampling (nearest) and colour conversion. Restart intervals and
 * arbitrary sampling factors up to 2x2 are handled.
 */
#include "pdf_jpeg.h"
#include "pdf_mem.h"
#include <string.h>

#define MAX_COMP 4

#define HUFF_FAST_BITS 9

struct huff {
	uint8_t bits[17];
	uint8_t vals[256];
	/* lookup: code lengths 1..16 */
	uint16_t mincode[17];
	int32_t maxcode[18];
	uint16_t valptr[17];
	/* fast path: the next HUFF_FAST_BITS bits -> (length << 8) | value, 0 = longer code */
	uint16_t fast[1 << HUFF_FAST_BITS];
	bool present;
};

struct comp {
	int id, h, v, tq;
	int td, ta;
	int dc_pred;
	uint8_t *pix; /* component plane, bw*8 x bh*8 */
	int bw, bh;   /* blocks per line / column (padded to MCU) */
	int stride;
};

struct jpeg {
	const uint8_t *d;
	size_t len, pos;
	uint16_t qt[4][64];
	struct huff hdc[4], hac[4];
	struct comp c[MAX_COMP];
	int ncomp;
	int w, h;
	int hmax, vmax;
	int mcux, mcuy;
	int restart;
	bool adobe;
	int adobe_transform;
	bool progressive;
	/* bit reader */
	uint32_t bitbuf;
	int bitcnt;
	bool eof;
};

static const uint8_t zigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
				   12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
				   35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
				   58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void build_huff(struct huff *h)
{
	int code = 0, k = 0;

	for (int l = 1; l <= 16; l++) {
		h->valptr[l] = (uint16_t)k;
		h->mincode[l] = (uint16_t)code;
		code += h->bits[l];
		k += h->bits[l];
		h->maxcode[l] = h->bits[l] ? code - 1 : -1;
		code <<= 1;
	}
	h->maxcode[17] = 0x7fffffff;
	/* fast lookup table for codes up to HUFF_FAST_BITS long */
	memset(h->fast, 0, sizeof(h->fast));
	code = 0;
	k = 0;
	for (int l = 1; l <= HUFF_FAST_BITS; l++) {
		for (int i = 0; i < h->bits[l]; i++, k++, code++) {
			int shift = HUFF_FAST_BITS - l;
			int base = code << shift;

			for (int j = 0; j < (1 << shift); j++) {
				h->fast[base + j] = (uint16_t)((l << 8) | h->vals[k]);
			}
		}
		code <<= 1;
	}
	h->present = true;
}

static int fill_bits(struct jpeg *j)
{
	while (j->bitcnt <= 24) {
		uint32_t b = 0;

		if (j->pos < j->len && !j->eof) {
			b = j->d[j->pos];
			if (b == 0xFF) {
				uint8_t n = (j->pos + 1 < j->len) ? j->d[j->pos + 1] : 0;

				if (n == 0) {
					j->pos += 2;
				} else if (n >= 0xD0 && n <= 0xD7) {
					/* restart marker: leave for the MCU loop */
					b = 0;
					j->eof = true;
				} else {
					b = 0;
					j->eof = true;
				}
			} else {
				j->pos++;
			}
		}
		j->bitbuf |= b << (24 - j->bitcnt);
		j->bitcnt += 8;
	}
	return 0;
}

static inline int get_bits(struct jpeg *j, int n)
{
	if (n == 0) {
		return 0;
	}
	if (j->bitcnt < n) {
		fill_bits(j);
	}
	int v = (int)(j->bitbuf >> (32 - n));

	j->bitbuf <<= n;
	j->bitcnt -= n;
	return v;
}

static inline int get_bit(struct jpeg *j)
{
	return get_bits(j, 1);
}

static int decode_huff(struct jpeg *j, const struct huff *h)
{
	if (j->bitcnt < 16) {
		fill_bits(j);
	}
	uint16_t f = h->fast[j->bitbuf >> (32 - HUFF_FAST_BITS)];

	if (f != 0) {
		int l = f >> 8;

		j->bitbuf <<= l;
		j->bitcnt -= l;
		return f & 0xFF;
	}
	int code = 0;

	for (int l = 1; l <= 16; l++) {
		code = (code << 1) | get_bit(j);
		if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) {
			return h->vals[h->valptr[l] + code - h->mincode[l]];
		}
	}
	return 0; /* corrupt: treat as EOB */
}

static inline int extend(int v, int t)
{
	return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v;
}

/*
 * Separable float IDCT producing a k x k block (k = 8 / denom). For k < 8
 * the output samples are taken at the centres of k equal intervals, which
 * is a cheap box-filtered downscale in the DCT domain.
 */
static float cosv[8][8];   /* full resolution */
static float cosk[4][8][8]; /* [log2 denom][x][u] for k = 8, 4, 2, 1 */
static bool cos_init;

static void init_cos(void)
{
	if (cos_init) {
		return;
	}
	for (int d = 0; d < 4; d++) {
		int k = 8 >> d;

		for (int x = 0; x < k; x++) {
			float xx = ((float)x + 0.5f) * 8.0f / (float)k - 0.5f;

			for (int u = 0; u < 8; u++) {
				float cu = u == 0 ? 0.70710678f : 1.0f;

				cosk[d][x][u] = cu * (float)__builtin_cos((2.0 * (double)xx + 1.0) * u * 3.14159265358979 / 16.0);
			}
		}
	}
	memcpy(cosv, cosk[0], sizeof(cosv));
	cos_init = true;
}

static void idct_block(const int *in, uint8_t *out, int stride, int logd)
{
	int k = 8 >> logd;
	const float (*ct)[8] = cosk[logd];
	float rows[64];

	/* rows: for each of the 8 input rows produce k samples */
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < k; x++) {
			float s = 0;

			for (int u = 0; u < 8; u++) {
				int c = in[y * 8 + u];

				if (c) {
					s += c * ct[x][u];
				}
			}
			rows[y * 8 + x] = s;
		}
	}
	for (int x = 0; x < k; x++) {
		for (int y = 0; y < k; y++) {
			float s = 0;

			for (int v = 0; v < 8; v++) {
				s += rows[v * 8 + x] * ct[y][v];
			}
			int p = (int)(s * 0.25f + 128.5f);

			out[y * stride + x] = (uint8_t)(p < 0 ? 0 : (p > 255 ? 255 : p));
		}
	}
}

static int decode_block(struct jpeg *j, struct comp *c, int *blk)
{
	memset(blk, 0, 64 * sizeof(int));
	int t = decode_huff(j, &j->hdc[c->td]);
	int diff = t ? extend(get_bits(j, t), t) : 0;

	c->dc_pred += diff;
	blk[0] = c->dc_pred * j->qt[c->tq][0];
	int k = 1;

	while (k < 64) {
		int rs = decode_huff(j, &j->hac[c->ta]);
		int r = rs >> 4, s = rs & 15;

		if (s == 0) {
			if (r == 15) {
				k += 16;
				continue;
			}
			break;
		}
		k += r;
		if (k > 63) {
			break;
		}
		blk[zigzag[k]] = extend(get_bits(j, s), s) * j->qt[c->tq][k];
		k++;
	}
	return 0;
}

static int parse_headers(struct jpeg *j, bool full)
{
	if (j->len < 4 || j->d[0] != 0xFF || j->d[1] != 0xD8) {
		return -1;
	}
	j->pos = 2;
	while (j->pos + 4 <= j->len) {
		if (j->d[j->pos] != 0xFF) {
			j->pos++;
			continue;
		}
		uint8_t m = j->d[j->pos + 1];

		if (m == 0xFF) {
			j->pos++;
			continue;
		}
		j->pos += 2;
		if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
			continue;
		}
		if (j->pos + 2 > j->len) {
			return -1;
		}
		size_t seglen = rd16(j->d + j->pos);
		const uint8_t *s = j->d + j->pos + 2;
		size_t sl = seglen >= 2 ? seglen - 2 : 0;

		if (j->pos + seglen > j->len) {
			sl = j->len - j->pos - 2;
		}
		switch (m) {
		case 0xC0:
		case 0xC1:
		case 0xC2:
		case 0xC3:
		case 0xC5:
		case 0xC6:
		case 0xC7:
		case 0xC9:
		case 0xCA:
		case 0xCB:
		case 0xCD:
		case 0xCE:
		case 0xCF: {
			if (sl < 6) {
				return -1;
			}
			j->progressive = (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE);
			if (m != 0xC0 && m != 0xC1 && m != 0xC2) {
				j->progressive = true; /* lossless/arithmetic: not supported either */
			}
			if (s[0] != 8 && !j->progressive) {
				return -2; /* 12 bit */
			}
			j->h = rd16(s + 1);
			j->w = rd16(s + 3);
			j->ncomp = s[5];
			if (j->ncomp < 1 || j->ncomp > MAX_COMP || sl < 6 + (size_t)j->ncomp * 3) {
				return -1;
			}
			for (int i = 0; i < j->ncomp; i++) {
				j->c[i].id = s[6 + i * 3];
				j->c[i].h = s[7 + i * 3] >> 4;
				j->c[i].v = s[7 + i * 3] & 15;
				j->c[i].tq = s[8 + i * 3] & 3;
				if (j->c[i].h < 1 || j->c[i].h > 2 || j->c[i].v < 1 || j->c[i].v > 2) {
					if (j->c[i].h < 1 || j->c[i].v < 1 || j->c[i].h > 4 || j->c[i].v > 4) {
						return -1;
					}
				}
			}
			if (!full) {
				return 0;
			}
			break;
		}
		case 0xC4: { /* DHT */
			size_t p = 0;

			while (p + 17 <= sl) {
				int tc = s[p] >> 4, th = s[p] & 15;

				if (th > 3) {
					return -1;
				}
				struct huff *h = tc ? &j->hac[th] : &j->hdc[th];
				int total = 0;

				h->bits[0] = 0;
				for (int i = 1; i <= 16; i++) {
					h->bits[i] = s[p + i];
					total += h->bits[i];
				}
				if (total > 256 || p + 17 + (size_t)total > sl) {
					return -1;
				}
				memcpy(h->vals, s + p + 17, (size_t)total);
				build_huff(h);
				p += 17 + (size_t)total;
			}
			break;
		}
		case 0xDB: { /* DQT */
			size_t p = 0;

			while (p < sl) {
				int pq = s[p] >> 4, tq = s[p] & 3;

				p++;
				for (int i = 0; i < 64; i++) {
					if (pq) {
						if (p + 2 > sl) {
							return -1;
						}
						j->qt[tq][i] = rd16(s + p);
						p += 2;
					} else {
						if (p >= sl) {
							return -1;
						}
						j->qt[tq][i] = s[p++];
					}
				}
			}
			break;
		}
		case 0xDD: /* DRI */
			if (sl >= 2) {
				j->restart = rd16(s);
			}
			break;
		case 0xEE: /* APP14 Adobe */
			if (sl >= 12 && memcmp(s, "Adobe", 5) == 0) {
				j->adobe = true;
				j->adobe_transform = s[11];
			}
			break;
		case 0xDA: /* SOS */
			if (sl < 1) {
				return -1;
			}
			{
				int ns = s[0];

				if (sl < 1 + (size_t)ns * 2) {
					return -1;
				}
				for (int i = 0; i < ns; i++) {
					int cid = s[1 + i * 2];

					for (int k = 0; k < j->ncomp; k++) {
						if (j->c[k].id == cid) {
							j->c[k].td = s[2 + i * 2] >> 4;
							j->c[k].ta = s[2 + i * 2] & 15;
						}
					}
				}
			}
			j->pos += seglen;
			return 1; /* scan data follows */
		case 0xD9:
			return -1;
		default:
			break;
		}
		j->pos += seglen;
	}
	return -1;
}

int pdf_jpeg_info(const uint8_t *data, size_t len, int *w, int *h, int *ncomp, bool *progressive)
{
	struct jpeg *j = pdf_calloc(sizeof(*j));

	if (j == NULL) {
		return -1;
	}
	j->d = data;
	j->len = len;
	int rc = parse_headers(j, false);

	*w = j->w;
	*h = j->h;
	*ncomp = j->ncomp;
	*progressive = j->progressive;
	pdf_free(j);
	return rc < 0 ? rc : 0;
}

static inline uint8_t clamp8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

int pdf_jpeg_decode(const uint8_t *data, size_t len, uint8_t **out, int *w, int *h, int *ncomp,
		    bool *adobe)
{
	return pdf_jpeg_decode_scaled(data, len, 1, out, w, h, ncomp, adobe);
}

int pdf_jpeg_decode_scaled(const uint8_t *data, size_t len, int denom, uint8_t **out, int *w, int *h,
			   int *ncomp, bool *adobe)
{
	struct jpeg *j = pdf_calloc(sizeof(*j));
	int rc = -1;
	int logd = denom >= 8 ? 3 : (denom >= 4 ? 2 : (denom >= 2 ? 1 : 0));
	int k = 8 >> logd;

	init_cos();
	*out = NULL;
	if (j == NULL) {
		return -1;
	}
	j->d = data;
	j->len = len;
	if (parse_headers(j, true) != 1) {
		goto done;
	}
	if (j->progressive) {
		rc = -3;
		goto done;
	}
	j->hmax = j->vmax = 1;
	for (int i = 0; i < j->ncomp; i++) {
		if (j->c[i].h > j->hmax) {
			j->hmax = j->c[i].h;
		}
		if (j->c[i].v > j->vmax) {
			j->vmax = j->c[i].v;
		}
	}
	j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
	j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
	if ((size_t)j->mcux * (size_t)j->mcuy > (1u << 22)) {
		goto done;
	}
	for (int i = 0; i < j->ncomp; i++) {
		struct comp *c = &j->c[i];

		c->bw = j->mcux * c->h;
		c->bh = j->mcuy * c->v;
		c->stride = c->bw * k;
		c->pix = pdf_malloc((size_t)c->stride * (size_t)c->bh * (size_t)k);
		if (c->pix == NULL) {
			goto done;
		}
	}
	int blk[64];
	int mcu_count = 0;

	for (int my = 0; my < j->mcuy; my++) {
		for (int mx = 0; mx < j->mcux; mx++) {
			if (j->restart && mcu_count > 0 && (mcu_count % j->restart) == 0) {
				/* byte-align, skip the RSTn marker, reset predictors */
				j->bitbuf = 0;
				j->bitcnt = 0;
				j->eof = false;
				while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 &&
							       j->d[j->pos + 1] <= 0xD7)) {
					j->pos++;
				}
				if (j->pos + 1 < j->len) {
					j->pos += 2;
				}
				for (int i = 0; i < j->ncomp; i++) {
					j->c[i].dc_pred = 0;
				}
			}
			for (int i = 0; i < j->ncomp; i++) {
				struct comp *c = &j->c[i];

				for (int by = 0; by < c->v; by++) {
					for (int bx = 0; bx < c->h; bx++) {
						decode_block(j, c, blk);
						int px = (mx * c->h + bx) * k, py = (my * c->v + by) * k;

						idct_block(blk, c->pix + py * c->stride + px, c->stride, logd);
					}
				}
			}
			mcu_count++;
		}
	}
	/* assemble output at the reduced size */
	int ow = (j->w + (1 << logd) - 1) >> logd, oh = (j->h + (1 << logd) - 1) >> logd;
	size_t npix = (size_t)ow * (size_t)oh;
	uint8_t *o = pdf_malloc(npix * (size_t)j->ncomp);

	if (o == NULL) {
		goto done;
	}
	bool ycc = (j->ncomp == 3 && (!j->adobe || j->adobe_transform != 0)) ||
		   (j->ncomp == 4 && j->adobe && j->adobe_transform == 2);

	for (int y = 0; y < oh; y++) {
		for (int x = 0; x < ow; x++) {
			int s[MAX_COMP];

			for (int i = 0; i < j->ncomp; i++) {
				struct comp *c = &j->c[i];
				int sx = x * c->h / j->hmax, sy = y * c->v / j->vmax;
				int maxx = c->bw * k - 1, maxy = c->bh * k - 1;

				if (sx > maxx) {
					sx = maxx;
				}
				if (sy > maxy) {
					sy = maxy;
				}
				s[i] = c->pix[sy * c->stride + sx];
			}
			uint8_t *p = o + ((size_t)y * (size_t)ow + (size_t)x) * (size_t)j->ncomp;

			if (ycc) {
				int Y = s[0], cb = s[1] - 128, cr = s[2] - 128;

				p[0] = clamp8(Y + ((91881 * cr) >> 16));
				p[1] = clamp8(Y - ((22554 * cb + 46802 * cr) >> 16));
				p[2] = clamp8(Y + ((116130 * cb) >> 16));
				if (j->ncomp == 4) {
					p[3] = (uint8_t)s[3];
				}
			} else {
				for (int i = 0; i < j->ncomp; i++) {
					p[i] = (uint8_t)s[i];
				}
			}
		}
	}
	*out = o;
	*w = ow;
	*h = oh;
	*ncomp = j->ncomp;
	*adobe = j->adobe;
	rc = 0;
done:
	for (int i = 0; i < MAX_COMP; i++) {
		pdf_free(j->c[i].pix);
	}
	pdf_free(j);
	return rc;
}
