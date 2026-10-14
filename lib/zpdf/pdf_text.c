/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Content stream interpreter for text (sections 8.4, 9.3, 9.4). Graphics
 * state is tracked only as far as it moves glyphs: CTM, text matrix, text
 * line matrix, font, size, spacing, scaling, rise and leading.
 */
#include <zpdf/pdf_text.h>
#include "pdf_font.h"
#include "pdf_lex.h"
#include "pdf_mem.h"
#include "pdf_util.h"
#include <string.h>
#include <math.h>

PDF_LOG_REGISTER();

#define GS_STACK 32
#define OPERANDS 32
#define MAX_FORM_DEPTH 12
#define MAX_TEXT_BYTES (1u << 20)

struct mat {
	float a, b, c, d, e, f;
};

static const struct mat identity = {1, 0, 0, 1, 0, 0};

static struct mat mat_mul(struct mat m, struct mat n)
{
	struct mat r;

	r.a = m.a * n.a + m.b * n.c;
	r.b = m.a * n.b + m.b * n.d;
	r.c = m.c * n.a + m.d * n.c;
	r.d = m.c * n.b + m.d * n.d;
	r.e = m.e * n.a + m.f * n.c + n.e;
	r.f = m.e * n.b + m.f * n.d + n.f;
	return r;
}

static void mat_apply(struct mat m, float x, float y, float *ox, float *oy)
{
	*ox = m.a * x + m.c * y + m.e;
	*oy = m.b * x + m.d * y + m.f;
}

struct text_state {
	struct pdf_font *font;
	float size;
	float tc, tw, th, tl, rise;
	int mode;
};

struct outbuf {
	char *p;
	size_t n, cap;
	bool overflow;
};

struct ctx {
	struct pdf_doc *doc;
	struct outbuf out;

	struct mat ctm_stack[GS_STACK];
	int gs_sp;
	struct mat ctm;
	struct text_state ts;
	struct text_state ts_stack[GS_STACK];
	struct mat tm, tlm;
	bool in_text;

	/* layout heuristics */
	bool have_last;
	float last_x, last_y, last_size;
	float line_dir_x, line_dir_y; /* baseline direction of the last glyph */
	float last_start_x, last_start_y;
	char last_utf8[16];
	int last_len;
	int form_depth;
	struct pdf_obj *form_path[MAX_FORM_DEPTH];
};

/* ---------------------------------------------------------------------- */
/* output                                                                 */

static void out_put(struct ctx *c, const char *s, size_t n)
{
	struct outbuf *b = &c->out;

	if (b->overflow) {
		return;
	}
	if (b->n + n + 1 > b->cap) {
		size_t ncap = b->cap ? b->cap : 2048;

		while (ncap < b->n + n + 1) {
			ncap *= 2;
		}
		if (ncap > MAX_TEXT_BYTES) {
			b->overflow = true;
			return;
		}
		char *np = pdf_realloc(b->p, ncap);

		if (np == NULL) {
			b->overflow = true;
			return;
		}
		b->p = np;
		b->cap = ncap;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

static char out_last(const struct ctx *c)
{
	return c->out.n ? c->out.p[c->out.n - 1] : '\n';
}

static void out_newline(struct ctx *c)
{
	/* trim trailing spaces of the line */
	while (c->out.n > 0 && c->out.p[c->out.n - 1] == ' ') {
		c->out.n--;
	}
	if (c->out.n > 0) {
		c->out.p[c->out.n] = 0;
	}
	out_put(c, "\n", 1);
}

static void out_space(struct ctx *c)
{
	char l = out_last(c);

	if (l != ' ' && l != '\n') {
		out_put(c, " ", 1);
	}
}

/* ---------------------------------------------------------------------- */
/* resources                                                              */

static struct pdf_font *get_font(struct ctx *c, struct pdf_obj *resources, const char *name)
{
	struct pdf_obj *fonts = pdf_get(c->doc, resources, "Font");
	struct pdf_obj *fd = pdf_get(c->doc, fonts, name);

	if (fd == NULL || fd->type != PDF_DICT) {
		return NULL;
	}
	struct pdf_font *f = pdf_doc_font_cache_get(c->doc, fd);

	if (f == NULL) {
		f = pdf_font_load(c->doc, fd);
		if (f != NULL) {
			pdf_doc_font_cache_put(c->doc, fd, f);
		}
	}
	return f;
}

/* ---------------------------------------------------------------------- */
/* glyph placement                                                        */

static void place_glyph(struct ctx *c, float x, float y, float size_dev, float adv_x, float adv_y,
			const char *utf8, int len, bool is_space)
{
#if !defined(__ZEPHYR__) && defined(ZPDF_DEBUG)
	fprintf(stderr, "glyph x=%.2f y=%.2f size=%.2f adv=%.2f,%.2f last=%.2f,%.2f '%.*s'\n", x, y,
		size_dev, adv_x, adv_y, c->last_x, c->last_y, len, utf8);
#endif
	if (c->have_last) {
		float ref = size_dev > c->last_size ? size_dev : c->last_size;

		if (ref < 1.0f) {
			ref = 1.0f;
		}
		/* project the movement from the last glyph onto its baseline direction */
		float dx = x - c->last_x, dy = y - c->last_y;
		float along = dx * c->line_dir_x + dy * c->line_dir_y;
		float across = -dx * c->line_dir_y + dy * c->line_dir_x;

		if (fabsf(across) > 0.5f * ref) {
			/* superscripts move a little; a real new line moves more */
			if (fabsf(across) > 0.9f * ref || along < -0.5f * ref) {
				out_newline(c);
			} else if (along > 0.12f * ref) {
				out_space(c);
			}
		} else if (along < -1.5f * ref) {
			out_newline(c);
		} else if (along > 0.12f * ref) {
			out_space(c);
		}
	}
	/* Fake bold draws the same glyph several times at nearly the same spot. */
	bool duplicate = c->have_last && len > 0 && len == c->last_len &&
			 memcmp(utf8, c->last_utf8, (size_t)len) == 0 &&
			 fabsf(x - c->last_start_x) < 0.15f * size_dev &&
			 fabsf(y - c->last_start_y) < 0.15f * size_dev;

	if (duplicate) {
		return;
	}
	if (len > 0) {
		if (is_space) {
			out_space(c);
		} else {
			out_put(c, utf8, (size_t)len);
		}
	}
	c->last_start_x = x;
	c->last_start_y = y;
	c->last_len = len;
	memcpy(c->last_utf8, utf8, (size_t)(len > 0 ? len : 0));
	float adv_len = sqrtf(adv_x * adv_x + adv_y * adv_y);

	c->last_x = x + adv_x;
	c->last_y = y + adv_y;
	c->last_size = size_dev;
	if (adv_len > 1e-6f) {
		c->line_dir_x = adv_x / adv_len;
		c->line_dir_y = adv_y / adv_len;
	}
	c->have_last = true;
}

static void show_text(struct ctx *c, const uint8_t *s, size_t n)
{
	struct text_state *ts = &c->ts;

	if (ts->font == NULL) {
		/* No font set: assume a 1 byte WinAnsi-ish font so text still appears. */
		static struct pdf_font *fallback;

		if (fallback == NULL) {
			fallback = pdf_calloc(sizeof(*fallback));
			if (fallback == NULL) {
				return;
			}
			for (int i = 0x20; i < 0x7F; i++) {
				fallback->simple_enc[i] = (uint16_t)i;
			}
			fallback->missing_width = 0.5f;
		}
		ts->font = fallback;
	}
	struct pdf_font *f = ts->font;
	size_t i = 0;

	while (i < n) {
		uint32_t code, cid;
		int nb = pdf_font_next_code(f, s + i, n - i, &code, &cid);

		if (nb <= 0) {
			nb = 1;
		}
		float w0 = pdf_font_width(f, code, cid);
		bool is_space_code = (nb == 1 && code == 32);
		float tx = (w0 * ts->size + ts->tc + (is_space_code ? ts->tw : 0.0f)) * ts->th;
		float ty = 0.0f;

		if (f->vertical) {
			ty = -(w0 * ts->size + ts->tc + (is_space_code ? ts->tw : 0.0f));
			tx = 0.0f;
		}
		struct mat trm = mat_mul(c->tm, c->ctm);
		float x, y, x2, y2;

		mat_apply(trm, 0.0f, ts->rise, &x, &y);
		mat_apply(trm, tx, ts->rise + ty, &x2, &y2);
		float size_dev = ts->size * sqrtf(trm.b * trm.b + trm.d * trm.d);

		char utf8[16];
		int len = pdf_font_to_utf8(f, code, cid, utf8);
		bool is_space = false;

		if (len == 1 && (utf8[0] == ' ' || utf8[0] == '\t' || utf8[0] == '\r' ||
				 utf8[0] == '\n')) {
			is_space = true;
		}
		if (len == 2 && (uint8_t)utf8[0] == 0xC2 && (uint8_t)utf8[1] == 0xA0) {
			is_space = true; /* no-break space */
		}
		if (len == 0 && code < 32 && !f->type0) {
			/* control code without a glyph: ignore but keep position */
		}
		if (ts->mode != 3 && ts->mode != 7) {
			place_glyph(c, x, y, size_dev, x2 - x, y2 - y, utf8, len, is_space);
		} else {
			/* invisible text (OCR layers): still extract it, scanners rely on it */
			place_glyph(c, x, y, size_dev, x2 - x, y2 - y, utf8, len, is_space);
		}
		struct mat adv = {1, 0, 0, 1, tx, ty};

		c->tm = mat_mul(adv, c->tm);
		i += (size_t)nb;
	}
}

static void adjust_tj(struct ctx *c, float amount)
{
	float tx = -amount / 1000.0f * c->ts.size * c->ts.th;
	struct mat adv = {1, 0, 0, 1, tx, 0};

	if (c->ts.font != NULL && c->ts.font->vertical) {
		adv.e = 0;
		adv.f = -amount / 1000.0f * c->ts.size;
	}
	c->tm = mat_mul(adv, c->tm);
}

static void next_line(struct ctx *c, float tx, float ty)
{
	struct mat t = {1, 0, 0, 1, tx, ty};

	c->tlm = mat_mul(t, c->tlm);
	c->tm = c->tlm;
}

/* ---------------------------------------------------------------------- */
/* content stream                                                         */

static void run_content(struct ctx *c, const uint8_t *data, size_t len, struct pdf_obj *resources);

static void do_xobject(struct ctx *c, struct pdf_obj *resources, const char *name)
{
	struct pdf_obj *xobjs = pdf_get(c->doc, resources, "XObject");
	struct pdf_obj *xo = pdf_get(c->doc, xobjs, name);

	if (xo == NULL || xo->type != PDF_STREAM) {
		return;
	}
	if (!pdf_is_name(pdf_get(c->doc, xo, "Subtype"), "Form")) {
		return;
	}
	if (c->form_depth >= MAX_FORM_DEPTH) {
		return;
	}
	for (int i = 0; i < c->form_depth; i++) {
		if (c->form_path[i] == xo) {
			return; /* recursive form */
		}
	}
	uint8_t *data;
	size_t dlen;

	if (pdf_stream_decode(c->doc, xo, &data, &dlen) < 0) {
		return;
	}
	struct pdf_obj *res = pdf_get(c->doc, xo, "Resources");
	struct pdf_obj *mtx = pdf_get(c->doc, xo, "Matrix");

	if (res == NULL) {
		res = resources;
	}
	/* q */
	struct mat saved_ctm = c->ctm;
	struct text_state saved_ts = c->ts;
	struct mat saved_tm = c->tm, saved_tlm = c->tlm;
	bool saved_in_text = c->in_text;
	int saved_sp = c->gs_sp;

	if (mtx != NULL && mtx->type == PDF_ARRAY && mtx->arr.n >= 6) {
		struct mat m;

		m.a = pdf_num(pdf_array_get(c->doc, mtx, 0));
		m.b = pdf_num(pdf_array_get(c->doc, mtx, 1));
		m.c = pdf_num(pdf_array_get(c->doc, mtx, 2));
		m.d = pdf_num(pdf_array_get(c->doc, mtx, 3));
		m.e = pdf_num(pdf_array_get(c->doc, mtx, 4));
		m.f = pdf_num(pdf_array_get(c->doc, mtx, 5));
		c->ctm = mat_mul(m, c->ctm);
	}
	c->form_path[c->form_depth++] = xo;
	run_content(c, data, dlen, res);
	c->form_depth--;
	pdf_free(data);
	/* Q */
	c->ctm = saved_ctm;
	c->ts = saved_ts;
	c->tm = saved_tm;
	c->tlm = saved_tlm;
	c->in_text = saved_in_text;
	c->gs_sp = saved_sp;
}

static void skip_inline_image(struct pdf_lex *lx)
{
	/* consume the image dictionary up to ID */
	for (;;) {
		struct pdf_obj *o = pdf_parse_obj(lx, false, 0);

		if (o == NULL) {
			return;
		}
		bool id = pdf_is_op(o, "ID");

		pdf_obj_free(o);
		if (id) {
			break;
		}
	}
	/* one whitespace byte, then binary data until whitespace EI whitespace */
	if (lx->pos < lx->len) {
		lx->pos++;
	}
	while (lx->pos + 1 < lx->len) {
		if (lx->buf[lx->pos] == 'E' && lx->buf[lx->pos + 1] == 'I' &&
		    (lx->pos == 0 || pdf_is_white(lx->buf[lx->pos - 1])) &&
		    (lx->pos + 2 >= lx->len || pdf_is_white(lx->buf[lx->pos + 2]) ||
		     pdf_is_delim(lx->buf[lx->pos + 2]))) {
			lx->pos += 2;
			return;
		}
		lx->pos++;
	}
	lx->pos = lx->len;
}

static float num_at(struct pdf_obj **ops, int n, int i)
{
	return (i >= 0 && i < n) ? pdf_num(ops[i]) : 0.0f;
}

static void run_content(struct ctx *c, const uint8_t *data, size_t len, struct pdf_obj *resources)
{
	struct pdf_lex lx;
	struct pdf_obj *ops[OPERANDS];
	int nops = 0;
	long guard = 0;

	pdf_lex_init(&lx, data, len, 0);
	for (;;) {
		struct pdf_obj *o = pdf_parse_obj(&lx, false, 0);

		if (o == NULL) {
			break;
		}
		if (++guard > 5000000 || c->out.overflow) {
			pdf_obj_free(o);
			break;
		}
		if (o->type != PDF_OP) {
			if (nops == OPERANDS) {
				pdf_obj_free(ops[0]);
				memmove(ops, ops + 1, (OPERANDS - 1) * sizeof(ops[0]));
				nops--;
			}
			ops[nops++] = o;
			continue;
		}
		const char *op = o->op;

		switch (op[0]) {
		case 'B':
			if (strcmp(op, "BT") == 0) {
				c->tm = identity;
				c->tlm = identity;
				c->in_text = true;
			} else if (strcmp(op, "BI") == 0) {
				skip_inline_image(&lx);
			}
			break;
		case 'E':
			if (strcmp(op, "ET") == 0) {
				c->in_text = false;
			}
			break;
		case 'q':
			if (op[1] == 0) {
				if (c->gs_sp < GS_STACK) {
					c->ctm_stack[c->gs_sp] = c->ctm;
					c->ts_stack[c->gs_sp] = c->ts;
					c->gs_sp++;
				}
			}
			break;
		case 'Q':
			if (op[1] == 0) {
				if (c->gs_sp > 0) {
					c->gs_sp--;
					c->ctm = c->ctm_stack[c->gs_sp];
					c->ts = c->ts_stack[c->gs_sp];
				}
			}
			break;
		case 'c':
			if (strcmp(op, "cm") == 0 && nops >= 6) {
				struct mat m = {num_at(ops, nops, nops - 6), num_at(ops, nops, nops - 5),
						num_at(ops, nops, nops - 4), num_at(ops, nops, nops - 3),
						num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1)};

				c->ctm = mat_mul(m, c->ctm);
			}
			break;
		case 'T':
			switch (op[1]) {
			case 'f':
				if (nops >= 2 && ops[nops - 2]->type == PDF_NAME) {
					c->ts.font = get_font(c, resources, pdf_name_str(ops[nops - 2]));
					c->ts.size = num_at(ops, nops, nops - 1);
				} else if (nops >= 1 && pdf_is_num(ops[nops - 1])) {
					c->ts.size = num_at(ops, nops, nops - 1);
				}
				break;
			case 'd':
				if (nops >= 2) {
					next_line(c, num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
				}
				break;
			case 'D':
				if (nops >= 2) {
					c->ts.tl = -num_at(ops, nops, nops - 1);
					next_line(c, num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
				}
				break;
			case 'm':
				if (nops >= 6) {
					struct mat m = {num_at(ops, nops, nops - 6), num_at(ops, nops, nops - 5),
							num_at(ops, nops, nops - 4), num_at(ops, nops, nops - 3),
							num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1)};

					c->tlm = m;
					c->tm = m;
				}
				break;
			case '*':
				next_line(c, 0.0f, -c->ts.tl);
				break;
			case 'L':
				c->ts.tl = num_at(ops, nops, nops - 1);
				break;
			case 'c':
				c->ts.tc = num_at(ops, nops, nops - 1);
				break;
			case 'w':
				c->ts.tw = num_at(ops, nops, nops - 1);
				break;
			case 'z':
				c->ts.th = num_at(ops, nops, nops - 1) / 100.0f;
				if (c->ts.th == 0.0f) {
					c->ts.th = 1.0f;
				}
				break;
			case 's':
				c->ts.rise = num_at(ops, nops, nops - 1);
				break;
			case 'r':
				c->ts.mode = (int)pdf_int(nops ? ops[nops - 1] : NULL);
				break;
			case 'j':
				if (nops >= 1 && ops[nops - 1]->type == PDF_STRING) {
					show_text(c, ops[nops - 1]->str.data, ops[nops - 1]->str.len);
				}
				break;
			case 'J':
				if (nops >= 1 && ops[nops - 1]->type == PDF_ARRAY) {
					struct pdf_obj *arr = ops[nops - 1];

					for (size_t i = 0; i < arr->arr.n; i++) {
						struct pdf_obj *e = arr->arr.items[i];

						if (e->type == PDF_STRING) {
							show_text(c, e->str.data, e->str.len);
						} else if (pdf_is_num(e)) {
							adjust_tj(c, pdf_num(e));
						}
					}
				}
				break;
			default:
				break;
			}
			break;
		case '\'':
			if (op[1] == 0 && nops >= 1 && ops[nops - 1]->type == PDF_STRING) {
				next_line(c, 0.0f, -c->ts.tl);
				show_text(c, ops[nops - 1]->str.data, ops[nops - 1]->str.len);
			}
			break;
		case '"':
			if (op[1] == 0 && nops >= 3 && ops[nops - 1]->type == PDF_STRING) {
				c->ts.tw = num_at(ops, nops, nops - 3);
				c->ts.tc = num_at(ops, nops, nops - 2);
				next_line(c, 0.0f, -c->ts.tl);
				show_text(c, ops[nops - 1]->str.data, ops[nops - 1]->str.len);
			}
			break;
		case 'D':
			if (strcmp(op, "Do") == 0 && nops >= 1 && ops[nops - 1]->type == PDF_NAME) {
				do_xobject(c, resources, pdf_name_str(ops[nops - 1]));
			}
			break;
		case 'g':
			if (strcmp(op, "gs") == 0 && nops >= 1 && ops[nops - 1]->type == PDF_NAME) {
				/* ExtGState may set the font */
				struct pdf_obj *egs = pdf_get(c->doc, resources, "ExtGState");
				struct pdf_obj *g = pdf_get(c->doc, egs, pdf_name_str(ops[nops - 1]));
				struct pdf_obj *fnt = pdf_get(c->doc, g, "Font");

				if (fnt != NULL && fnt->type == PDF_ARRAY && fnt->arr.n == 2) {
					struct pdf_obj *fd = pdf_array_get(c->doc, fnt, 0);

					if (fd != NULL && fd->type == PDF_DICT) {
						struct pdf_font *f = pdf_doc_font_cache_get(c->doc, fd);

						if (f == NULL) {
							f = pdf_font_load(c->doc, fd);
							if (f != NULL) {
								pdf_doc_font_cache_put(c->doc, fd, f);
							}
						}
						if (f != NULL) {
							c->ts.font = f;
							c->ts.size = pdf_num(pdf_array_get(c->doc, fnt, 1));
						}
					}
				}
			}
			break;
		default:
			break;
		}
		pdf_obj_free(o);
		for (int i = 0; i < nops; i++) {
			pdf_obj_free(ops[i]);
		}
		nops = 0;
	}
	for (int i = 0; i < nops; i++) {
		pdf_obj_free(ops[i]);
	}
}

/* ---------------------------------------------------------------------- */
/* post processing                                                        */

/* Collapse runs of blank lines and strip trailing whitespace. */
static void tidy(struct outbuf *b)
{
	size_t r = 0, w = 0;
	int newlines = 0;

	while (r < b->n) {
		char ch = b->p[r++];

		if (ch == '\n') {
			newlines++;
			if (newlines <= 2) {
				/* drop trailing spaces before the newline */
				while (w > 0 && b->p[w - 1] == ' ') {
					w--;
				}
				if (w > 0) {
					b->p[w++] = '\n';
				}
			}
		} else {
			newlines = 0;
			b->p[w++] = ch;
		}
	}
	while (w > 0 && (b->p[w - 1] == '\n' || b->p[w - 1] == ' ')) {
		w--;
	}
	b->n = w;
	if (b->p != NULL) {
		b->p[w] = 0;
	}
}

int pdf_page_text(struct pdf_doc *doc, size_t index, char **out)
{
	struct pdf_obj *page = pdf_page(doc, index);

	*out = NULL;
	if (page == NULL) {
		return -2;
	}
	if (pdf_is_encrypted(doc)) {
		return -13;
	}
	struct ctx *c = pdf_calloc(sizeof(*c));

	if (c == NULL) {
		return -12;
	}
	c->doc = doc;
	c->ctm = identity;
	c->tm = identity;
	c->tlm = identity;
	c->ts.th = 1.0f;

	uint8_t *content;
	size_t clen;
	int rc = pdf_page_content(doc, page, &content, &clen);

	if (rc == 0) {
		struct pdf_obj *res = pdf_page_attr(doc, page, "Resources");

		run_content(c, content, clen, res);
		pdf_free(content);
	}
	tidy(&c->out);
	if (c->out.p == NULL) {
		c->out.p = pdf_malloc(1);
		if (c->out.p == NULL) {
			pdf_free(c);
			return -12;
		}
		c->out.p[0] = 0;
	}
	*out = c->out.p;
	pdf_free(c);
	return 0;
}

void pdf_text_free(char *text)
{
	pdf_free(text);
}
