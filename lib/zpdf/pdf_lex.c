/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_lex.h"
#include "pdf_mem.h"
#include <string.h>

#define MAX_DEPTH 48

bool pdf_is_white(uint8_t c)
{
	return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0';
}

bool pdf_is_delim(uint8_t c)
{
	return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
	       c == '}' || c == '/' || c == '%';
}

void pdf_lex_skip_ws(struct pdf_lex *lx)
{
	while (lx->pos < lx->len) {
		uint8_t c = lx->buf[lx->pos];

		if (pdf_is_white(c)) {
			lx->pos++;
		} else if (c == '%') {
			while (lx->pos < lx->len && lx->buf[lx->pos] != '\n' && lx->buf[lx->pos] != '\r') {
				lx->pos++;
			}
		} else {
			break;
		}
	}
}

static int hexval(uint8_t c)
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

bool pdf_parse_number(const uint8_t *s, size_t len, bool *is_int, int64_t *ival, float *fval)
{
	size_t i = 0;
	bool neg = false, seen_digit = false, seen_dot = false;
	int64_t ip = 0;
	float frac = 0.0f, scale = 0.1f;

	/* PDFs in the wild contain "--5" and "-.5" and "5." */
	while (i < len && (s[i] == '+' || s[i] == '-')) {
		if (s[i] == '-') {
			neg = !neg;
		}
		i++;
	}
	for (; i < len; i++) {
		uint8_t c = s[i];

		if (c >= '0' && c <= '9') {
			seen_digit = true;
			if (!seen_dot) {
				if (ip < (int64_t)1 << 50) {
					ip = ip * 10 + (c - '0');
				}
			} else {
				frac += (c - '0') * scale;
				scale *= 0.1f;
			}
		} else if (c == '.' && !seen_dot) {
			seen_dot = true;
		} else {
			break;
		}
	}
	if (!seen_digit) {
		return false;
	}
	*is_int = !seen_dot;
	*ival = neg ? -ip : ip;
	*fval = (float)ip + frac;
	if (neg) {
		*fval = -*fval;
	}
	return true;
}

static int lex_literal_string(struct pdf_lex *lx, struct pdf_tok *t)
{
	size_t cap = 64, n = 0;
	uint8_t *out = pdf_malloc(cap);
	int depth = 1;

	if (out == NULL) {
		return -1;
	}
	lx->pos++; /* ( */
	while (lx->pos < lx->len && depth > 0) {
		uint8_t c = lx->buf[lx->pos++];

		if (c == '\\') {
			if (lx->pos >= lx->len) {
				break;
			}
			c = lx->buf[lx->pos++];
			switch (c) {
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case '\r':
				if (lx->pos < lx->len && lx->buf[lx->pos] == '\n') {
					lx->pos++;
				}
				continue;
			case '\n':
				continue;
			default:
				if (c >= '0' && c <= '7') {
					int v = c - '0';

					for (int k = 0; k < 2 && lx->pos < lx->len; k++) {
						uint8_t d = lx->buf[lx->pos];

						if (d < '0' || d > '7') {
							break;
						}
						v = v * 8 + (d - '0');
						lx->pos++;
					}
					c = (uint8_t)v;
				}
				break;
			}
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				break;
			}
		} else if (c == '\r') {
			/* EOL inside a string is always \n */
			if (lx->pos < lx->len && lx->buf[lx->pos] == '\n') {
				lx->pos++;
			}
			c = '\n';
		}
		if (n + 1 >= cap) {
			cap *= 2;
			uint8_t *o2 = pdf_realloc(out, cap);

			if (o2 == NULL) {
				pdf_free(out);
				return -1;
			}
			out = o2;
		}
		out[n++] = c;
	}
	out[n] = 0;
	t->type = TOK_STRING;
	t->str = out;
	t->slen = n;
	return 0;
}

static int lex_hex_string(struct pdf_lex *lx, struct pdf_tok *t)
{
	size_t cap = 64, n = 0;
	uint8_t *out = pdf_malloc(cap);
	int hi = -1;

	if (out == NULL) {
		return -1;
	}
	lx->pos++; /* < */
	while (lx->pos < lx->len) {
		uint8_t c = lx->buf[lx->pos++];

		if (c == '>') {
			break;
		}
		int v = hexval(c);

		if (v < 0) {
			continue;
		}
		if (hi < 0) {
			hi = v;
		} else {
			if (n + 1 >= cap) {
				cap *= 2;
				uint8_t *o2 = pdf_realloc(out, cap);

				if (o2 == NULL) {
					pdf_free(out);
					return -1;
				}
				out = o2;
			}
			out[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}
	if (hi >= 0) {
		if (n + 1 >= cap) {
			cap += 2;
			uint8_t *o2 = pdf_realloc(out, cap);

			if (o2 == NULL) {
				pdf_free(out);
				return -1;
			}
			out = o2;
		}
		out[n++] = (uint8_t)(hi << 4);
	}
	out[n] = 0;
	t->type = TOK_STRING;
	t->str = out;
	t->slen = n;
	return 0;
}

static int lex_name(struct pdf_lex *lx, struct pdf_tok *t)
{
	size_t cap = 32, n = 0;
	uint8_t *out = pdf_malloc(cap);

	if (out == NULL) {
		return -1;
	}
	lx->pos++; /* / */
	while (lx->pos < lx->len) {
		uint8_t c = lx->buf[lx->pos];

		if (pdf_is_white(c) || pdf_is_delim(c)) {
			break;
		}
		lx->pos++;
		if (c == '#' && lx->pos + 1 < lx->len) {
			int h = hexval(lx->buf[lx->pos]), l = hexval(lx->buf[lx->pos + 1]);

			if (h >= 0 && l >= 0) {
				c = (uint8_t)((h << 4) | l);
				lx->pos += 2;
			}
		}
		if (n + 1 >= cap) {
			cap *= 2;
			uint8_t *o2 = pdf_realloc(out, cap);

			if (o2 == NULL) {
				pdf_free(out);
				return -1;
			}
			out = o2;
		}
		out[n++] = c;
	}
	out[n] = 0;
	t->type = TOK_NAME;
	t->str = out;
	t->slen = n;
	return 0;
}

int pdf_lex_next(struct pdf_lex *lx, struct pdf_tok *t)
{
	memset(t, 0, sizeof(*t));
	pdf_lex_skip_ws(lx);
	if (lx->pos >= lx->len) {
		t->type = TOK_EOF;
		return 0;
	}
	uint8_t c = lx->buf[lx->pos];

	t->start = lx->buf + lx->pos;
	t->len = 1;
	switch (c) {
	case '[':
		lx->pos++;
		t->type = TOK_ARR_OPEN;
		return 0;
	case ']':
		lx->pos++;
		t->type = TOK_ARR_CLOSE;
		return 0;
	case '{':
		lx->pos++;
		t->type = TOK_BRACE_OPEN;
		return 0;
	case '}':
		lx->pos++;
		t->type = TOK_BRACE_CLOSE;
		return 0;
	case '<':
		if (lx->pos + 1 < lx->len && lx->buf[lx->pos + 1] == '<') {
			lx->pos += 2;
			t->type = TOK_DICT_OPEN;
			t->len = 2;
			return 0;
		}
		return lex_hex_string(lx, t);
	case '>':
		if (lx->pos + 1 < lx->len && lx->buf[lx->pos + 1] == '>') {
			lx->pos += 2;
			t->type = TOK_DICT_CLOSE;
			t->len = 2;
			return 0;
		}
		/* stray '>' - skip it */
		lx->pos++;
		return pdf_lex_next(lx, t);
	case '(':
		return lex_literal_string(lx, t);
	case ')':
		lx->pos++;
		return pdf_lex_next(lx, t);
	case '/':
		return lex_name(lx, t);
	default:
		break;
	}

	size_t start = lx->pos;

	while (lx->pos < lx->len && !pdf_is_white(lx->buf[lx->pos]) && !pdf_is_delim(lx->buf[lx->pos])) {
		lx->pos++;
	}
	t->start = lx->buf + start;
	t->len = lx->pos - start;
	if (t->len == 0) {
		/* unreachable in practice, guard against infinite loops */
		lx->pos++;
		return pdf_lex_next(lx, t);
	}
	if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
		if (pdf_parse_number(t->start, t->len, &t->is_int, &t->ival, &t->fval)) {
			t->type = TOK_NUM;
			return 0;
		}
	}
	t->type = TOK_KEYWORD;
	return 0;
}

void pdf_tok_free(struct pdf_tok *t)
{
	pdf_free(t->str);
	t->str = NULL;
}

static struct pdf_obj *obj_from_tok(struct pdf_tok *t)
{
	struct pdf_obj *o = NULL;

	switch (t->type) {
	case TOK_NUM:
		o = t->is_int ? pdf_obj_new_int(t->ival) : pdf_obj_new_real(t->fval);
		break;
	case TOK_NAME:
		o = pdf_obj_new(PDF_NAME);
		if (o != NULL) {
			o->str.data = t->str;
			o->str.len = t->slen;
			t->str = NULL;
		}
		break;
	case TOK_STRING:
		o = pdf_obj_new(PDF_STRING);
		if (o != NULL) {
			o->str.data = t->str;
			o->str.len = t->slen;
			t->str = NULL;
		}
		break;
	case TOK_KEYWORD:
		if (t->len == 4 && memcmp(t->start, "true", 4) == 0) {
			o = pdf_obj_new(PDF_BOOL);
			if (o != NULL) {
				o->b = true;
			}
		} else if (t->len == 5 && memcmp(t->start, "false", 5) == 0) {
			o = pdf_obj_new(PDF_BOOL);
		} else if (t->len == 4 && memcmp(t->start, "null", 4) == 0) {
			o = pdf_obj_new(PDF_NULL);
		} else {
			o = pdf_obj_new_op((const char *)t->start, t->len);
		}
		break;
	default:
		break;
	}
	pdf_tok_free(t);
	return o;
}

struct pdf_obj *pdf_parse_obj(struct pdf_lex *lx, bool allow_refs, int depth)
{
	struct pdf_tok t;

	if (depth > MAX_DEPTH) {
		return NULL;
	}
	if (pdf_lex_next(lx, &t) < 0) {
		return NULL;
	}
	switch (t.type) {
	case TOK_EOF:
		return NULL;
	case TOK_ARR_OPEN: {
		struct pdf_obj *arr = pdf_obj_new(PDF_ARRAY);

		if (arr == NULL) {
			return NULL;
		}
		for (;;) {
			pdf_lex_skip_ws(lx);
			if (lx->pos >= lx->len) {
				break;
			}
			if (lx->buf[lx->pos] == ']') {
				lx->pos++;
				break;
			}
			/* a dict close inside an array means a broken file: stop */
			if (lx->buf[lx->pos] == '>' && lx->pos + 1 < lx->len && lx->buf[lx->pos + 1] == '>') {
				break;
			}
			struct pdf_obj *item = pdf_parse_obj(lx, allow_refs, depth + 1);

			if (item == NULL) {
				break;
			}
			if (item->type == PDF_OP) {
				/* "endobj"/"endstream" inside an array: unterminated array */
				if (pdf_is_op(item, "endobj") || pdf_is_op(item, "endstream") ||
				    pdf_is_op(item, "stream")) {
					pdf_obj_free(item);
					break;
				}
			}
			if (pdf_array_push(arr, item) < 0) {
				break;
			}
		}
		return arr;
	}
	case TOK_ARR_CLOSE:
	case TOK_DICT_CLOSE:
	case TOK_BRACE_OPEN:
	case TOK_BRACE_CLOSE:
		/* unbalanced: return as a no-op operator so callers can keep going */
		return pdf_obj_new_op((const char *)t.start, t.len);
	case TOK_DICT_OPEN: {
		struct pdf_obj *dict = pdf_obj_new(PDF_DICT);

		if (dict == NULL) {
			return NULL;
		}
		for (;;) {
			struct pdf_tok k;

			if (pdf_lex_next(lx, &k) < 0 || k.type == TOK_EOF) {
				break;
			}
			if (k.type == TOK_DICT_CLOSE) {
				break;
			}
			if (k.type != TOK_NAME) {
				/* keys must be names; skip junk values */
				if (k.type == TOK_KEYWORD &&
				    ((k.len == 6 && memcmp(k.start, "endobj", 6) == 0) ||
				     (k.len == 6 && memcmp(k.start, "stream", 6) == 0))) {
					lx->pos -= k.len;
					pdf_tok_free(&k);
					break;
				}
				pdf_tok_free(&k);
				continue;
			}
			struct pdf_obj *val = pdf_parse_obj(lx, allow_refs, depth + 1);

			if (val == NULL) {
				pdf_tok_free(&k);
				break;
			}
			if (val->type == PDF_OP) {
				/* ">>" or junk: value missing */
				bool close = strcmp(val->op, ">>") == 0;

				pdf_obj_free(val);
				pdf_tok_free(&k);
				if (close) {
					break;
				}
				continue;
			}
			pdf_dict_put(dict, (const char *)k.str, val);
			pdf_tok_free(&k);
		}
		return dict;
	}
	case TOK_NUM:
		if (allow_refs && t.is_int && t.ival >= 0) {
			size_t save = lx->pos;
			struct pdf_tok t2, t3;

			if (pdf_lex_next(lx, &t2) == 0 && t2.type == TOK_NUM && t2.is_int && t2.ival >= 0) {
				size_t save2 = lx->pos;

				if (pdf_lex_next(lx, &t3) == 0 && t3.type == TOK_KEYWORD && t3.len == 1 &&
				    t3.start[0] == 'R') {
					struct pdf_obj *ref = pdf_obj_new(PDF_REF);

					pdf_tok_free(&t2);
					pdf_tok_free(&t3);
					if (ref != NULL) {
						ref->ref.num = (uint32_t)t.ival;
						ref->ref.gen = (uint32_t)t2.ival;
					}
					return ref;
				}
				pdf_tok_free(&t3);
				lx->pos = save2;
			}
			pdf_tok_free(&t2);
			lx->pos = save;
		}
		return obj_from_tok(&t);
	default:
		return obj_from_tok(&t);
	}
}
