/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Document layer: cross-reference tables and streams (section 7.5),
 * object streams (7.5.7), stream filters (7.4) and the page tree (7.7.3).
 * Files with broken offsets are rebuilt by scanning for "n g obj".
 */
#include <zpdf/pdf_doc.h>
#include "pdf_lex.h"
#include "pdf_mem.h"
#include "pdf_filter.h"
#include "pdf_util.h"
#include "inflate.h"
#include <string.h>

PDF_LOG_REGISTER();

#ifndef CONFIG_ZPDF_MAX_PAGES
#define CONFIG_ZPDF_MAX_PAGES 4096
#endif

#define XREF_MAX_OBJECTS (1u << 22)
#define OBJSTM_CACHE 4
#define XREF_UNSET 0xFF

struct xref_entry {
	uint8_t type; /* XREF_UNSET, 0 free, 1 at offset, 2 inside object stream */
	uint32_t off; /* file offset, or the object stream number for type 2 */
	uint32_t idx; /* generation, or index inside the object stream */
};

struct objstm {
	uint32_t num;
	uint8_t *data;
	size_t len;
	int n;
	size_t first;
};

struct font_cache_entry {
	const struct pdf_obj *key;
	void *font;
};

struct aux_entry {
	const void *key;
	void *data;
	size_t size;
	void (*free_fn)(void *);
	int refs;
	uint32_t stamp;
};

#ifndef CONFIG_ZPDF_AUX_CACHE_BYTES
#define CONFIG_ZPDF_AUX_CACHE_BYTES (8u << 20)
#endif

struct pdf_doc {
	const uint8_t *buf;
	size_t len;
	size_t hdr_off;

	struct xref_entry *xref;
	size_t xref_n;
	struct pdf_obj **cache;
	uint8_t *parsing;
	struct pdf_obj *trailer;

	struct pdf_obj **pages;
	size_t npages;

	bool encrypted;
	bool reconstructed;

	struct objstm ostm[OBJSTM_CACHE];
	int ostm_next;

	struct font_cache_entry *fonts;
	size_t nfonts, fonts_cap;

	struct aux_entry *aux;
	size_t naux, aux_cap, aux_bytes;
	uint32_t aux_stamp;
};

void pdf_font_free_cb(void *font); /* provided by pdf_font.c */

/* ---------------------------------------------------------------------- */
/* xref storage                                                           */

static int xref_reserve(struct pdf_doc *doc, size_t n)
{
	if (n <= doc->xref_n) {
		return 0;
	}
	if (n > XREF_MAX_OBJECTS) {
		return -1;
	}
	struct xref_entry *x = pdf_realloc(doc->xref, n * sizeof(*x));
	struct pdf_obj **c = x ? pdf_realloc(doc->cache, n * sizeof(*c)) : NULL;
	uint8_t *p = c ? pdf_realloc(doc->parsing, n) : NULL;

	if (x != NULL) {
		doc->xref = x;
	}
	if (c != NULL) {
		doc->cache = c;
	}
	if (p != NULL) {
		doc->parsing = p;
	}
	if (x == NULL || c == NULL || p == NULL) {
		return -1;
	}
	for (size_t i = doc->xref_n; i < n; i++) {
		doc->xref[i].type = XREF_UNSET;
		doc->xref[i].off = 0;
		doc->xref[i].idx = 0;
		doc->cache[i] = NULL;
		doc->parsing[i] = 0;
	}
	doc->xref_n = n;
	return 0;
}

static void xref_set(struct pdf_doc *doc, uint32_t num, uint8_t type, uint32_t off, uint32_t idx,
		     bool overwrite)
{
	if (num >= doc->xref_n) {
		size_t n = doc->xref_n ? doc->xref_n : 64;

		while (n <= num) {
			n *= 2;
		}
		if (xref_reserve(doc, n) < 0) {
			return;
		}
	}
	if (!overwrite && doc->xref[num].type != XREF_UNSET) {
		return;
	}
	doc->xref[num].type = type;
	doc->xref[num].off = off;
	doc->xref[num].idx = idx;
}

/* ---------------------------------------------------------------------- */
/* helpers                                                                */

static bool match_at(const struct pdf_doc *doc, size_t pos, const char *kw)
{
	size_t n = strlen(kw);

	return pos + n <= doc->len && memcmp(doc->buf + pos, kw, n) == 0;
}

static long find_fwd(const struct pdf_doc *doc, size_t from, size_t limit, const char *kw)
{
	size_t n = strlen(kw);

	if (limit > doc->len) {
		limit = doc->len;
	}
	for (size_t i = from; i + n <= limit; i++) {
		if (doc->buf[i] == (uint8_t)kw[0] && memcmp(doc->buf + i, kw, n) == 0) {
			return (long)i;
		}
	}
	return -1;
}

static long find_back(const struct pdf_doc *doc, size_t from, size_t min, const char *kw)
{
	size_t n = strlen(kw);

	if (from + n > doc->len) {
		from = doc->len - n;
	}
	for (size_t i = from + 1; i-- > min;) {
		if (doc->buf[i] == (uint8_t)kw[0] && memcmp(doc->buf + i, kw, n) == 0) {
			return (long)i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------------- */
/* indirect objects                                                       */

static size_t stream_length(struct pdf_doc *doc, struct pdf_obj *dict, size_t start)
{
	struct pdf_obj *lo = pdf_dict_get_raw(dict, "Length");
	int64_t len = -1;

	if (lo != NULL) {
		lo = pdf_resolve(doc, lo);
		if (lo != NULL && lo->type == PDF_INT) {
			len = lo->i;
		}
	}
	if (len >= 0 && start + (size_t)len <= doc->len) {
		struct pdf_lex lx;

		pdf_lex_init(&lx, doc->buf, doc->len, start + (size_t)len);
		pdf_lex_skip_ws(&lx);
		if (match_at(doc, lx.pos, "endstream")) {
			return (size_t)len;
		}
	}
	/* /Length missing or wrong: look for the endstream keyword */
	long e = find_fwd(doc, start, doc->len, "endstream");

	if (e < 0) {
		e = find_fwd(doc, start, doc->len, "endobj");
		if (e < 0) {
			return doc->len - start;
		}
	}
	size_t end = (size_t)e;

	if (end > start && doc->buf[end - 1] == '\n') {
		end--;
	}
	if (end > start && doc->buf[end - 1] == '\r') {
		end--;
	}
	return end - start;
}

/*
 * Parse "num gen obj <object> [stream...] endobj" at off. num/gen receive the
 * header values. Returns the object (caller owns it) or NULL.
 */
static struct pdf_obj *parse_indirect_at(struct pdf_doc *doc, size_t off, uint32_t *num,
					 uint32_t *gen)
{
	struct pdf_lex lx;
	struct pdf_tok t1, t2, t3;

	if (off >= doc->len) {
		return NULL;
	}
	pdf_lex_init(&lx, doc->buf, doc->len, off);
	if (pdf_lex_next(&lx, &t1) < 0 || t1.type != TOK_NUM || !t1.is_int) {
		pdf_tok_free(&t1);
		return NULL;
	}
	if (pdf_lex_next(&lx, &t2) < 0 || t2.type != TOK_NUM || !t2.is_int) {
		pdf_tok_free(&t2);
		return NULL;
	}
	if (pdf_lex_next(&lx, &t3) < 0 || t3.type != TOK_KEYWORD || t3.len != 3 ||
	    memcmp(t3.start, "obj", 3) != 0) {
		pdf_tok_free(&t3);
		return NULL;
	}
	*num = (uint32_t)t1.ival;
	*gen = (uint32_t)t2.ival;

	struct pdf_obj *o = pdf_parse_obj(&lx, true, 0);

	if (o == NULL) {
		return NULL;
	}
	if (o->type == PDF_OP) {
		/* "endobj" right away: an empty object */
		pdf_obj_free(o);
		return pdf_obj_new(PDF_NULL);
	}
	if (o->type == PDF_DICT) {
		pdf_lex_skip_ws(&lx);
		if (match_at(doc, lx.pos, "stream")) {
			size_t p = lx.pos + 6;

			if (p < doc->len && doc->buf[p] == '\r') {
				p++;
			}
			if (p < doc->len && doc->buf[p] == '\n') {
				p++;
			}
			struct pdf_obj *s = pdf_obj_new(PDF_STREAM);

			if (s == NULL) {
				pdf_obj_free(o);
				return NULL;
			}
			s->stream.dict = o;
			s->stream.data = doc->buf + p;
			s->stream.len = stream_length(doc, o, p);
			return s;
		}
	}
	return o;
}

static void reconstruct(struct pdf_doc *doc);

static struct pdf_obj *load_from_objstm(struct pdf_doc *doc, uint32_t stm_num, uint32_t idx,
					uint32_t want);

static struct pdf_obj *load_object(struct pdf_doc *doc, uint32_t num)
{
	if (num >= doc->xref_n) {
		return NULL;
	}
	struct xref_entry *e = &doc->xref[num];

	if (e->type == 1) {
		uint32_t n = 0, g = 0;
		struct pdf_obj *o = parse_indirect_at(doc, e->off, &n, &g);

		if (o != NULL && n == num) {
			return o;
		}
		pdf_obj_free(o);
		if (doc->hdr_off != 0) {
			o = parse_indirect_at(doc, e->off + doc->hdr_off, &n, &g);
			if (o != NULL && n == num) {
				return o;
			}
			pdf_obj_free(o);
		}
		if (!doc->reconstructed) {
			PDF_WRN("object %u not at offset %u, rebuilding xref", num, e->off);
			reconstruct(doc);
			e = &doc->xref[num];
			if (e->type == 1) {
				o = parse_indirect_at(doc, e->off, &n, &g);
				if (o != NULL && n == num) {
					return o;
				}
				pdf_obj_free(o);
			} else if (e->type == 2) {
				return load_from_objstm(doc, e->off, e->idx, num);
			}
		}
		return NULL;
	}
	if (e->type == 2) {
		return load_from_objstm(doc, e->off, e->idx, num);
	}
	return NULL;
}

struct pdf_obj *pdf_resolve(struct pdf_doc *doc, struct pdf_obj *o)
{
	int hops = 0;

	while (o != NULL && o->type == PDF_REF && hops++ < 32) {
		uint32_t num = o->ref.num;

		if (num >= doc->xref_n) {
			return NULL;
		}
		if (doc->cache[num] != NULL) {
			o = doc->cache[num];
			continue;
		}
		if (doc->parsing[num]) {
			return NULL; /* reference cycle */
		}
		doc->parsing[num] = 1;
		struct pdf_obj *loaded = load_object(doc, num);

		doc->parsing[num] = 0;
		if (loaded == NULL) {
			return NULL;
		}
		if (doc->cache[num] != NULL) {
			/* reconstruction may have filled it meanwhile */
			pdf_obj_free(loaded);
		} else {
			doc->cache[num] = loaded;
		}
		o = doc->cache[num];
	}
	return o;
}

struct pdf_obj *pdf_get(struct pdf_doc *doc, struct pdf_obj *dict, const char *key)
{
	return pdf_resolve(doc, pdf_dict_get_raw(dict, key));
}

struct pdf_obj *pdf_array_get(struct pdf_doc *doc, struct pdf_obj *arr, size_t i)
{
	return pdf_resolve(doc, pdf_array_get_raw(arr, i));
}

/* ---------------------------------------------------------------------- */
/* object streams                                                         */

static struct objstm *objstm_get(struct pdf_doc *doc, uint32_t stm_num)
{
	for (int i = 0; i < OBJSTM_CACHE; i++) {
		if (doc->ostm[i].data != NULL && doc->ostm[i].num == stm_num) {
			return &doc->ostm[i];
		}
	}
	struct pdf_obj ref = {.type = PDF_REF, .ref = {stm_num, 0}};
	struct pdf_obj *s = pdf_resolve(doc, &ref);

	if (s == NULL || s->type != PDF_STREAM) {
		return NULL;
	}
	uint8_t *data;
	size_t len;

	if (pdf_stream_decode(doc, s, &data, &len) < 0) {
		return NULL;
	}
	struct objstm *c = &doc->ostm[doc->ostm_next];

	doc->ostm_next = (doc->ostm_next + 1) % OBJSTM_CACHE;
	pdf_free(c->data);
	c->num = stm_num;
	c->data = data;
	c->len = len;
	c->n = (int)pdf_int(pdf_get(doc, s, "N"));
	c->first = (size_t)pdf_int(pdf_get(doc, s, "First"));
	if (c->n < 0) {
		c->n = 0;
	}
	return c;
}

static struct pdf_obj *load_from_objstm(struct pdf_doc *doc, uint32_t stm_num, uint32_t idx,
					uint32_t want)
{
	struct objstm *c = objstm_get(doc, stm_num);

	if (c == NULL) {
		return NULL;
	}
	struct pdf_lex lx;
	struct pdf_tok a, b;
	size_t found_off = 0;
	bool found = false;

	pdf_lex_init(&lx, c->data, c->len, 0);
	for (int i = 0; i < c->n; i++) {
		if (pdf_lex_next(&lx, &a) < 0 || a.type != TOK_NUM) {
			pdf_tok_free(&a);
			break;
		}
		if (pdf_lex_next(&lx, &b) < 0 || b.type != TOK_NUM) {
			pdf_tok_free(&b);
			break;
		}
		if ((uint32_t)a.ival == want) {
			found = true;
			found_off = (size_t)b.ival;
			break;
		}
	}
	(void)idx; /* objects are located by number, the index is only a hint */
	if (!found) {
		return NULL;
	}
	if (c->first + found_off >= c->len) {
		return NULL;
	}
	pdf_lex_init(&lx, c->data, c->len, c->first + found_off);
	return pdf_parse_obj(&lx, true, 0);
}

/* ---------------------------------------------------------------------- */
/* stream decoding                                                        */

static int apply_one_filter(struct pdf_doc *doc, const char *name, struct pdf_obj *parms,
			    uint8_t **data, size_t *len, bool *own)
{
	uint8_t *out = NULL;
	size_t out_len = 0;
	int rc;

	if (strcmp(name, "FlateDecode") == 0 || strcmp(name, "Fl") == 0) {
		rc = inflate_buf(*data, *len, &out, &out_len);
		if (rc < 0) {
			return -1;
		}
	} else if (strcmp(name, "LZWDecode") == 0 || strcmp(name, "LZW") == 0) {
		int early = 1;
		struct pdf_obj *ec = pdf_get(doc, parms, "EarlyChange");

		if (ec != NULL && ec->type == PDF_INT) {
			early = (int)ec->i;
		}
		rc = pdf_lzw_decode(*data, *len, early, &out, &out_len);
	} else if (strcmp(name, "ASCII85Decode") == 0 || strcmp(name, "A85") == 0) {
		rc = pdf_a85_decode(*data, *len, &out, &out_len);
	} else if (strcmp(name, "ASCIIHexDecode") == 0 || strcmp(name, "AHx") == 0) {
		rc = pdf_ahx_decode(*data, *len, &out, &out_len);
	} else if (strcmp(name, "RunLengthDecode") == 0 || strcmp(name, "RL") == 0) {
		rc = pdf_rl_decode(*data, *len, &out, &out_len);
	} else if (strcmp(name, "Crypt") == 0) {
		return 0; /* identity crypt filter */
	} else {
		return 1; /* image codec: leave as is */
	}
	if (rc < 0) {
		return -1;
	}
	if (parms != NULL && parms->type == PDF_DICT) {
		int pred = (int)pdf_int(pdf_get(doc, parms, "Predictor"));

		if (pred > 1) {
			int colors = (int)pdf_int(pdf_get(doc, parms, "Colors"));
			int bpc = (int)pdf_int(pdf_get(doc, parms, "BitsPerComponent"));
			int cols = (int)pdf_int(pdf_get(doc, parms, "Columns"));

			out_len = pdf_apply_predictor(out, out_len, pred, colors ? colors : 1,
						      bpc ? bpc : 8, cols ? cols : 1);
		}
	}
	if (*own) {
		pdf_free(*data);
	}
	*data = out;
	*len = out_len;
	*own = true;
	return 0;
}

int pdf_stream_decode(struct pdf_doc *doc, struct pdf_obj *stream, uint8_t **out, size_t *out_len)
{
	*out = NULL;
	*out_len = 0;
	if (stream == NULL || stream->type != PDF_STREAM) {
		return -1;
	}
	uint8_t *data = (uint8_t *)stream->stream.data;
	size_t len = stream->stream.len;
	bool own = false;
	int ret = 0;

	struct pdf_obj *filter = pdf_get(doc, stream, "Filter");
	struct pdf_obj *parms = pdf_get(doc, stream, "DecodeParms");

	if (parms == NULL) {
		parms = pdf_get(doc, stream, "DP");
	}
	if (filter == NULL) {
		filter = pdf_get(doc, stream, "F");
		if (filter != NULL && filter->type != PDF_NAME && filter->type != PDF_ARRAY) {
			filter = NULL; /* /F is also the file specification key */
		}
	}
	if (filter != NULL && filter->type == PDF_NAME) {
		struct pdf_obj *p = (parms != NULL && parms->type == PDF_ARRAY)
					    ? pdf_array_get(doc, parms, 0)
					    : parms;

		ret = apply_one_filter(doc, pdf_name_str(filter), p, &data, &len, &own);
	} else if (filter != NULL && filter->type == PDF_ARRAY) {
		for (size_t i = 0; i < pdf_array_len(filter); i++) {
			struct pdf_obj *f = pdf_array_get(doc, filter, i);
			struct pdf_obj *p = NULL;

			if (parms != NULL && parms->type == PDF_ARRAY) {
				p = pdf_array_get(doc, parms, i);
			} else if (parms != NULL && pdf_array_len(filter) == 1) {
				p = parms;
			}
			if (f == NULL || f->type != PDF_NAME) {
				continue;
			}
			ret = apply_one_filter(doc, pdf_name_str(f), p, &data, &len, &own);
			if (ret != 0) {
				break;
			}
		}
	}
	if (ret < 0) {
		if (own) {
			pdf_free(data);
		}
		return -1;
	}
	if (!own) {
		uint8_t *copy = pdf_malloc(len + 1);

		if (copy == NULL) {
			return -1;
		}
		memcpy(copy, data, len);
		copy[len] = 0;
		data = copy;
	}
	*out = data;
	*out_len = len;
	return ret;
}

int pdf_page_content(struct pdf_doc *doc, struct pdf_obj *page, uint8_t **out, size_t *out_len)
{
	struct pdf_obj *c = pdf_get(doc, page, "Contents");

	*out = NULL;
	*out_len = 0;
	if (c == NULL) {
		return -1;
	}
	if (c->type == PDF_STREAM) {
		int rc = pdf_stream_decode(doc, c, out, out_len);

		return rc < 0 ? rc : 0;
	}
	if (c->type != PDF_ARRAY) {
		return -1;
	}
	uint8_t *all = NULL;
	size_t all_len = 0;

	for (size_t i = 0; i < pdf_array_len(c); i++) {
		struct pdf_obj *s = pdf_array_get(doc, c, i);
		uint8_t *part;
		size_t part_len;

		if (s == NULL || s->type != PDF_STREAM) {
			continue;
		}
		if (pdf_stream_decode(doc, s, &part, &part_len) < 0) {
			continue;
		}
		uint8_t *n = pdf_realloc(all, all_len + part_len + 2);

		if (n == NULL) {
			pdf_free(part);
			break;
		}
		all = n;
		memcpy(all + all_len, part, part_len);
		all_len += part_len;
		all[all_len++] = '\n';
		all[all_len] = 0;
		pdf_free(part);
	}
	if (all == NULL) {
		return -1;
	}
	*out = all;
	*out_len = all_len;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* cross reference parsing                                                */

static void merge_trailer(struct pdf_doc *doc, struct pdf_obj *t)
{
	static const char *const keys[] = {"Root", "Info", "Encrypt", "ID", "Size"};

	if (t == NULL || t->type != PDF_DICT) {
		return;
	}
	if (doc->trailer == NULL) {
		doc->trailer = pdf_obj_new(PDF_DICT);
		if (doc->trailer == NULL) {
			return;
		}
	}
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		struct pdf_obj *v = pdf_dict_get_raw(t, keys[i]);

		if (v != NULL && pdf_dict_get_raw(doc->trailer, keys[i]) == NULL) {
			/* move the value over */
			for (size_t k = 0; k < t->dict.n; k++) {
				if (t->dict.items[k].val == v) {
					t->dict.items[k].val = pdf_obj_new(PDF_NULL);
					break;
				}
			}
			pdf_dict_put(doc->trailer, keys[i], v);
		}
	}
}

static uint64_t read_be(const uint8_t *p, int w)
{
	uint64_t v = 0;

	for (int i = 0; i < w; i++) {
		v = (v << 8) | p[i];
	}
	return v;
}

/* Returns the /Prev offset to continue with, or -1. Fills *xrefstm for hybrids. */
static long parse_xref_stream(struct pdf_doc *doc, size_t off, struct pdf_obj **trailer_out)
{
	uint32_t num, gen;
	struct pdf_obj *s = parse_indirect_at(doc, off, &num, &gen);

	*trailer_out = NULL;
	if (s == NULL || s->type != PDF_STREAM) {
		pdf_obj_free(s);
		return -2;
	}
	struct pdf_obj *w = pdf_get(doc, s, "W");
	int wd[3] = {1, 1, 1};

	if (w == NULL || w->type != PDF_ARRAY || w->arr.n < 3) {
		pdf_obj_free(s);
		return -2;
	}
	for (int i = 0; i < 3; i++) {
		wd[i] = (int)pdf_int(pdf_array_get(doc, w, i));
		if (wd[i] < 0 || wd[i] > 8) {
			pdf_obj_free(s);
			return -2;
		}
	}
	int64_t size = pdf_int(pdf_get(doc, s, "Size"));

	if (size > 0 && size < XREF_MAX_OBJECTS) {
		xref_reserve(doc, (size_t)size);
	}
	uint8_t *data;
	size_t len;

	if (pdf_stream_decode(doc, s, &data, &len) < 0) {
		pdf_obj_free(s);
		return -2;
	}
	struct pdf_obj *index = pdf_get(doc, s, "Index");
	size_t entry = (size_t)(wd[0] + wd[1] + wd[2]);
	size_t pos = 0;
	size_t nranges = (index != NULL && index->type == PDF_ARRAY) ? index->arr.n / 2 : 1;

	for (size_t r = 0; r < nranges && entry > 0; r++) {
		int64_t start = 0, count = size;

		if (index != NULL && index->type == PDF_ARRAY) {
			start = pdf_int(pdf_array_get(doc, index, r * 2));
			count = pdf_int(pdf_array_get(doc, index, r * 2 + 1));
		}
		for (int64_t i = 0; i < count && pos + entry <= len; i++, pos += entry) {
			uint64_t type = wd[0] ? read_be(data + pos, wd[0]) : 1;
			uint64_t f2 = read_be(data + pos + wd[0], wd[1]);
			uint64_t f3 = read_be(data + pos + wd[0] + wd[1], wd[2]);
			int64_t onum = start + i;

			if (onum < 0 || onum >= XREF_MAX_OBJECTS || type > 2) {
				continue;
			}
			xref_set(doc, (uint32_t)onum, (uint8_t)type, (uint32_t)f2, (uint32_t)f3,
				 false);
		}
	}
	pdf_free(data);
	long prev = -1;
	struct pdf_obj *p = pdf_get(doc, s, "Prev");

	if (p != NULL && p->type == PDF_INT) {
		prev = (long)p->i;
	}
	/* Hand the dictionary to the caller as the trailer. */
	*trailer_out = s->stream.dict;
	s->stream.dict = NULL;
	pdf_obj_free(s);
	return prev;
}

static long parse_xref_table(struct pdf_doc *doc, size_t off, struct pdf_obj **trailer_out,
			     long *xrefstm)
{
	struct pdf_lex lx;
	struct pdf_tok t;

	*trailer_out = NULL;
	*xrefstm = -1;
	pdf_lex_init(&lx, doc->buf, doc->len, off + 4);
	for (;;) {
		pdf_lex_skip_ws(&lx);
		if (match_at(doc, lx.pos, "trailer")) {
			lx.pos += 7;
			struct pdf_obj *tr = pdf_parse_obj(&lx, true, 0);

			if (tr != NULL && tr->type != PDF_DICT) {
				pdf_obj_free(tr);
				tr = NULL;
			}
			*trailer_out = tr;
			break;
		}
		if (pdf_lex_next(&lx, &t) < 0 || t.type != TOK_NUM) {
			pdf_tok_free(&t);
			break;
		}
		int64_t start = t.ival;

		if (pdf_lex_next(&lx, &t) < 0 || t.type != TOK_NUM) {
			pdf_tok_free(&t);
			break;
		}
		int64_t count = t.ival;

		if (start < 0 || count < 0 || start + count > XREF_MAX_OBJECTS) {
			break;
		}
		xref_reserve(doc, (size_t)(start + count));
		for (int64_t i = 0; i < count; i++) {
			pdf_lex_skip_ws(&lx);
			if (match_at(doc, lx.pos, "trailer")) {
				break;
			}
			if (pdf_lex_next(&lx, &t) < 0 || t.type != TOK_NUM) {
				pdf_tok_free(&t);
				goto out;
			}
			int64_t eoff = t.ival;

			if (pdf_lex_next(&lx, &t) < 0 || t.type != TOK_NUM) {
				pdf_tok_free(&t);
				goto out;
			}
			int64_t egen = t.ival;

			if (pdf_lex_next(&lx, &t) < 0 || t.type != TOK_KEYWORD) {
				pdf_tok_free(&t);
				goto out;
			}
			uint8_t kind = t.start[0];

			if (kind == 'n') {
				xref_set(doc, (uint32_t)(start + i), 1, (uint32_t)eoff, (uint32_t)egen,
					 false);
			} else if (kind == 'f') {
				xref_set(doc, (uint32_t)(start + i), 0, 0, 0, false);
			}
		}
	}
out:
	if (*trailer_out == NULL) {
		return -2;
	}
	long prev = -1;
	struct pdf_obj *p = pdf_dict_get_raw(*trailer_out, "Prev");
	struct pdf_obj *xs = pdf_dict_get_raw(*trailer_out, "XRefStm");

	if (p != NULL && p->type == PDF_INT) {
		prev = (long)p->i;
	}
	if (xs != NULL && xs->type == PDF_INT) {
		*xrefstm = (long)xs->i;
	}
	return prev;
}

static int parse_xref_chain(struct pdf_doc *doc, long off)
{
	long visited[64];
	int nvisited = 0;
	int ok = 0;

	while (off >= 0 && (size_t)off < doc->len && nvisited < 64) {
		for (int i = 0; i < nvisited; i++) {
			if (visited[i] == off) {
				return ok;
			}
		}
		visited[nvisited++] = off;

		struct pdf_lex lx;

		pdf_lex_init(&lx, doc->buf, doc->len, (size_t)off);
		pdf_lex_skip_ws(&lx);
		struct pdf_obj *trailer = NULL;
		long next;

		if (match_at(doc, lx.pos, "xref")) {
			long xrefstm;

			next = parse_xref_table(doc, lx.pos, &trailer, &xrefstm);
			if (trailer != NULL) {
				merge_trailer(doc, trailer);
				pdf_obj_free(trailer);
				ok = 1;
			}
			if (xrefstm >= 0) {
				struct pdf_obj *t2 = NULL;

				parse_xref_stream(doc, (size_t)xrefstm, &t2);
				pdf_obj_free(t2);
			}
		} else {
			next = parse_xref_stream(doc, lx.pos, &trailer);
			if (trailer != NULL) {
				merge_trailer(doc, trailer);
				pdf_obj_free(trailer);
				ok = 1;
			}
		}
		if (next == -2 && !ok) {
			return -1;
		}
		off = next;
	}
	return ok ? 0 : -1;
}

/* ---------------------------------------------------------------------- */
/* reconstruction of damaged files                                        */

static bool is_digit(uint8_t c)
{
	return c >= '0' && c <= '9';
}

static void register_objstm_members(struct pdf_doc *doc, uint32_t stm_num)
{
	struct objstm *c = objstm_get(doc, stm_num);

	if (c == NULL) {
		return;
	}
	struct pdf_lex lx;
	struct pdf_tok a, b;

	pdf_lex_init(&lx, c->data, c->len, 0);
	for (int i = 0; i < c->n; i++) {
		if (pdf_lex_next(&lx, &a) < 0 || a.type != TOK_NUM) {
			pdf_tok_free(&a);
			break;
		}
		if (pdf_lex_next(&lx, &b) < 0 || b.type != TOK_NUM) {
			pdf_tok_free(&b);
			break;
		}
		if (a.ival < 0 || a.ival >= XREF_MAX_OBJECTS) {
			continue;
		}
		uint32_t onum = (uint32_t)a.ival;

		if (onum < doc->xref_n && doc->xref[onum].type == 1) {
			continue; /* a directly stored object wins */
		}
		xref_set(doc, onum, 2, stm_num, (uint32_t)i, true);
	}
}

static void reconstruct(struct pdf_doc *doc)
{
	doc->reconstructed = true;
	PDF_WRN("rebuilding cross reference by scanning");

	for (size_t i = 0; i < doc->xref_n; i++) {
		doc->xref[i].type = XREF_UNSET;
	}
	struct pdf_obj *found_trailer = NULL;
	long root_num = -1;

	for (size_t i = 0; i + 3 <= doc->len; i++) {
		if (doc->buf[i] != 'o') {
			continue;
		}
		if (memcmp(doc->buf + i, "obj", 3) != 0) {
			continue;
		}
		if (i + 3 < doc->len && !pdf_is_white(doc->buf[i + 3]) && !pdf_is_delim(doc->buf[i + 3])) {
			continue;
		}
		size_t p = i;

		if (p == 0 || !pdf_is_white(doc->buf[p - 1])) {
			continue;
		}
		p--;
		while (p > 0 && pdf_is_white(doc->buf[p])) {
			p--;
		}
		if (!is_digit(doc->buf[p])) {
			continue;
		}
		while (p > 0 && is_digit(doc->buf[p - 1])) {
			p--;
		}
		if (p == 0 || !pdf_is_white(doc->buf[p - 1])) {
			continue;
		}
		p--;
		while (p > 0 && pdf_is_white(doc->buf[p])) {
			p--;
		}
		if (!is_digit(doc->buf[p])) {
			continue;
		}
		size_t num_end = p + 1;

		while (p > 0 && is_digit(doc->buf[p - 1])) {
			p--;
		}
		if (p > 0 && !pdf_is_white(doc->buf[p - 1]) && !pdf_is_delim(doc->buf[p - 1])) {
			continue;
		}
		uint64_t num = 0;

		for (size_t k = p; k < num_end; k++) {
			num = num * 10 + (doc->buf[k] - '0');
			if (num >= XREF_MAX_OBJECTS) {
				break;
			}
		}
		if (num >= XREF_MAX_OBJECTS) {
			continue;
		}
		xref_set(doc, (uint32_t)num, 1, (uint32_t)p, 0, true);
	}

	/* trailer dictionaries: the last one with /Root wins */
	for (long t = find_fwd(doc, 0, doc->len, "trailer"); t >= 0;
	     t = find_fwd(doc, (size_t)t + 7, doc->len, "trailer")) {
		struct pdf_lex lx;

		pdf_lex_init(&lx, doc->buf, doc->len, (size_t)t + 7);
		struct pdf_obj *d = pdf_parse_obj(&lx, true, 0);

		if (d != NULL && d->type == PDF_DICT && pdf_dict_get_raw(d, "Root") != NULL) {
			pdf_obj_free(found_trailer);
			found_trailer = d;
		} else {
			pdf_obj_free(d);
		}
	}

	/* Objects already parsed stay cached; decoded object streams are stale. */
	for (int i = 0; i < OBJSTM_CACHE; i++) {
		pdf_free(doc->ostm[i].data);
		doc->ostm[i].data = NULL;
	}

	/* Object streams and xref streams: walk every direct object once. */
	for (size_t i = 0; i < doc->xref_n; i++) {
		if (doc->xref[i].type != 1) {
			continue;
		}
		uint32_t n, g;
		struct pdf_obj *o = parse_indirect_at(doc, doc->xref[i].off, &n, &g);

		if (o == NULL) {
			continue;
		}
		struct pdf_obj *type = pdf_dict_get_raw(o, "Type");

		if (o->type == PDF_STREAM && pdf_is_name(type, "ObjStm")) {
			if (doc->cache[i] == NULL) {
				doc->cache[i] = o;
				o = NULL;
			}
			register_objstm_members(doc, (uint32_t)i);
		} else if (o->type == PDF_STREAM && pdf_is_name(type, "XRef") &&
			   found_trailer == NULL && pdf_dict_get_raw(o, "Root") != NULL) {
			found_trailer = o->stream.dict;
			o->stream.dict = NULL;
		} else if (o->type == PDF_DICT && pdf_is_name(type, "Catalog")) {
			root_num = (long)i;
		}
		pdf_obj_free(o);
	}
	/* Objects inside object streams can be the catalog too. */
	if (found_trailer == NULL && root_num < 0) {
		for (size_t i = 0; i < doc->xref_n; i++) {
			if (doc->xref[i].type != 2) {
				continue;
			}
			struct pdf_obj *o = load_from_objstm(doc, doc->xref[i].off, doc->xref[i].idx,
							     (uint32_t)i);

			if (o != NULL && o->type == PDF_DICT &&
			    pdf_is_name(pdf_dict_get_raw(o, "Type"), "Catalog")) {
				root_num = (long)i;
			}
			pdf_obj_free(o);
			if (root_num >= 0) {
				break;
			}
		}
	}
	if (found_trailer != NULL) {
		if (doc->trailer != NULL) {
			/* the scanned trailer replaces the Root we had */
			struct pdf_obj *r = pdf_dict_get_raw(found_trailer, "Root");

			for (size_t k = 0; k < found_trailer->dict.n; k++) {
				if (found_trailer->dict.items[k].val == r) {
					found_trailer->dict.items[k].val = pdf_obj_new(PDF_NULL);
				}
			}
			pdf_dict_put(doc->trailer, "Root", r);
			pdf_obj_free(found_trailer);
		} else {
			doc->trailer = found_trailer;
		}
	}
	if (root_num >= 0) {
		struct pdf_obj *root = pdf_dict_get_raw(doc->trailer, "Root");
		struct pdf_obj *cur = pdf_resolve(doc, root);

		if (cur == NULL || cur->type != PDF_DICT ||
		    !pdf_is_name(pdf_dict_get_raw(cur, "Type"), "Catalog")) {
			if (doc->trailer == NULL) {
				doc->trailer = pdf_obj_new(PDF_DICT);
			}
			struct pdf_obj *ref = pdf_obj_new(PDF_REF);

			if (ref != NULL && doc->trailer != NULL) {
				ref->ref.num = (uint32_t)root_num;
				pdf_dict_put(doc->trailer, "Root", ref);
			}
		}
	}
}

/* ---------------------------------------------------------------------- */
/* page tree                                                              */

static void walk_pages(struct pdf_doc *doc, struct pdf_obj *node, struct pdf_obj **path, int depth)
{
	if (node == NULL || node->type != PDF_DICT || depth >= 60 ||
	    doc->npages >= CONFIG_ZPDF_MAX_PAGES) {
		return;
	}
	for (int i = 0; i < depth; i++) {
		if (path[i] == node) {
			return;
		}
	}
	path[depth] = node;

	struct pdf_obj *type = pdf_get(doc, node, "Type");
	struct pdf_obj *kids = pdf_get(doc, node, "Kids");

	if (kids != NULL && kids->type == PDF_ARRAY && !pdf_is_name(type, "Page")) {
		for (size_t i = 0; i < kids->arr.n; i++) {
			walk_pages(doc, pdf_array_get(doc, kids, i), path, depth + 1);
		}
		return;
	}
	if (pdf_is_name(type, "Page") || pdf_dict_get_raw(node, "Contents") != NULL ||
	    (pdf_dict_get_raw(node, "MediaBox") != NULL && !pdf_is_name(type, "Pages"))) {
		struct pdf_obj **np = pdf_realloc(doc->pages, (doc->npages + 1) * sizeof(*np));

		if (np == NULL) {
			return;
		}
		doc->pages = np;
		doc->pages[doc->npages++] = node;
	}
}

static void build_pages(struct pdf_doc *doc)
{
	struct pdf_obj *path[60];
	struct pdf_obj *root = pdf_get(doc, doc->trailer, "Root");
	struct pdf_obj *pages = pdf_get(doc, root, "Pages");

	pdf_free(doc->pages);
	doc->pages = NULL;
	doc->npages = 0;
	if (pages == NULL && root != NULL) {
		/* some broken files point Root at the Pages node itself */
		if (pdf_dict_get_raw(root, "Kids") != NULL) {
			pages = root;
		}
	}
	walk_pages(doc, pages, path, 0);
}

size_t pdf_page_count(struct pdf_doc *doc)
{
	return doc->npages;
}

struct pdf_obj *pdf_page(struct pdf_doc *doc, size_t index)
{
	return index < doc->npages ? doc->pages[index] : NULL;
}

struct pdf_obj *pdf_page_attr(struct pdf_doc *doc, struct pdf_obj *page, const char *key)
{
	int hops = 0;

	while (page != NULL && page->type == PDF_DICT && hops++ < 64) {
		struct pdf_obj *v = pdf_get(doc, page, key);

		if (v != NULL && v->type != PDF_NULL) {
			return v;
		}
		page = pdf_get(doc, page, "Parent");
	}
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* info strings                                                           */

char *pdf_info_string(struct pdf_doc *doc, const char *key)
{
	struct pdf_obj *info = pdf_get(doc, doc->trailer, "Info");
	struct pdf_obj *s = pdf_get(doc, info, key);

	if (s == NULL || s->type != PDF_STRING || s->str.len == 0) {
		return NULL;
	}
	const uint8_t *b = s->str.data;
	size_t n = s->str.len;
	char *out = pdf_malloc(n * 3 + 1);
	size_t o = 0;

	if (out == NULL) {
		return NULL;
	}
	if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
		for (size_t i = 2; i + 1 < n; i += 2) {
			uint32_t cp = (b[i] << 8) | b[i + 1];

			if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
				uint32_t lo = (b[i + 2] << 8) | b[i + 3];

				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
			o += pdf_utf8_put(cp, out + o);
		}
	} else if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
		memcpy(out, b + 3, n - 3);
		o = n - 3;
	} else {
		for (size_t i = 0; i < n; i++) {
			o += pdf_utf8_put(b[i], out + o);
		}
	}
	out[o] = 0;
	return out;
}

/* ---------------------------------------------------------------------- */
/* font cache                                                             */

void *pdf_doc_font_cache_get(struct pdf_doc *doc, const struct pdf_obj *fontdict)
{
	for (size_t i = 0; i < doc->nfonts; i++) {
		if (doc->fonts[i].key == fontdict) {
			return doc->fonts[i].font;
		}
	}
	return NULL;
}

void pdf_doc_font_cache_put(struct pdf_doc *doc, const struct pdf_obj *fontdict, void *font)
{
	if (doc->nfonts == doc->fonts_cap) {
		size_t ncap = doc->fonts_cap ? doc->fonts_cap * 2 : 8;
		struct font_cache_entry *n = pdf_realloc(doc->fonts, ncap * sizeof(*n));

		if (n == NULL) {
			pdf_font_free_cb(font);
			return;
		}
		doc->fonts = n;
		doc->fonts_cap = ncap;
	}
	doc->fonts[doc->nfonts].key = fontdict;
	doc->fonts[doc->nfonts].font = font;
	doc->nfonts++;
}

static void aux_remove(struct pdf_doc *doc, size_t i)
{
	struct aux_entry *e = &doc->aux[i];

	if (e->free_fn != NULL) {
		e->free_fn(e->data);
	}
	doc->aux_bytes -= e->size;
	doc->aux[i] = doc->aux[doc->naux - 1];
	doc->naux--;
}

void *pdf_doc_aux_get(struct pdf_doc *doc, const void *key)
{
	for (size_t i = 0; i < doc->naux; i++) {
		if (doc->aux[i].key == key) {
			doc->aux[i].refs++;
			doc->aux[i].stamp = ++doc->aux_stamp;
			return doc->aux[i].data;
		}
	}
	return NULL;
}

int pdf_doc_aux_put(struct pdf_doc *doc, const void *key, void *data, size_t size, void (*free_fn)(void *))
{
	/* make room: drop unreferenced entries, oldest first */
	while (doc->aux_bytes + size > CONFIG_ZPDF_AUX_CACHE_BYTES && doc->naux > 0) {
		size_t oldest = doc->naux;
		uint32_t stamp = 0xFFFFFFFFu;

		for (size_t i = 0; i < doc->naux; i++) {
			if (doc->aux[i].refs == 0 && doc->aux[i].stamp < stamp) {
				stamp = doc->aux[i].stamp;
				oldest = i;
			}
		}
		if (oldest == doc->naux) {
			break; /* everything is in use */
		}
		aux_remove(doc, oldest);
	}
	if (doc->naux == doc->aux_cap) {
		size_t ncap = doc->aux_cap ? doc->aux_cap * 2 : 8;
		struct aux_entry *n = pdf_realloc(doc->aux, ncap * sizeof(*n));

		if (n == NULL) {
			return -1;
		}
		doc->aux = n;
		doc->aux_cap = ncap;
	}
	struct aux_entry *e = &doc->aux[doc->naux++];

	e->key = key;
	e->data = data;
	e->size = size;
	e->free_fn = free_fn;
	e->refs = 1;
	e->stamp = ++doc->aux_stamp;
	doc->aux_bytes += size;
	return 0;
}

void pdf_doc_aux_unref(struct pdf_doc *doc, const void *key)
{
	for (size_t i = 0; i < doc->naux; i++) {
		if (doc->aux[i].key == key && doc->aux[i].refs > 0) {
			doc->aux[i].refs--;
			return;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* open / close                                                           */

bool pdf_is_encrypted(const struct pdf_doc *doc)
{
	return doc->encrypted;
}

static void set_err(char *err, size_t err_len, const char *msg)
{
	if (err != NULL && err_len > 0) {
		strncpy(err, msg, err_len - 1);
		err[err_len - 1] = 0;
	}
}

static void clear_cache(struct pdf_doc *doc)
{
	for (size_t i = 0; i < doc->xref_n; i++) {
		pdf_obj_free(doc->cache[i]);
		doc->cache[i] = NULL;
	}
	for (int i = 0; i < OBJSTM_CACHE; i++) {
		pdf_free(doc->ostm[i].data);
		doc->ostm[i].data = NULL;
	}
}

struct pdf_doc *pdf_open(const uint8_t *buf, size_t len, char *err, size_t err_len)
{
	if (buf == NULL || len < 16) {
		set_err(err, err_len, "file too small");
		return NULL;
	}
	struct pdf_doc *doc = pdf_calloc(sizeof(*doc));

	if (doc == NULL) {
		set_err(err, err_len, "out of memory");
		return NULL;
	}
	doc->buf = buf;
	doc->len = len;

	long hdr = find_fwd(doc, 0, 1024, "%PDF");

	if (hdr < 0) {
		set_err(err, err_len, "not a PDF file");
		pdf_close(doc);
		return NULL;
	}
	doc->hdr_off = (size_t)hdr;

	long sx = find_back(doc, len, len > 2048 ? len - 2048 : 0, "startxref");
	int rc = -1;

	if (sx >= 0) {
		struct pdf_lex lx;
		struct pdf_tok t;

		pdf_lex_init(&lx, buf, len, (size_t)sx + 9);
		if (pdf_lex_next(&lx, &t) == 0 && t.type == TOK_NUM && t.ival >= 0) {
			rc = parse_xref_chain(doc, (long)t.ival);
			if (rc < 0 && doc->hdr_off != 0) {
				rc = parse_xref_chain(doc, (long)t.ival + (long)doc->hdr_off);
			}
		}
		pdf_tok_free(&t);
	}
	struct pdf_obj *root = (rc == 0) ? pdf_get(doc, doc->trailer, "Root") : NULL;

	if (root == NULL || root->type != PDF_DICT) {
		clear_cache(doc);
		reconstruct(doc);
		root = pdf_get(doc, doc->trailer, "Root");
		if (root == NULL || root->type != PDF_DICT) {
			set_err(err, err_len, "no document catalog");
			pdf_close(doc);
			return NULL;
		}
	}
	if (pdf_dict_get_raw(doc->trailer, "Encrypt") != NULL) {
		doc->encrypted = true;
	}
	build_pages(doc);
	if (doc->npages == 0 && !doc->reconstructed) {
		clear_cache(doc);
		reconstruct(doc);
		build_pages(doc);
	}
	if (doc->npages == 0) {
		set_err(err, err_len, "no pages found");
		pdf_close(doc);
		return NULL;
	}
	PDF_INF("opened: %u objects, %u pages", (unsigned)doc->xref_n, (unsigned)doc->npages);
	return doc;
}

void pdf_close(struct pdf_doc *doc)
{
	if (doc == NULL) {
		return;
	}
	for (size_t i = 0; i < doc->nfonts; i++) {
		pdf_font_free_cb(doc->fonts[i].font);
	}
	pdf_free(doc->fonts);
	while (doc->naux > 0) {
		aux_remove(doc, doc->naux - 1);
	}
	pdf_free(doc->aux);
	clear_cache(doc);
	pdf_free(doc->cache);
	pdf_free(doc->parsing);
	pdf_free(doc->xref);
	pdf_free(doc->pages);
	pdf_obj_free(doc->trailer);
	pdf_free(doc);
}
