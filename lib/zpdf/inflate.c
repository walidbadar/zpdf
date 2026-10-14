/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Canonical Huffman DEFLATE decoder in the style of zlib's "puff", with a
 * growable output buffer. No tables larger than a few hundred bytes are
 * held on the stack; the decoder state lives on the heap.
 */
#include "inflate.h"
#include "pdf_mem.h"
#include <string.h>

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

struct huff {
	uint16_t count[MAXBITS + 1];
	uint16_t symbol[FIXLCODES];
};

#define WINDOW 32768
#define WINBUF (WINDOW * 2 + 512)

struct state {
	const uint8_t *in;
	size_t in_len;
	size_t in_pos;
	uint32_t bitbuf;
	int bitcnt;

	/* output: either a growing buffer or a sliding window flushed to a sink */
	uint8_t *out;
	size_t out_len;
	size_t out_cap;
	inflate_sink_fn sink;
	void *sink_ud;
	size_t total_out;
	int err;

	struct huff lencode;
	struct huff distcode;
	uint16_t lengths[MAXCODES];
};

/* Push out everything but the last WINDOW bytes (kept for back references). */
static int flush_window(struct state *s, bool final)
{
	size_t keep = final ? 0 : WINDOW;

	if (s->out_len <= keep) {
		return 0;
	}
	size_t n = s->out_len - keep;

	if (s->sink(s->sink_ud, s->out, n) < 0) {
		s->err = 3;
		return -1;
	}
	s->total_out += n;
	memmove(s->out, s->out + n, keep);
	s->out_len = keep;
	return 0;
}

static int ensure_out(struct state *s, size_t extra)
{
	if (s->out_len + extra <= s->out_cap) {
		return 0;
	}
	if (s->sink != NULL) {
		/* window mode: make room by flushing */
		if (flush_window(s, false) < 0 || s->out_len + extra > s->out_cap) {
			return -1;
		}
		return 0;
	}
	size_t ncap = s->out_cap ? s->out_cap : 4096;

	while (ncap < s->out_len + extra) {
		ncap *= 2;
		if (ncap > (64u << 20)) {
			return -1;
		}
	}
	uint8_t *n = pdf_realloc(s->out, ncap);

	if (n == NULL) {
		return -1;
	}
	s->out = n;
	s->out_cap = ncap;
	return 0;
}

static int bits(struct state *s, int need)
{
	uint32_t val = s->bitbuf;

	while (s->bitcnt < need) {
		if (s->in_pos >= s->in_len) {
			s->err = 1;
			return -1;
		}
		val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)(val & ((1u << need) - 1));
}

static int decode(struct state *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0;

	for (int len = 1; len <= MAXBITS; len++) {
		int b = bits(s, 1);

		if (b < 0) {
			return -1;
		}
		code |= b;
		int count = h->count[len];

		if (code - count < first) {
			return h->symbol[index + (code - first)];
		}
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->err = 1;
	return -1;
}

static int construct(struct huff *h, const uint16_t *length, int n)
{
	int offs[MAXBITS + 1];

	for (int len = 0; len <= MAXBITS; len++) {
		h->count[len] = 0;
	}
	for (int sym = 0; sym < n; sym++) {
		h->count[length[sym]]++;
	}
	if (h->count[0] == n) {
		return 0;
	}
	int left = 1;

	for (int len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0) {
			return left;
		}
	}
	offs[1] = 0;
	for (int len = 1; len < MAXBITS; len++) {
		offs[len + 1] = offs[len] + h->count[len];
	}
	for (int sym = 0; sym < n; sym++) {
		if (length[sym] != 0) {
			h->symbol[offs[length[sym]]++] = (uint16_t)sym;
		}
	}
	return left;
}

static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
				   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
				  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
				   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
				   6145, 8193, 12289, 16385, 24577};
static const uint16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
				  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct state *s)
{
	for (;;) {
		int sym = decode(s, &s->lencode);

		if (sym < 0) {
			return -1;
		}
		if (sym < 256) {
			if (ensure_out(s, 1)) {
				s->err = 2;
				return -1;
			}
			s->out[s->out_len++] = (uint8_t)sym;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29) {
				s->err = 1;
				return -1;
			}
			int eb = bits(s, lext[sym]);

			if (eb < 0) {
				return -1;
			}
			int len = lbase[sym] + eb;

			sym = decode(s, &s->distcode);
			if (sym < 0) {
				return -1;
			}
			if (sym >= 30) {
				s->err = 1;
				return -1;
			}
			eb = bits(s, dext[sym]);
			if (eb < 0) {
				return -1;
			}
			size_t dist = dbase[sym] + (size_t)eb;

			if (dist > s->out_len) {
				s->err = 1;
				return -1;
			}
			if (ensure_out(s, len)) {
				s->err = 2;
				return -1;
			}
			uint8_t *dst = s->out + s->out_len;
			const uint8_t *src = dst - dist;

			for (int i = 0; i < len; i++) {
				dst[i] = src[i];
			}
			s->out_len += len;
		}
	}
}

static int stored(struct state *s)
{
	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->in_pos + 4 > s->in_len) {
		s->err = 1;
		return -1;
	}
	unsigned len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
	unsigned nlen = s->in[s->in_pos + 2] | (s->in[s->in_pos + 3] << 8);

	s->in_pos += 4;
	if ((len ^ 0xffffu) != nlen) {
		s->err = 1;
		return -1;
	}
	if (s->in_pos + len > s->in_len) {
		len = (unsigned)(s->in_len - s->in_pos);
		s->err = 1;
	}
	if (ensure_out(s, len)) {
		s->err = 2;
		return -1;
	}
	memcpy(s->out + s->out_len, s->in + s->in_pos, len);
	s->out_len += len;
	s->in_pos += len;
	return s->err ? -1 : 0;
}

static int fixed(struct state *s)
{
	int sym;

	for (sym = 0; sym < 144; sym++) {
		s->lengths[sym] = 8;
	}
	for (; sym < 256; sym++) {
		s->lengths[sym] = 9;
	}
	for (; sym < 280; sym++) {
		s->lengths[sym] = 7;
	}
	for (; sym < FIXLCODES; sym++) {
		s->lengths[sym] = 8;
	}
	construct(&s->lencode, s->lengths, FIXLCODES);
	for (sym = 0; sym < MAXDCODES; sym++) {
		s->lengths[sym] = 5;
	}
	construct(&s->distcode, s->lengths, MAXDCODES);
	return codes(s);
}

static int dynamic(struct state *s)
{
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
					  11, 4, 12, 3, 13, 2, 14, 1, 15};
	int nlen, ndist, ncode, index;
	int b;

	if ((b = bits(s, 5)) < 0) {
		return -1;
	}
	nlen = b + 257;
	if ((b = bits(s, 5)) < 0) {
		return -1;
	}
	ndist = b + 1;
	if ((b = bits(s, 4)) < 0) {
		return -1;
	}
	ncode = b + 4;
	if (nlen > MAXLCODES || ndist > MAXDCODES) {
		s->err = 1;
		return -1;
	}
	for (index = 0; index < ncode; index++) {
		if ((b = bits(s, 3)) < 0) {
			return -1;
		}
		s->lengths[order[index]] = (uint16_t)b;
	}
	for (; index < 19; index++) {
		s->lengths[order[index]] = 0;
	}
	if (construct(&s->lencode, s->lengths, 19) != 0) {
		s->err = 1;
		return -1;
	}
	index = 0;
	while (index < nlen + ndist) {
		int sym = decode(s, &s->lencode);

		if (sym < 0) {
			return -1;
		}
		if (sym < 16) {
			s->lengths[index++] = (uint16_t)sym;
		} else {
			int len = 0, rep;

			if (sym == 16) {
				if (index == 0) {
					s->err = 1;
					return -1;
				}
				len = s->lengths[index - 1];
				if ((b = bits(s, 2)) < 0) {
					return -1;
				}
				rep = 3 + b;
			} else if (sym == 17) {
				if ((b = bits(s, 3)) < 0) {
					return -1;
				}
				rep = 3 + b;
			} else {
				if ((b = bits(s, 7)) < 0) {
					return -1;
				}
				rep = 11 + b;
			}
			if (index + rep > nlen + ndist) {
				s->err = 1;
				return -1;
			}
			while (rep--) {
				s->lengths[index++] = (uint16_t)len;
			}
		}
	}
	if (s->lengths[256] == 0) {
		s->err = 1;
		return -1;
	}
	int err = construct(&s->lencode, s->lengths, nlen);

	if (err < 0 || (err > 0 && nlen - s->lencode.count[0] != 1)) {
		s->err = 1;
		return -1;
	}
	err = construct(&s->distcode, s->lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - s->distcode.count[0] != 1)) {
		s->err = 1;
		return -1;
	}
	return codes(s);
}

static int run(struct state *s)
{
	int last, type, err = 0;

	do {
		last = bits(s, 1);
		type = bits(s, 2);
		if (last < 0 || type < 0) {
			err = -1;
			break;
		}
		if (type == 0) {
			err = stored(s);
		} else if (type == 1) {
			err = fixed(s);
		} else if (type == 2) {
			err = dynamic(s);
		} else {
			s->err = 1;
			err = -1;
		}
	} while (!last && err == 0);
	return err;
}

static const uint8_t *skip_header(const uint8_t *in, size_t *in_len)
{
	/* Some producers put whitespace or garbage before the zlib header. */
	while (*in_len > 0 && (*in == '\n' || *in == '\r' || *in == ' ' || *in == '\t')) {
		in++;
		(*in_len)--;
	}
	if (*in_len >= 2 && (in[0] & 0x0f) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) {
		in += 2;
		*in_len -= 2;
	}
	return in;
}

int inflate_buf(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len)
{
	*out = NULL;
	*out_len = 0;
	in = skip_header(in, &in_len);
	if (in_len < 1) {
		return -1;
	}
	struct state *s = pdf_calloc(sizeof(*s));

	if (s == NULL) {
		return -2;
	}
	s->in = in;
	s->in_len = in_len;

	int err = run(s);
	int ret;

	if (s->out_len == 0) {
		pdf_free(s->out);
		ret = -1;
	} else {
		*out = s->out;
		*out_len = s->out_len;
		ret = (err == 0) ? 0 : 1;
	}
	pdf_free(s);
	return ret;
}

int inflate_stream(const uint8_t *in, size_t in_len, inflate_sink_fn sink, void *ud)
{
	in = skip_header(in, &in_len);
	if (in_len < 1) {
		return -1;
	}
	struct state *s = pdf_calloc(sizeof(*s));

	if (s == NULL) {
		return -2;
	}
	s->in = in;
	s->in_len = in_len;
	s->sink = sink;
	s->sink_ud = ud;
	s->out = pdf_malloc(WINBUF);
	if (s->out == NULL) {
		pdf_free(s);
		return -2;
	}
	s->out_cap = WINBUF;

	int err = run(s);

	if (s->err != 3) {
		flush_window(s, true);
	}
	int ret = (s->err == 3) ? -3 : (s->total_out == 0 ? -1 : (err == 0 ? 0 : 1));

	pdf_free(s->out);
	pdf_free(s);
	return ret;
}
