/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Standard stream filters other than FlateDecode (see inflate.c).
 */
#include "pdf_filter.h"
#include "pdf_mem.h"
#include <string.h>

struct outbuf {
	uint8_t *p;
	size_t n, cap;
};

static int ob_put(struct outbuf *b, const uint8_t *d, size_t len)
{
	if (b->n + len > b->cap) {
		size_t ncap = b->cap ? b->cap : 1024;

		while (ncap < b->n + len) {
			ncap *= 2;
		}
		uint8_t *np = pdf_realloc(b->p, ncap);

		if (np == NULL) {
			return -1;
		}
		b->p = np;
		b->cap = ncap;
	}
	memcpy(b->p + b->n, d, len);
	b->n += len;
	return 0;
}

static int ob_putc(struct outbuf *b, uint8_t c)
{
	return ob_put(b, &c, 1);
}

int pdf_lzw_decode(const uint8_t *in, size_t n, int early, uint8_t **out, size_t *out_len)
{
	/* Dictionary as (prefix code, last byte) pairs; strings rebuilt backwards. */
	uint16_t *prefix = pdf_malloc(4096 * sizeof(uint16_t));
	uint8_t *suffix = pdf_malloc(4096);
	uint8_t *stack = pdf_malloc(4096);
	struct outbuf ob = {0};
	int ret = -1;

	if (prefix == NULL || suffix == NULL || stack == NULL) {
		goto done;
	}
	for (int i = 0; i < 256; i++) {
		prefix[i] = 0xffff;
		suffix[i] = (uint8_t)i;
	}
	int next = 258, codelen = 9;
	int prev = -1;
	uint32_t bitbuf = 0;
	int bitcnt = 0;
	size_t pos = 0;
	uint8_t first = 0;

	for (;;) {
		while (bitcnt < codelen) {
			if (pos >= n) {
				goto finish;
			}
			bitbuf = (bitbuf << 8) | in[pos++];
			bitcnt += 8;
		}
		int code = (int)((bitbuf >> (bitcnt - codelen)) & ((1u << codelen) - 1));

		bitcnt -= codelen;
		if (code == 256) {
			next = 258;
			codelen = 9;
			prev = -1;
			continue;
		}
		if (code == 257) {
			break;
		}
		int sp = 0;
		int c;

		if (code < next) {
			c = code;
			while (c != 0xffff && c >= 0 && sp < 4096) {
				stack[sp++] = suffix[c];
				c = (c < 256) ? 0xffff : prefix[c];
			}
		} else if (code == next && prev >= 0) {
			c = prev;
			stack[sp++] = first;
			while (c != 0xffff && c >= 0 && sp < 4096) {
				stack[sp++] = suffix[c];
				c = (c < 256) ? 0xffff : prefix[c];
			}
		} else {
			break; /* corrupt */
		}
		first = stack[sp - 1];
		for (int i = sp - 1; i >= 0; i--) {
			if (ob_putc(&ob, stack[i]) < 0) {
				goto done;
			}
		}
		if (prev >= 0 && next < 4096) {
			prefix[next] = (uint16_t)prev;
			suffix[next] = first;
			next++;
		}
		prev = code;
		if (next + (early ? 1 : 0) >= (1 << codelen) && codelen < 12) {
			codelen++;
		}
	}
finish:
	ret = 0;
done:
	pdf_free(prefix);
	pdf_free(suffix);
	pdf_free(stack);
	if (ret < 0) {
		pdf_free(ob.p);
		*out = NULL;
		*out_len = 0;
	} else {
		*out = ob.p;
		*out_len = ob.n;
	}
	return ret;
}

int pdf_a85_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len)
{
	struct outbuf ob = {0};
	uint32_t tuple = 0;
	int cnt = 0;
	size_t i = 0;

	if (n >= 2 && in[0] == '<' && in[1] == '~') {
		i = 2;
	}
	for (; i < n; i++) {
		uint8_t c = in[i];

		if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0') {
			continue;
		}
		if (c == '~') {
			break;
		}
		if (c == 'z' && cnt == 0) {
			uint8_t z[4] = {0, 0, 0, 0};

			if (ob_put(&ob, z, 4) < 0) {
				goto fail;
			}
			continue;
		}
		if (c < '!' || c > 'u') {
			continue;
		}
		tuple = tuple * 85 + (c - '!');
		if (++cnt == 5) {
			uint8_t b[4] = {tuple >> 24, tuple >> 16, tuple >> 8, tuple};

			if (ob_put(&ob, b, 4) < 0) {
				goto fail;
			}
			tuple = 0;
			cnt = 0;
		}
	}
	if (cnt > 0) {
		for (int k = cnt; k < 5; k++) {
			tuple = tuple * 85 + 84;
		}
		uint8_t b[4] = {tuple >> 24, tuple >> 16, tuple >> 8, tuple};

		if (ob_put(&ob, b, cnt - 1) < 0) {
			goto fail;
		}
	}
	*out = ob.p;
	*out_len = ob.n;
	return 0;
fail:
	pdf_free(ob.p);
	*out = NULL;
	*out_len = 0;
	return -1;
}

static int hexv(uint8_t c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int pdf_ahx_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len)
{
	struct outbuf ob = {0};
	int hi = -1;

	for (size_t i = 0; i < n; i++) {
		if (in[i] == '>') {
			break;
		}
		int v = hexv(in[i]);

		if (v < 0) {
			continue;
		}
		if (hi < 0) {
			hi = v;
		} else {
			if (ob_putc(&ob, (uint8_t)((hi << 4) | v)) < 0) {
				goto fail;
			}
			hi = -1;
		}
	}
	if (hi >= 0 && ob_putc(&ob, (uint8_t)(hi << 4)) < 0) {
		goto fail;
	}
	*out = ob.p;
	*out_len = ob.n;
	return 0;
fail:
	pdf_free(ob.p);
	*out = NULL;
	*out_len = 0;
	return -1;
}

int pdf_rl_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len)
{
	struct outbuf ob = {0};
	size_t i = 0;

	while (i < n) {
		uint8_t l = in[i++];

		if (l == 128) {
			break;
		}
		if (l < 128) {
			size_t cnt = l + 1u;

			if (i + cnt > n) {
				cnt = n - i;
			}
			if (ob_put(&ob, in + i, cnt) < 0) {
				goto fail;
			}
			i += cnt;
		} else {
			if (i >= n) {
				break;
			}
			uint8_t b = in[i++];

			for (int k = 0; k < 257 - l; k++) {
				if (ob_putc(&ob, b) < 0) {
					goto fail;
				}
			}
		}
	}
	*out = ob.p;
	*out_len = ob.n;
	return 0;
fail:
	pdf_free(ob.p);
	*out = NULL;
	*out_len = 0;
	return -1;
}

size_t pdf_apply_predictor(uint8_t *data, size_t n, int predictor, int colors, int bpc, int columns)
{
	if (predictor <= 1) {
		return n;
	}
	if (colors <= 0) {
		colors = 1;
	}
	if (bpc <= 0) {
		bpc = 8;
	}
	if (columns <= 0) {
		columns = 1;
	}
	size_t bpp = ((size_t)colors * bpc + 7) / 8;
	size_t rowlen = ((size_t)colors * bpc * columns + 7) / 8;

	if (bpp == 0) {
		bpp = 1;
	}
	if (predictor == 2) {
		/* TIFF predictor, 8-bit components only */
		if (bpc != 8) {
			return n;
		}
		for (size_t r = 0; r + rowlen <= n; r += rowlen) {
			for (size_t i = bpp; i < rowlen; i++) {
				data[r + i] += data[r + i - bpp];
			}
		}
		return n;
	}
	/* PNG predictors: each row has a leading filter type byte */
	size_t in_row = rowlen + 1;
	size_t rows = n / in_row;
	uint8_t *prev = NULL;
	size_t out = 0;

	for (size_t r = 0; r < rows; r++) {
		uint8_t *src = data + r * in_row;
		uint8_t ft = src[0];
		uint8_t *row = src + 1;

		for (size_t i = 0; i < rowlen; i++) {
			uint8_t a = (i >= bpp) ? row[i - bpp] : 0;
			uint8_t b = prev ? prev[i] : 0;
			uint8_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
			uint8_t x = row[i];

			switch (ft) {
			case 1:
				x += a;
				break;
			case 2:
				x += b;
				break;
			case 3:
				x += (uint8_t)(((unsigned)a + b) / 2);
				break;
			case 4: {
				int p = (int)a + b - c;
				int pa = p > a ? p - a : a - p;
				int pb = p > b ? p - b : b - p;
				int pc = p > c ? p - c : c - p;

				x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
				break;
			}
			default:
				break;
			}
			row[i] = x;
		}
		/* row is now decoded in place; move it to the packed output */
		memmove(data + out, row, rowlen);
		prev = data + out;
		out += rowlen;
	}
	return out;
}
