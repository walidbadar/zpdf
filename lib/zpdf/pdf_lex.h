/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Tokenizer and object parser for PDF syntax (ISO 32000-1 section 7.2).
 * The same parser handles file-level objects, content streams and CMaps.
 */
#ifndef ZPDF_PDF_LEX_H
#define ZPDF_PDF_LEX_H

#include <zpdf/pdf_obj.h>

struct pdf_lex {
	const uint8_t *buf;
	size_t len;
	size_t pos;
};

enum pdf_tok_type {
	TOK_EOF = 0,
	TOK_NUM,
	TOK_NAME,
	TOK_STRING,
	TOK_ARR_OPEN,
	TOK_ARR_CLOSE,
	TOK_DICT_OPEN,
	TOK_DICT_CLOSE,
	TOK_BRACE_OPEN,
	TOK_BRACE_CLOSE,
	TOK_KEYWORD,
};

struct pdf_tok {
	enum pdf_tok_type type;
	const uint8_t *start; /* keyword text / raw token start */
	size_t len;
	bool is_int;
	int64_t ival;
	float fval;
	uint8_t *str; /* decoded name/string, owned by caller (pdf_free) */
	size_t slen;
};

static inline void pdf_lex_init(struct pdf_lex *lx, const uint8_t *buf, size_t len, size_t pos)
{
	lx->buf = buf;
	lx->len = len;
	lx->pos = pos < len ? pos : len;
}

bool pdf_is_white(uint8_t c);
bool pdf_is_delim(uint8_t c);
void pdf_lex_skip_ws(struct pdf_lex *lx); /* also skips comments */
int pdf_lex_next(struct pdf_lex *lx, struct pdf_tok *t);
void pdf_tok_free(struct pdf_tok *t);

/*
 * Parse one object. With allow_refs, "n g R" sequences become PDF_REF
 * objects. Keywords are returned as PDF_OP objects (true/false/null are
 * converted). Returns NULL at end of input.
 */
struct pdf_obj *pdf_parse_obj(struct pdf_lex *lx, bool allow_refs, int depth);

/* Parse a decimal number without libc (no locale, no double). */
bool pdf_parse_number(const uint8_t *s, size_t len, bool *is_int, int64_t *ival, float *fval);

#endif
