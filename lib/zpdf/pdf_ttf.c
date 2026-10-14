/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_ttf.h"
#include "pdf_mem.h"
#include <string.h>

#define MAX_GLYPHS 65536

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int find_table(const uint8_t *data, size_t len, const char *tag, size_t *off, size_t *tlen)
{
	if (len < 12) {
		return -1;
	}
	size_t base = 0;
	uint32_t magic = rd32(data);

	/* TrueType collection: use the first font */
	if (magic == 0x74746366u /* ttcf */) {
		if (len < 16) {
			return -1;
		}
		base = rd32(data + 12);
		if (base + 12 > len) {
			return -1;
		}
	}
	uint16_t ntab = rd16(data + base + 4);

	for (uint16_t i = 0; i < ntab; i++) {
		size_t rec = base + 12 + (size_t)i * 16;

		if (rec + 16 > len) {
			break;
		}
		if (memcmp(data + rec, tag, 4) == 0) {
			*off = rd32(data + rec + 8);
			*tlen = rd32(data + rec + 12);
			if (*off >= len) {
				return -1;
			}
			if (*off + *tlen > len) {
				*tlen = len - *off;
			}
			return 0;
		}
	}
	return -1;
}

static int find_table32(const uint8_t *data, size_t len, const char *tag, uint32_t *off, uint32_t *tlen)
{
	size_t o, l;

	if (find_table(data, len, tag, &o, &l) < 0) {
		return -1;
	}
	*off = (uint32_t)o;
	*tlen = (uint32_t)l;
	return 0;
}

static void set_map(uint32_t *map, size_t n, uint32_t gid, uint32_t cp, int *count)
{
	if (gid != 0 && gid < n && map[gid] == 0 && cp != 0) {
		map[gid] = cp;
		(*count)++;
	}
}

static int parse_format4(const uint8_t *t, size_t tlen, uint32_t *map, size_t n, bool symbol)
{
	if (tlen < 16) {
		return 0;
	}
	size_t segx2 = rd16(t + 6);
	size_t nseg = segx2 / 2;
	size_t ends = 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
	int count = 0;

	if (ranges + segx2 > tlen) {
		return 0;
	}
	for (size_t s = 0; s < nseg; s++) {
		uint32_t end = rd16(t + ends + s * 2);
		uint32_t start = rd16(t + starts + s * 2);
		uint16_t delta = rd16(t + deltas + s * 2);
		uint16_t ro = rd16(t + ranges + s * 2);

		if (start > end || start == 0xFFFF) {
			continue;
		}
		for (uint32_t c = start; c <= end && c != 0xFFFF; c++) {
			uint32_t gid;

			if (ro == 0) {
				gid = (c + delta) & 0xFFFF;
			} else {
				size_t gi = ranges + s * 2 + ro + (c - start) * 2;

				if (gi + 2 > tlen) {
					continue;
				}
				gid = rd16(t + gi);
				if (gid != 0) {
					gid = (gid + delta) & 0xFFFF;
				}
			}
			uint32_t cp = c;

			if (symbol && (cp & 0xFF00) == 0xF000) {
				cp &= 0xFF;
			}
			set_map(map, n, gid, cp, &count);
		}
	}
	return count;
}

static int parse_format12(const uint8_t *t, size_t tlen, uint32_t *map, size_t n)
{
	if (tlen < 16) {
		return 0;
	}
	uint32_t ngroups = rd32(t + 12);
	int count = 0;

	for (uint32_t g = 0; g < ngroups; g++) {
		size_t off = 16 + (size_t)g * 12;

		if (off + 12 > tlen) {
			break;
		}
		uint32_t start = rd32(t + off), end = rd32(t + off + 4), gid = rd32(t + off + 8);

		if (end < start || end - start > 65535) {
			continue;
		}
		for (uint32_t c = start; c <= end; c++) {
			set_map(map, n, gid + (c - start), c, &count);
		}
	}
	return count;
}

static int parse_format0(const uint8_t *t, size_t tlen, uint32_t *map, size_t n)
{
	int count = 0;

	if (tlen < 6 + 256) {
		return 0;
	}
	for (uint32_t c = 0; c < 256; c++) {
		set_map(map, n, t[6 + c], c, &count);
	}
	return count;
}

int pdf_ttf_gid_to_unicode(const uint8_t *data, size_t len, uint32_t **map, size_t *n)
{
	size_t cmap_off, cmap_len, maxp_off, maxp_len;

	*map = NULL;
	*n = 0;
	if (find_table(data, len, "cmap", &cmap_off, &cmap_len) < 0 || cmap_len < 4) {
		return 0;
	}
	size_t nglyphs = MAX_GLYPHS;

	if (find_table(data, len, "maxp", &maxp_off, &maxp_len) == 0 && maxp_len >= 6) {
		nglyphs = rd16(data + maxp_off + 4);
		if (nglyphs == 0) {
			nglyphs = MAX_GLYPHS;
		}
	}
	const uint8_t *cm = data + cmap_off;
	uint16_t ntab = rd16(cm + 2);
	/* preference: (3,10) format 12, (3,1)/(0,x) format 4, (3,0) symbol, (1,0) mac */
	int best_score = 0;
	size_t best_off = 0;
	bool best_symbol = false;

	for (uint16_t i = 0; i < ntab; i++) {
		size_t rec = 4 + (size_t)i * 8;

		if (rec + 8 > cmap_len) {
			break;
		}
		uint16_t pid = rd16(cm + rec), eid = rd16(cm + rec + 2);
		uint32_t off = rd32(cm + rec + 4);
		int score = 0;
		bool symbol = false;

		if (pid == 3 && eid == 10) {
			score = 5;
		} else if (pid == 3 && eid == 1) {
			score = 4;
		} else if (pid == 0) {
			score = 3;
		} else if (pid == 3 && eid == 0) {
			score = 2;
			symbol = true;
		} else if (pid == 1 && eid == 0) {
			score = 1;
		}
		if (score > best_score && off < cmap_len) {
			best_score = score;
			best_off = off;
			best_symbol = symbol;
		}
	}
	if (best_score == 0) {
		return 0;
	}
	uint32_t *m = pdf_calloc(nglyphs * sizeof(uint32_t));

	if (m == NULL) {
		return -1;
	}
	const uint8_t *t = cm + best_off;
	size_t tlen = cmap_len - best_off;
	uint16_t format = rd16(t);
	int count;

	if (format == 4) {
		count = parse_format4(t, tlen, m, nglyphs, best_symbol);
	} else if (format == 12) {
		count = parse_format12(t, tlen, m, nglyphs);
	} else if (format == 0) {
		count = parse_format0(t, tlen, m, nglyphs);
	} else {
		count = 0;
	}
	if (count <= 0) {
		pdf_free(m);
		return 0;
	}
	*map = m;
	*n = nglyphs;
	return count;
}

/* ---------------------------------------------------------------------- */
/* glyph outlines                                                         */

int ttf_open(struct ttf_font *f, const uint8_t *data, size_t len)
{
	size_t off, tl;

	memset(f, 0, sizeof(*f));
	f->data = data;
	f->len = len;
	if (len < 12) {
		return -1;
	}
	uint32_t magic = rd32(data);

	if (magic != 0x00010000u && magic != 0x74727565u /* true */ && magic != 0x74746366u &&
	    magic != 0x4F54544Fu /* OTTO */) {
		return -1;
	}
	if (find_table(data, len, "head", &off, &tl) < 0 || tl < 54) {
		return -1;
	}
	f->upem = rd16(data + off + 18);
	if (f->upem == 0) {
		f->upem = 1000;
	}
	f->long_loca = rd16(data + off + 50) != 0;
	if (find_table(data, len, "maxp", &off, &tl) == 0 && tl >= 6) {
		f->nglyphs = rd16(data + off + 4);
	}
	if (find_table32(data, len, "glyf", &f->glyf_off, &f->glyf_len) == 0 &&
	    find_table32(data, len, "loca", &f->loca_off, &f->loca_len) == 0) {
		f->has_glyf = true;
		size_t entries = f->loca_len / (f->long_loca ? 4 : 2);

		if (entries > 0 && entries - 1 < f->nglyphs) {
			f->nglyphs = (uint16_t)(entries - 1);
		}
	}
	if (find_table32(data, len, "cmap", &f->cmap_off, &f->cmap_len) == 0 && f->cmap_len >= 4) {
		const uint8_t *cm = data + f->cmap_off;
		uint16_t ntab = rd16(cm + 2);

		for (uint16_t i = 0; i < ntab; i++) {
			size_t rec = 4 + (size_t)i * 8;

			if (rec + 8 > f->cmap_len) {
				break;
			}
			uint16_t pid = rd16(cm + rec), eid = rd16(cm + rec + 2);
			uint32_t so = rd32(cm + rec + 4);

			if (so >= f->cmap_len) {
				continue;
			}
			if (pid == 3 && eid == 1 && !f->cmap31) {
				f->cmap31 = so;
			} else if (pid == 3 && eid == 0 && !f->cmap30) {
				f->cmap30 = so;
			} else if (pid == 1 && eid == 0 && !f->cmap10) {
				f->cmap10 = so;
			} else if (pid == 3 && eid == 10 && !f->cmap310) {
				f->cmap310 = so;
			} else if (pid == 0 && !f->cmap31) {
				f->cmap31 = so; /* Unicode platform behaves like (3,1) */
			}
		}
	}
	if (find_table32(data, len, "post", &f->post_off, &f->post_len) < 0) {
		f->post_off = 0;
	}
	if (find_table(data, len, "hhea", &off, &tl) == 0 && tl >= 36) {
		f->nhmetrics = rd16(data + off + 34);
	}
	if (find_table32(data, len, "hmtx", &f->hmtx_off, &f->hmtx_len) < 0) {
		f->hmtx_off = 0;
	}
	return 0;
}

static uint16_t cmap_subtable_lookup(const uint8_t *t, size_t tlen, uint32_t code)
{
	if (tlen < 4) {
		return 0;
	}
	uint16_t format = rd16(t);

	if (format == 0) {
		if (code < 256 && tlen >= 6 + 256) {
			return t[6 + code];
		}
		return 0;
	}
	if (format == 4) {
		if (code > 0xFFFF || tlen < 16) {
			return 0;
		}
		size_t segx2 = rd16(t + 6);
		size_t ends = 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;

		if (ranges + segx2 > tlen) {
			return 0;
		}
		for (size_t s = 0; s < segx2 / 2; s++) {
			uint32_t end = rd16(t + ends + s * 2);

			if (code > end) {
				continue;
			}
			uint32_t start = rd16(t + starts + s * 2);

			if (code < start) {
				return 0;
			}
			uint16_t delta = rd16(t + deltas + s * 2);
			uint16_t ro = rd16(t + ranges + s * 2);

			if (ro == 0) {
				return (uint16_t)((code + delta) & 0xFFFF);
			}
			size_t gi = ranges + s * 2 + ro + (code - start) * 2;

			if (gi + 2 > tlen) {
				return 0;
			}
			uint16_t g = rd16(t + gi);

			return g ? (uint16_t)((g + delta) & 0xFFFF) : 0;
		}
		return 0;
	}
	if (format == 6) {
		if (tlen < 10) {
			return 0;
		}
		uint32_t first = rd16(t + 6), count = rd16(t + 8);

		if (code < first || code >= first + count || 10 + (code - first) * 2 + 2 > tlen) {
			return 0;
		}
		return rd16(t + 10 + (code - first) * 2);
	}
	if (format == 12) {
		if (tlen < 16) {
			return 0;
		}
		uint32_t ngroups = rd32(t + 12);

		for (uint32_t g = 0; g < ngroups; g++) {
			size_t o = 16 + (size_t)g * 12;

			if (o + 12 > tlen) {
				break;
			}
			uint32_t sc = rd32(t + o), ec = rd32(t + o + 4), sg = rd32(t + o + 8);

			if (code >= sc && code <= ec) {
				return (uint16_t)(sg + (code - sc));
			}
		}
	}
	return 0;
}

uint16_t ttf_cmap_lookup(const struct ttf_font *f, int which, uint32_t code)
{
	uint32_t so = 0;

	if (which == 31) {
		so = f->cmap31 ? f->cmap31 : f->cmap310;
	} else if (which == 30) {
		so = f->cmap30;
	} else if (which == 10) {
		so = f->cmap10;
	}
	if (so == 0 || f->cmap_off == 0) {
		return 0;
	}
	return cmap_subtable_lookup(f->data + f->cmap_off + so, f->cmap_len - so, code);
}

bool ttf_has_cmap(const struct ttf_font *f, int which)
{
	if (which == 31) {
		return f->cmap31 || f->cmap310;
	}
	if (which == 30) {
		return f->cmap30 != 0;
	}
	if (which == 10) {
		return f->cmap10 != 0;
	}
	return false;
}

uint16_t ttf_advance(const struct ttf_font *f, uint16_t gid)
{
	if (f->hmtx_off == 0 || f->nhmetrics == 0) {
		return 0;
	}
	uint16_t i = gid < f->nhmetrics ? gid : f->nhmetrics - 1;

	if ((size_t)i * 4 + 2 > f->hmtx_len) {
		return 0;
	}
	return rd16(f->data + f->hmtx_off + (size_t)i * 4);
}

/* The 258 standard Macintosh glyph names are referenced by index in post format 2. */
static const char *const mac_glyph_names[258] = {
	".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar",
	"percent", "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma",
	"hyphen", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven",
	"eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at", "A", "B",
	"C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U",
	"V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum",
	"underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
	"o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright",
	"asciitilde", "Adieresis", "Aring", "Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis",
	"aacute", "agrave", "acircumflex", "adieresis", "atilde", "aring", "ccedilla", "eacute",
	"egrave", "ecircumflex", "edieresis", "iacute", "igrave", "icircumflex", "idieresis", "ntilde",
	"oacute", "ograve", "ocircumflex", "odieresis", "otilde", "uacute", "ugrave", "ucircumflex",
	"udieresis", "dagger", "degree", "cent", "sterling", "section", "bullet", "paragraph",
	"germandbls", "registered", "copyright", "trademark", "acute", "dieresis", "notequal", "AE",
	"Oslash", "infinity", "plusminus", "lessequal", "greaterequal", "yen", "mu", "partialdiff",
	"summation", "product", "pi", "integral", "ordfeminine", "ordmasculine", "Omega", "ae",
	"oslash", "questiondown", "exclamdown", "logicalnot", "radical", "florin", "approxequal",
	"Delta", "guillemotleft", "guillemotright", "ellipsis", "nonbreakingspace", "Agrave", "Atilde",
	"Otilde", "OE", "oe", "endash", "emdash", "quotedblleft", "quotedblright", "quoteleft",
	"quoteright", "divide", "lozenge", "ydieresis", "Ydieresis", "fraction", "currency",
	"guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl", "periodcentered", "quotesinglbase",
	"quotedblbase", "perthousand", "Acircumflex", "Ecircumflex", "Aacute", "Edieresis", "Egrave",
	"Iacute", "Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex", "apple", "Ograve",
	"Uacute", "Ucircumflex", "Ugrave", "dotlessi", "circumflex", "tilde", "macron", "breve",
	"dotaccent", "ring", "cedilla", "hungarumlaut", "ogonek", "caron", "Lslash", "lslash",
	"Scaron", "scaron", "Zcaron", "zcaron", "brokenbar", "Eth", "eth", "Yacute", "yacute", "Thorn",
	"thorn", "minus", "multiply", "onesuperior", "twosuperior", "threesuperior", "onehalf",
	"onequarter", "threequarters", "franc", "Gbreve", "gbreve", "Idotaccent", "Scedilla",
	"scedilla", "Cacute", "cacute", "Ccaron", "ccaron", "dcroat"};

uint16_t ttf_post_lookup(const struct ttf_font *f, const char *name)
{
	if (f->post_off == 0 || f->post_len < 34) {
		return 0;
	}
	const uint8_t *p = f->data + f->post_off;
	uint32_t ver = rd32(p);

	if (ver == 0x00010000) {
		for (int i = 0; i < 258; i++) {
			if (strcmp(mac_glyph_names[i], name) == 0) {
				return (uint16_t)i;
			}
		}
		return 0;
	}
	if (ver != 0x00020000) {
		return 0;
	}
	uint16_t num = rd16(p + 32);
	size_t idx_off = 34, names_off = 34 + (size_t)num * 2;

	if (names_off > f->post_len) {
		return 0;
	}
	size_t nlen = strlen(name);

	for (uint16_t g = 0; g < num; g++) {
		uint16_t idx = rd16(p + idx_off + (size_t)g * 2);

		if (idx < 258) {
			if (strcmp(mac_glyph_names[idx], name) == 0) {
				return g;
			}
			continue;
		}
		/* walk the Pascal strings to entry idx - 258 */
		size_t o = names_off;
		uint16_t k = idx - 258;

		while (o < f->post_len) {
			uint8_t l = p[o];

			if (k == 0) {
				if (l == nlen && o + 1 + l <= f->post_len && memcmp(p + o + 1, name, l) == 0) {
					return g;
				}
				break;
			}
			o += 1 + l;
			k--;
		}
	}
	return 0;
}

struct outline_ctx {
	const struct ttf_font *f;
	const struct ttf_outline_cb *cb;
	void *ud;
	int depth;
};

static void xf(const float m[6], float x, float y, float *ox, float *oy)
{
	*ox = m[0] * x + m[2] * y + m[4];
	*oy = m[1] * x + m[3] * y + m[5];
}

static int emit_simple(struct outline_ctx *c, const uint8_t *g, size_t glen, int ncont, const float m[6])
{
	if (glen < 10 + (size_t)ncont * 2 + 2) {
		return -1;
	}
	size_t p = 10;
	uint16_t last_end = 0;

	for (int i = 0; i < ncont; i++) {
		last_end = rd16(g + p);
		p += 2;
	}
	int npts = last_end + 1;

	if (npts <= 0 || npts > 10000) {
		return -1;
	}
	uint16_t ilen = rd16(g + p);

	p += 2 + ilen;
	if (p > glen) {
		return -1;
	}
	uint8_t *flags = pdf_malloc((size_t)npts);
	int16_t *xs = pdf_malloc((size_t)npts * sizeof(int16_t));
	int16_t *ys = pdf_malloc((size_t)npts * sizeof(int16_t));

	if (flags == NULL || xs == NULL || ys == NULL) {
		pdf_free(flags);
		pdf_free(xs);
		pdf_free(ys);
		return -1;
	}
	int ret = -1;

	for (int i = 0; i < npts;) {
		if (p >= glen) {
			goto out;
		}
		uint8_t fl = g[p++];

		flags[i++] = fl;
		if (fl & 8) {
			if (p >= glen) {
				goto out;
			}
			uint8_t rep = g[p++];

			while (rep-- && i < npts) {
				flags[i++] = fl;
			}
		}
	}
	int16_t v = 0;

	for (int i = 0; i < npts; i++) {
		uint8_t fl = flags[i];

		if (fl & 2) {
			if (p >= glen) {
				goto out;
			}
			int16_t d = g[p++];

			v += (fl & 16) ? d : -d;
		} else if (!(fl & 16)) {
			if (p + 2 > glen) {
				goto out;
			}
			v += (int16_t)rd16(g + p);
			p += 2;
		}
		xs[i] = v;
	}
	v = 0;
	for (int i = 0; i < npts; i++) {
		uint8_t fl = flags[i];

		if (fl & 4) {
			if (p >= glen) {
				goto out;
			}
			int16_t d = g[p++];

			v += (fl & 32) ? d : -d;
		} else if (!(fl & 32)) {
			if (p + 2 > glen) {
				goto out;
			}
			v += (int16_t)rd16(g + p);
			p += 2;
		}
		ys[i] = v;
	}
	/* walk the contours, converting on/off curve points to quads */
	int start = 0;

	for (int ci = 0; ci < ncont; ci++) {
		int end = rd16(g + 10 + ci * 2);

		if (end < start || end >= npts) {
			break;
		}
		int n = end - start + 1;

		if (n <= 0) {
			start = end + 1;
			continue;
		}
#define PX(k) ((float)xs[start + ((k) % n)])
#define PY(k) ((float)ys[start + ((k) % n)])
#define ON(k) ((flags[start + ((k) % n)] & 1) != 0)
		/* find a starting on-curve point (or synthesize one) */
		int s = -1;

		for (int k = 0; k < n; k++) {
			if (ON(k)) {
				s = k;
				break;
			}
		}
		float sx, sy;

		if (s < 0) {
			/* all off-curve: start at midpoint of the first two */
			sx = (PX(0) + PX(1)) * 0.5f;
			sy = (PY(0) + PY(1)) * 0.5f;
			s = 0;
		} else {
			sx = PX(s);
			sy = PY(s);
		}
		float ox, oy;

		xf(m, sx, sy, &ox, &oy);
		c->cb->move(c->ud, ox, oy);
		float prev_cx = 0, prev_cy = 0;
		bool have_ctrl = false;
		int k0 = (ON(s)) ? s + 1 : s;

		for (int k = k0; k <= s + n; k++) {
			float px = PX(k), py = PY(k);
			bool on = ON(k);

			if (k == s + n) {
				/* back to start */
				px = sx;
				py = sy;
				on = true;
			}
			if (on) {
				if (have_ctrl) {
					float cx, cy, ex, ey;

					xf(m, prev_cx, prev_cy, &cx, &cy);
					xf(m, px, py, &ex, &ey);
					c->cb->quad(c->ud, cx, cy, ex, ey);
					have_ctrl = false;
				} else {
					xf(m, px, py, &ox, &oy);
					c->cb->line(c->ud, ox, oy);
				}
			} else {
				if (have_ctrl) {
					float mx = (prev_cx + px) * 0.5f, my = (prev_cy + py) * 0.5f;
					float cx, cy, ex, ey;

					xf(m, prev_cx, prev_cy, &cx, &cy);
					xf(m, mx, my, &ex, &ey);
					c->cb->quad(c->ud, cx, cy, ex, ey);
				}
				prev_cx = px;
				prev_cy = py;
				have_ctrl = true;
			}
		}
#undef PX
#undef PY
#undef ON
		c->cb->close(c->ud);
		start = end + 1;
	}
	ret = 0;
out:
	pdf_free(flags);
	pdf_free(xs);
	pdf_free(ys);
	return ret;
}

static int emit_glyph(struct outline_ctx *c, uint16_t gid, const float m[6]);

static int emit_composite(struct outline_ctx *c, const uint8_t *g, size_t glen, const float m[6])
{
	size_t p = 10;

	for (;;) {
		if (p + 4 > glen) {
			return -1;
		}
		uint16_t flags = rd16(g + p), gi = rd16(g + p + 2);

		p += 4;
		float dx, dy;

		if (flags & 1) {
			if (p + 4 > glen) {
				return -1;
			}
			dx = (int16_t)rd16(g + p);
			dy = (int16_t)rd16(g + p + 2);
			p += 4;
		} else {
			if (p + 2 > glen) {
				return -1;
			}
			dx = (int8_t)g[p];
			dy = (int8_t)g[p + 1];
			p += 2;
		}
		float a = 1, b = 0, cc = 0, d = 1;

		if (flags & 8) {
			if (p + 2 > glen) {
				return -1;
			}
			a = d = (int16_t)rd16(g + p) / 16384.0f;
			p += 2;
		} else if (flags & 0x40) {
			if (p + 4 > glen) {
				return -1;
			}
			a = (int16_t)rd16(g + p) / 16384.0f;
			d = (int16_t)rd16(g + p + 2) / 16384.0f;
			p += 4;
		} else if (flags & 0x80) {
			if (p + 8 > glen) {
				return -1;
			}
			a = (int16_t)rd16(g + p) / 16384.0f;
			b = (int16_t)rd16(g + p + 2) / 16384.0f;
			cc = (int16_t)rd16(g + p + 4) / 16384.0f;
			d = (int16_t)rd16(g + p + 6) / 16384.0f;
			p += 8;
		}
		if (!(flags & 2)) {
			/* point matching: not supported, treat as zero offset */
			dx = dy = 0;
		}
		/* component matrix (a b c d dx dy) times m */
		float cm[6];

		cm[0] = a * m[0] + b * m[2];
		cm[1] = a * m[1] + b * m[3];
		cm[2] = cc * m[0] + d * m[2];
		cm[3] = cc * m[1] + d * m[3];
		cm[4] = dx * m[0] + dy * m[2] + m[4];
		cm[5] = dx * m[1] + dy * m[3] + m[5];
		emit_glyph(c, gi, cm);
		if (!(flags & 0x20)) {
			break;
		}
	}
	return 0;
}

static int emit_glyph(struct outline_ctx *c, uint16_t gid, const float m[6])
{
	const struct ttf_font *f = c->f;

	if (!f->has_glyf || gid >= f->nglyphs || c->depth > 8) {
		return -1;
	}
	uint32_t a, b;

	if (f->long_loca) {
		if ((size_t)gid * 4 + 8 > f->loca_len) {
			return -1;
		}
		a = rd32(f->data + f->loca_off + (size_t)gid * 4);
		b = rd32(f->data + f->loca_off + (size_t)gid * 4 + 4);
	} else {
		if ((size_t)gid * 2 + 4 > f->loca_len) {
			return -1;
		}
		a = rd16(f->data + f->loca_off + (size_t)gid * 2) * 2u;
		b = rd16(f->data + f->loca_off + (size_t)gid * 2 + 2) * 2u;
	}
	if (b <= a || b > f->glyf_len) {
		return 0; /* empty glyph */
	}
	const uint8_t *g = f->data + f->glyf_off + a;
	size_t glen = b - a;

	if (glen < 10) {
		return 0;
	}
	int ncont = (int16_t)rd16(g);

	c->depth++;
	int ret = ncont >= 0 ? emit_simple(c, g, glen, ncont, m) : emit_composite(c, g, glen, m);

	c->depth--;
	return ret;
}

int ttf_glyph_outline(const struct ttf_font *f, uint16_t gid, const float m[6],
		      const struct ttf_outline_cb *cb, void *ud)
{
	struct outline_ctx c = {.f = f, .cb = cb, .ud = ud, .depth = 0};

	return emit_glyph(&c, gid, m);
}
