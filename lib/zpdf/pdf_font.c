/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Font loading: simple fonts (Type1, TrueType, Type3) with their encodings
 * and composite Type0 fonts with embedded or Identity CMaps, plus ToUnicode
 * CMaps (section 9.10) for the text extractor.
 */
#include "pdf_font.h"
#include "pdf_lex.h"
#include "pdf_mem.h"
#include "pdf_util.h"
#include "pdf_encodings.h"
#include "pdf_ttf.h"
#include <string.h>

PDF_LOG_REGISTER();

#define CMAP_MAX_RANGES 65536
#define CMAP_MAX_SINGLES 65536

/* ---------------------------------------------------------------------- */
/* CMap                                                                   */

static uint32_t bytes_to_code(const uint8_t *s, size_t n)
{
	uint32_t v = 0;

	if (n > 4) {
		n = 4;
	}
	for (size_t i = 0; i < n; i++) {
		v = (v << 8) | s[i];
	}
	return v;
}

static int cmap_add_range(struct cmap *cm, uint32_t lo, uint32_t hi, uint32_t dst, uint8_t nbytes)
{
	if (cm->nranges == cm->ranges_cap) {
		size_t ncap = cm->ranges_cap ? cm->ranges_cap * 2 : 32;

		if (ncap > CMAP_MAX_RANGES) {
			return -1;
		}
		struct cmap_range *n = pdf_realloc(cm->ranges, ncap * sizeof(*n));

		if (n == NULL) {
			return -1;
		}
		cm->ranges = n;
		cm->ranges_cap = ncap;
	}
	cm->ranges[cm->nranges].lo = lo;
	cm->ranges[cm->nranges].hi = hi;
	cm->ranges[cm->nranges].dst = dst;
	cm->ranges[cm->nranges].nbytes = nbytes;
	cm->nranges++;
	return 0;
}

static int cmap_add_single(struct cmap *cm, uint32_t code, uint8_t nbytes, const char *utf8,
			   size_t len)
{
	if (cm->nsingles == cm->singles_cap) {
		size_t ncap = cm->singles_cap ? cm->singles_cap * 2 : 32;

		if (ncap > CMAP_MAX_SINGLES) {
			return -1;
		}
		struct cmap_single *n = pdf_realloc(cm->singles, ncap * sizeof(*n));

		if (n == NULL) {
			return -1;
		}
		cm->singles = n;
		cm->singles_cap = ncap;
	}
	struct cmap_single *s = &cm->singles[cm->nsingles++];

	s->code = code;
	s->nbytes = nbytes;
	if (len >= sizeof(s->utf8)) {
		len = sizeof(s->utf8) - 1;
	}
	memcpy(s->utf8, utf8, len);
	s->utf8[len] = 0;
	return 0;
}

static void cmap_add_codespace(struct cmap *cm, uint32_t lo, uint32_t hi, uint8_t nbytes)
{
	if (cm->ncs < (int)(sizeof(cm->cs) / sizeof(cm->cs[0])) && nbytes >= 1 && nbytes <= 4) {
		cm->cs[cm->ncs].lo = lo;
		cm->cs[cm->ncs].hi = hi;
		cm->cs[cm->ncs].nbytes = nbytes;
		cm->ncs++;
	}
}

/* UTF-16BE string (a bfchar/bfrange destination) to UTF-8. Returns length. */
static size_t utf16be_to_utf8(const uint8_t *s, size_t n, char *out, size_t out_cap)
{
	size_t o = 0;

	if (n == 1) {
		/* a one byte destination: treat as Latin-1 */
		return (size_t)pdf_utf8_put(s[0], out);
	}
	for (size_t i = 0; i + 1 < n; i += 2) {
		uint32_t cp = (s[i] << 8) | s[i + 1];

		if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
			uint32_t lo = (s[i + 2] << 8) | s[i + 3];

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (o + 4 >= out_cap) {
			break;
		}
		o += pdf_utf8_put(cp, out + o);
	}
	return o;
}

static uint32_t utf16be_first_cp(const uint8_t *s, size_t n, bool *single)
{
	*single = false;
	if (n == 1) {
		*single = true;
		return s[0];
	}
	if (n < 2) {
		return 0;
	}
	uint32_t cp = (s[0] << 8) | s[1];

	if (cp >= 0xD800 && cp <= 0xDBFF && n >= 4) {
		uint32_t lo = (s[2] << 8) | s[3];

		cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
		*single = (n == 4);
		return cp;
	}
	*single = (n == 2);
	return cp;
}

/*
 * Parse CMap syntax. For encoding CMaps destinations are CIDs (integers);
 * for ToUnicode CMaps they are UTF-16BE strings (or glyph names).
 */
static void cmap_parse(struct cmap *cm, const uint8_t *data, size_t len)
{
	struct pdf_lex lx;
	struct pdf_obj *stack[64];
	int sp = 0;
	int guard = 0;

	pdf_lex_init(&lx, data, len, 0);
	for (;;) {
		struct pdf_obj *o = pdf_parse_obj(&lx, false, 0);

		if (o == NULL) {
			break;
		}
		if (++guard > 2000000) {
			pdf_obj_free(o);
			break;
		}
		if (o->type != PDF_OP) {
			if (sp == 64) {
				pdf_obj_free(stack[0]);
				memmove(stack, stack + 1, 63 * sizeof(stack[0]));
				sp = 63;
			}
			stack[sp++] = o;
			continue;
		}
		const char *op = o->op;

		if (strcmp(op, "begincodespacerange") == 0) {
			for (;;) {
				struct pdf_obj *lo = pdf_parse_obj(&lx, false, 0);

				if (lo == NULL || lo->type != PDF_STRING) {
					pdf_obj_free(lo);
					break;
				}
				struct pdf_obj *hi = pdf_parse_obj(&lx, false, 0);

				if (hi == NULL || hi->type != PDF_STRING) {
					pdf_obj_free(lo);
					pdf_obj_free(hi);
					break;
				}
				cmap_add_codespace(cm, bytes_to_code(lo->str.data, lo->str.len),
						   bytes_to_code(hi->str.data, hi->str.len),
						   (uint8_t)lo->str.len);
				pdf_obj_free(lo);
				pdf_obj_free(hi);
			}
		} else if (strcmp(op, "begincidrange") == 0 || strcmp(op, "beginbfrange") == 0) {
			bool bf = op[5] == 'b';

			for (;;) {
				struct pdf_obj *lo = pdf_parse_obj(&lx, false, 0);

				if (lo == NULL || lo->type != PDF_STRING) {
					pdf_obj_free(lo);
					break;
				}
				struct pdf_obj *hi = pdf_parse_obj(&lx, false, 0);
				struct pdf_obj *dst = hi ? pdf_parse_obj(&lx, false, 0) : NULL;

				if (hi == NULL || hi->type != PDF_STRING || dst == NULL) {
					pdf_obj_free(lo);
					pdf_obj_free(hi);
					pdf_obj_free(dst);
					break;
				}
				uint32_t l = bytes_to_code(lo->str.data, lo->str.len);
				uint32_t h = bytes_to_code(hi->str.data, hi->str.len);
				uint8_t nb = (uint8_t)lo->str.len;

				if (h < l) {
					h = l;
				}
				if (h - l > 65535) {
					h = l + 65535;
				}
				if (!bf) {
					if (pdf_is_num(dst)) {
						cmap_add_range(cm, l, h, (uint32_t)pdf_int(dst), nb);
					}
				} else if (dst->type == PDF_STRING) {
					bool single;
					uint32_t cp = utf16be_first_cp(dst->str.data, dst->str.len, &single);

					if (single) {
						cmap_add_range(cm, l, h, cp, nb);
					} else {
						/* multi-char destination: only the low code gets it exactly */
						char buf[16];
						size_t bl = utf16be_to_utf8(dst->str.data, dst->str.len, buf,
									    sizeof(buf));

						cmap_add_single(cm, l, nb, buf, bl);
						if (h > l) {
							cmap_add_range(cm, l + 1, h, cp + 1, nb);
						}
					}
				} else if (dst->type == PDF_ARRAY) {
					for (size_t i = 0; i < dst->arr.n && l + i <= h; i++) {
						struct pdf_obj *d = dst->arr.items[i];

						if (d->type == PDF_STRING) {
							char buf[16];
							size_t bl = utf16be_to_utf8(d->str.data, d->str.len,
										    buf, sizeof(buf));

							cmap_add_single(cm, l + (uint32_t)i, nb, buf, bl);
						}
					}
				}
				pdf_obj_free(lo);
				pdf_obj_free(hi);
				pdf_obj_free(dst);
			}
		} else if (strcmp(op, "begincidchar") == 0 || strcmp(op, "beginbfchar") == 0) {
			bool bf = op[5] == 'b';

			for (;;) {
				struct pdf_obj *src = pdf_parse_obj(&lx, false, 0);

				if (src == NULL || src->type != PDF_STRING) {
					pdf_obj_free(src);
					break;
				}
				struct pdf_obj *dst = pdf_parse_obj(&lx, false, 0);

				if (dst == NULL) {
					pdf_obj_free(src);
					break;
				}
				uint32_t c = bytes_to_code(src->str.data, src->str.len);
				uint8_t nb = (uint8_t)src->str.len;

				if (!bf) {
					if (pdf_is_num(dst)) {
						cmap_add_range(cm, c, c, (uint32_t)pdf_int(dst), nb);
					}
				} else if (dst->type == PDF_STRING) {
					char buf[16];
					size_t bl = utf16be_to_utf8(dst->str.data, dst->str.len, buf,
								    sizeof(buf));

					cmap_add_single(cm, c, nb, buf, bl);
				} else if (dst->type == PDF_NAME) {
					uint32_t cp = pdf_glyph_to_unicode(pdf_name_str(dst));

					if (cp) {
						cmap_add_range(cm, c, c, cp, nb);
					}
				}
				pdf_obj_free(src);
				pdf_obj_free(dst);
			}
		} else if (strcmp(op, "usecmap") == 0) {
			/* "/Identity-H usecmap" */
			if (sp > 0 && stack[sp - 1]->type == PDF_NAME &&
			    strncmp(pdf_name_str(stack[sp - 1]), "Identity", 8) == 0) {
				cm->identity = true;
			}
		} else if (strcmp(op, "endcmap") == 0) {
			pdf_obj_free(o);
			break;
		}
		pdf_obj_free(o);
		while (sp > 0) {
			pdf_obj_free(stack[--sp]);
		}
	}
	while (sp > 0) {
		pdf_obj_free(stack[--sp]);
	}
}

static void cmap_free(struct cmap *cm)
{
	pdf_free(cm->ranges);
	pdf_free(cm->singles);
	cm->ranges = NULL;
	cm->singles = NULL;
}

/* Returns bytes consumed for the next code according to the codespace ranges. */
static int cmap_split(const struct cmap *cm, const uint8_t *s, size_t n, uint32_t *code)
{
	if (cm->ncs == 0) {
		int nb = n >= 2 ? 2 : 1;

		*code = bytes_to_code(s, (size_t)nb);
		return nb;
	}
	/* try 1..4 byte prefixes against the codespace ranges */
	for (int nb = 1; nb <= 4 && (size_t)nb <= n; nb++) {
		uint32_t c = bytes_to_code(s, (size_t)nb);

		for (int i = 0; i < cm->ncs; i++) {
			if (cm->cs[i].nbytes == nb && c >= cm->cs[i].lo && c <= cm->cs[i].hi) {
				*code = c;
				return nb;
			}
		}
	}
	/* no match: use the shortest codespace length (partial match rule, simplified) */
	int nb = cm->cs[0].nbytes;

	for (int i = 1; i < cm->ncs; i++) {
		if (cm->cs[i].nbytes < nb) {
			nb = cm->cs[i].nbytes;
		}
	}
	if ((size_t)nb > n) {
		nb = (int)n;
	}
	*code = bytes_to_code(s, (size_t)nb);
	return nb;
}

static bool cmap_lookup_cid(const struct cmap *cm, uint32_t code, uint32_t *cid)
{
	for (size_t i = 0; i < cm->nranges; i++) {
		if (code >= cm->ranges[i].lo && code <= cm->ranges[i].hi) {
			*cid = cm->ranges[i].dst + (code - cm->ranges[i].lo);
			return true;
		}
	}
	return false;
}

static int cmap_lookup_utf8(const struct cmap *cm, uint32_t code, char *out)
{
	for (size_t i = 0; i < cm->nsingles; i++) {
		if (cm->singles[i].code == code) {
			size_t l = strlen(cm->singles[i].utf8);

			memcpy(out, cm->singles[i].utf8, l);
			return (int)l;
		}
	}
	for (size_t i = 0; i < cm->nranges; i++) {
		if (code >= cm->ranges[i].lo && code <= cm->ranges[i].hi) {
			return pdf_utf8_put(cm->ranges[i].dst + (code - cm->ranges[i].lo), out);
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* widths                                                                 */

static void load_simple_widths(struct pdf_doc *doc, struct pdf_font *f, struct pdf_obj *dict)
{
	struct pdf_obj *fc = pdf_get(doc, dict, "FirstChar");
	struct pdf_obj *w = pdf_get(doc, dict, "Widths");
	struct pdf_obj *fd = pdf_get(doc, dict, "FontDescriptor");
	struct pdf_obj *mw = pdf_get(doc, fd, "MissingWidth");

	f->first_char = fc ? (int)pdf_int(fc) : 0;
	f->missing_width = mw ? pdf_num(mw) / 1000.0f : 0.0f;
	if (w != NULL && w->type == PDF_ARRAY && w->arr.n > 0 && w->arr.n <= 256) {
		f->nwidths = (int)w->arr.n;
		f->widths = pdf_malloc(sizeof(float) * (size_t)f->nwidths);
		if (f->widths == NULL) {
			f->nwidths = 0;
			return;
		}
		for (int i = 0; i < f->nwidths; i++) {
			struct pdf_obj *wi = pdf_array_get(doc, w, (size_t)i);

			f->widths[i] = pdf_num(wi) * f->type3_scale / 1000.0f;
		}
	} else {
		/* standard 14 fonts carry no /Widths: use the AFM metrics */
		const char *base = pdf_name_str(pdf_get(doc, dict, "BaseFont"));
		int flags = (int)pdf_int(pdf_get(doc, fd, "Flags"));
		bool fixed;
		const uint16_t *std = pdf_std_font_widths(base, flags, &fixed);

		if (fixed || std == NULL) {
			f->missing_width = 0.6f;
			return;
		}
		f->first_char = 0x20;
		f->nwidths = 95;
		f->widths = pdf_malloc(sizeof(float) * 95);
		if (f->widths == NULL) {
			f->nwidths = 0;
			f->missing_width = 0.5f;
			return;
		}
		for (int i = 0; i < 95; i++) {
			f->widths[i] = std[i] / 1000.0f;
		}
		f->missing_width = 0.5f;
	}
}

static void load_cid_widths(struct pdf_doc *doc, struct pdf_font *f, struct pdf_obj *cidfont)
{
	struct pdf_obj *dw = pdf_get(doc, cidfont, "DW");
	struct pdf_obj *w = pdf_get(doc, cidfont, "W");

	f->default_width = dw ? pdf_num(dw) / 1000.0f : 1.0f;
	if (w == NULL || w->type != PDF_ARRAY) {
		return;
	}
	size_t cap = 0;
	size_t i = 0;

	while (i < w->arr.n) {
		struct pdf_obj *a = pdf_array_get(doc, w, i);

		if (!pdf_is_num(a)) {
			i++;
			continue;
		}
		uint32_t first = (uint32_t)pdf_int(a);
		struct pdf_obj *b = pdf_array_get(doc, w, i + 1);

		if (b != NULL && b->type == PDF_ARRAY) {
			for (size_t k = 0; k < b->arr.n; k++) {
				if (f->nwranges == cap) {
					cap = cap ? cap * 2 : 32;
					struct width_range *n = pdf_realloc(f->wranges, cap * sizeof(*n));

					if (n == NULL) {
						return;
					}
					f->wranges = n;
				}
				f->wranges[f->nwranges].first = first + (uint32_t)k;
				f->wranges[f->nwranges].last = first + (uint32_t)k;
				f->wranges[f->nwranges].w = pdf_num(pdf_array_get(doc, b, k)) / 1000.0f;
				f->nwranges++;
			}
			i += 2;
		} else if (pdf_is_num(b)) {
			struct pdf_obj *c = pdf_array_get(doc, w, i + 2);

			if (f->nwranges == cap) {
				cap = cap ? cap * 2 : 32;
				struct width_range *n = pdf_realloc(f->wranges, cap * sizeof(*n));

				if (n == NULL) {
					return;
				}
				f->wranges = n;
			}
			f->wranges[f->nwranges].first = first;
			f->wranges[f->nwranges].last = (uint32_t)pdf_int(b);
			f->wranges[f->nwranges].w = pdf_num(c) / 1000.0f;
			f->nwranges++;
			i += 3;
		} else {
			break;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* encodings                                                              */

static void load_simple_encoding(struct pdf_doc *doc, struct pdf_font *f, struct pdf_obj *dict)
{
	struct pdf_obj *enc = pdf_get(doc, dict, "Encoding");
	struct pdf_obj *fd = pdf_get(doc, dict, "FontDescriptor");
	int flags = (int)pdf_int(pdf_get(doc, fd, "Flags"));
	enum pdf_base_encoding base = PDF_ENC_STANDARD;
	bool have_base = false;

	f->symbolic = (flags & 4) && !(flags & 32);

	const char *subtype = pdf_name_str(pdf_get(doc, dict, "Subtype"));
	struct pdf_obj *base_name = NULL;

	if (enc != NULL && enc->type == PDF_NAME) {
		base_name = enc;
	} else if (enc != NULL && enc->type == PDF_DICT) {
		base_name = pdf_get(doc, enc, "BaseEncoding");
	}
	if (base_name != NULL) {
		if (pdf_is_name(base_name, "WinAnsiEncoding")) {
			base = PDF_ENC_WINANSI;
			have_base = true;
		} else if (pdf_is_name(base_name, "MacRomanEncoding")) {
			base = PDF_ENC_MACROMAN;
			have_base = true;
		} else if (pdf_is_name(base_name, "StandardEncoding") ||
			   pdf_is_name(base_name, "MacExpertEncoding")) {
			base = PDF_ENC_STANDARD;
			have_base = true;
		}
	}
	if (!have_base) {
		/* TrueType fonts default to WinAnsi in practice, Type1 to Standard */
		base = (strcmp(subtype, "TrueType") == 0 || f->symbolic) ? PDF_ENC_WINANSI
									  : PDF_ENC_STANDARD;
		if (f->symbolic && strcmp(subtype, "TrueType") != 0 && !f->type3) {
			/* symbolic Type1 (Symbol, Dingbats): keep ASCII only */
			base = PDF_ENC_STANDARD;
		}
	}
	pdf_encoding_fill(base, f->simple_enc);

	if (enc != NULL && enc->type == PDF_DICT) {
		struct pdf_obj *diff = pdf_get(doc, enc, "Differences");

		if (diff != NULL && diff->type == PDF_ARRAY) {
			int code = 0;

			for (size_t i = 0; i < diff->arr.n; i++) {
				struct pdf_obj *d = pdf_array_get(doc, diff, i);

				if (pdf_is_num(d)) {
					code = (int)pdf_int(d);
				} else if (d != NULL && d->type == PDF_NAME && code >= 0 && code < 256) {
					uint32_t cp = pdf_glyph_to_unicode(pdf_name_str(d));

					f->simple_enc[code] = (cp && cp < 0x10000) ? (uint16_t)cp : 0;
					if (cp == 0) {
						/* unknown glyph name such as g123: keep the base */
						f->simple_enc[code] = f->simple_enc[code];
					}
					code++;
				}
			}
		}
	}
}

/* ---------------------------------------------------------------------- */
/* loading                                                                */

struct pdf_font *pdf_font_load(struct pdf_doc *doc, struct pdf_obj *dict)
{
	struct pdf_font *f = pdf_calloc(sizeof(*f));

	if (f == NULL) {
		return NULL;
	}
	f->type3_scale = 1.0f;
	f->default_width = 1.0f;

	const char *subtype = pdf_name_str(pdf_get(doc, dict, "Subtype"));

	if (strcmp(subtype, "Type0") == 0) {
		f->type0 = true;
		struct pdf_obj *enc = pdf_get(doc, dict, "Encoding");

		if (enc != NULL && enc->type == PDF_NAME) {
			const char *n = pdf_name_str(enc);

			f->enc.identity = true; /* predefined CMaps other than Identity: assume 2 bytes */
			f->vertical = n[strlen(n) - 1] == 'V' && strlen(n) > 2 && n[strlen(n) - 2] == '-';
			if (strncmp(n, "Identity", 8) != 0) {
				PDF_WRN("predefined CMap %s not available, assuming 2 byte codes", n);
			}
		} else if (enc != NULL && enc->type == PDF_STREAM) {
			uint8_t *data;
			size_t len;

			if (pdf_stream_decode(doc, enc, &data, &len) >= 0) {
				cmap_parse(&f->enc, data, len);
				pdf_free(data);
			}
			if (f->enc.nranges == 0 && f->enc.ncs == 0) {
				f->enc.identity = true;
			}
		} else {
			f->enc.identity = true;
		}
		struct pdf_obj *desc = pdf_get(doc, dict, "DescendantFonts");
		struct pdf_obj *cidfont = pdf_array_get(doc, desc, 0);

		if (cidfont == NULL && desc != NULL && desc->type == PDF_DICT) {
			cidfont = desc;
		}
		load_cid_widths(doc, f, cidfont);
		f->doc = doc;
		f->descriptor = pdf_get(doc, cidfont, "FontDescriptor");
		struct pdf_obj *c2g = pdf_get(doc, cidfont, "CIDToGIDMap");

		f->cid_to_gid = (c2g != NULL && c2g->type == PDF_STREAM) ? c2g : NULL;
	} else {
		if (strcmp(subtype, "Type3") == 0) {
			f->type3 = true;
			struct pdf_obj *fm = pdf_get(doc, dict, "FontMatrix");

			if (fm != NULL && fm->type == PDF_ARRAY && fm->arr.n >= 4) {
				float a = pdf_num(pdf_array_get(doc, fm, 0));

				f->type3_scale = a * 1000.0f;
			}
		}
		load_simple_widths(doc, f, dict);
		load_simple_encoding(doc, f, dict);
	}

	struct pdf_obj *tu = pdf_get(doc, dict, "ToUnicode");

	if (tu != NULL && tu->type == PDF_STREAM) {
		uint8_t *data;
		size_t len;

		if (pdf_stream_decode(doc, tu, &data, &len) >= 0) {
			cmap_parse(&f->tounicode, data, len);
			pdf_free(data);
			f->has_tounicode = f->tounicode.nranges > 0 || f->tounicode.nsingles > 0;
		}
	}
	return f;
}

void pdf_font_free(struct pdf_font *f)
{
	if (f == NULL) {
		return;
	}
	cmap_free(&f->enc);
	cmap_free(&f->tounicode);
	pdf_free(f->widths);
	pdf_free(f->wranges);
	pdf_free(f->gid_unicode);
	pdf_free(f->cid2gid);
	pdf_free(f);
}

/* Glyph index -> Unicode from the embedded TrueType/OpenType program. */
static void load_font_program(struct pdf_font *f)
{
	f->program_tried = true;
	if (f->doc == NULL || f->descriptor == NULL) {
		return;
	}
	struct pdf_obj *ff = pdf_get(f->doc, f->descriptor, "FontFile2");

	if (ff == NULL) {
		ff = pdf_get(f->doc, f->descriptor, "FontFile3");
		if (ff != NULL && !pdf_is_name(pdf_get(f->doc, ff, "Subtype"), "OpenType")) {
			ff = NULL; /* CFF programs carry no cmap */
		}
	}
	if (ff == NULL || ff->type != PDF_STREAM) {
		return;
	}
	uint8_t *data;
	size_t len;

	if (pdf_stream_decode(f->doc, ff, &data, &len) != 0) {
		return;
	}
	int rc = pdf_ttf_gid_to_unicode(data, len, &f->gid_unicode, &f->ngid);

	pdf_free(data);
	if (rc > 0) {
		PDF_INF("font program maps %d glyphs", rc);
	}
	if (f->cid_to_gid != NULL && pdf_stream_decode(f->doc, f->cid_to_gid, &data, &len) == 0) {
		f->cid2gid = data;
		f->cid2gid_len = len;
	}
}

static int program_to_utf8(struct pdf_font *f, uint32_t cid, char *out)
{
	if (!f->program_tried) {
		load_font_program(f);
	}
	if (f->gid_unicode == NULL) {
		return 0;
	}
	uint32_t gid = cid;

	if (f->cid2gid != NULL) {
		if ((size_t)cid * 2 + 1 >= f->cid2gid_len) {
			return 0;
		}
		gid = ((uint32_t)f->cid2gid[cid * 2] << 8) | f->cid2gid[cid * 2 + 1];
	}
	if (gid >= f->ngid || f->gid_unicode[gid] == 0) {
		return 0;
	}
	return pdf_utf8_put(f->gid_unicode[gid], out);
}

void pdf_font_free_cb(void *font)
{
	pdf_font_free(font);
}

int pdf_font_next_code(const struct pdf_font *f, const uint8_t *s, size_t n, uint32_t *code,
		       uint32_t *cid)
{
	if (n == 0) {
		*code = 0;
		*cid = 0;
		return 1;
	}
	if (!f->type0) {
		*code = s[0];
		*cid = s[0];
		return 1;
	}
	int nb;

	if (f->enc.ncs > 0) {
		nb = cmap_split(&f->enc, s, n, code);
	} else if (f->tounicode.ncs > 0 && !f->enc.identity) {
		nb = cmap_split(&f->tounicode, s, n, code);
	} else {
		nb = n >= 2 ? 2 : 1;
		*code = bytes_to_code(s, (size_t)nb);
	}
	if (f->enc.nranges > 0 && cmap_lookup_cid(&f->enc, *code, cid)) {
		return nb;
	}
	*cid = *code; /* Identity */
	return nb;
}

float pdf_font_width(const struct pdf_font *f, uint32_t code, uint32_t cid)
{
	if (f->type0) {
		for (size_t i = 0; i < f->nwranges; i++) {
			if (cid >= f->wranges[i].first && cid <= f->wranges[i].last) {
				return f->wranges[i].w;
			}
		}
		return f->default_width;
	}
	int idx = (int)code - f->first_char;

	if (f->widths != NULL && idx >= 0 && idx < f->nwidths) {
		float w = f->widths[idx];

		if (w != 0.0f || f->missing_width == 0.0f) {
			return w;
		}
	}
	return f->missing_width;
}

int pdf_font_to_utf8(struct pdf_font *f, uint32_t code, uint32_t cid, char *out)
{
	if (f->has_tounicode) {
		int n = cmap_lookup_utf8(&f->tounicode, code, out);

		if (n > 0) {
			return n;
		}
	}
	if (!f->type0) {
		uint16_t u = (code < 256) ? f->simple_enc[code] : 0;

		if (u == 0) {
			return 0;
		}
		return pdf_utf8_put(u, out);
	}
	/* Composite font: try the cmap of the embedded font program. */
	int n = program_to_utf8(f, cid, out);

	if (n > 0) {
		return n;
	}
	return pdf_utf8_put(0xFFFD, out);
}
