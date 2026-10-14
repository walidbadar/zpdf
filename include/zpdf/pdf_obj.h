/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * In-memory PDF object model (ISO 32000-1 section 7.3).
 */
#ifndef ZPDF_PDF_OBJ_H
#define ZPDF_PDF_OBJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pdf_type {
	PDF_NULL = 0,
	PDF_BOOL,
	PDF_INT,
	PDF_REAL,
	PDF_STRING,
	PDF_NAME,
	PDF_ARRAY,
	PDF_DICT,
	PDF_REF,
	PDF_STREAM,
	PDF_OP, /* keyword / content stream operator */
};

struct pdf_obj;

struct pdf_dict_entry {
	char *key; /* name without the leading slash */
	struct pdf_obj *val;
};

struct pdf_obj {
	enum pdf_type type;
	union {
		bool b;
		int64_t i;
		float r;
		struct {
			uint8_t *data; /* string bytes (not NUL terminated) or name text (NUL terminated) */
			size_t len;
		} str;
		struct {
			struct pdf_obj **items;
			size_t n, cap;
		} arr;
		struct {
			struct pdf_dict_entry *items;
			size_t n, cap;
		} dict;
		struct {
			uint32_t num, gen;
		} ref;
		struct {
			struct pdf_obj *dict;
			const uint8_t *data; /* raw (still encoded) bytes, inside the file buffer */
			size_t len;
		} stream;
		char op[24]; /* content operators and CMap keywords ("begincodespacerange") */
	};
};

struct pdf_obj *pdf_obj_new(enum pdf_type type);
struct pdf_obj *pdf_obj_new_int(int64_t v);
struct pdf_obj *pdf_obj_new_real(float v);
struct pdf_obj *pdf_obj_new_name(const char *name, size_t len);
struct pdf_obj *pdf_obj_new_string(const uint8_t *data, size_t len); /* copies */
struct pdf_obj *pdf_obj_new_op(const char *op, size_t len);
void pdf_obj_free(struct pdf_obj *o);

int pdf_array_push(struct pdf_obj *arr, struct pdf_obj *item);       /* takes ownership */
int pdf_dict_put(struct pdf_obj *dict, const char *key, struct pdf_obj *val); /* takes ownership */
struct pdf_obj *pdf_dict_get_raw(const struct pdf_obj *dict_or_stream, const char *key);

static inline bool pdf_is_num(const struct pdf_obj *o)
{
	return o != NULL && (o->type == PDF_INT || o->type == PDF_REAL);
}

static inline float pdf_num(const struct pdf_obj *o)
{
	if (o == NULL) {
		return 0.0f;
	}
	if (o->type == PDF_INT) {
		return (float)o->i;
	}
	if (o->type == PDF_REAL) {
		return o->r;
	}
	return 0.0f;
}

static inline int64_t pdf_int(const struct pdf_obj *o)
{
	if (o == NULL) {
		return 0;
	}
	if (o->type == PDF_INT) {
		return o->i;
	}
	if (o->type == PDF_REAL) {
		return (int64_t)o->r;
	}
	return 0;
}

bool pdf_is_name(const struct pdf_obj *o, const char *name);
bool pdf_is_op(const struct pdf_obj *o, const char *op);
const char *pdf_name_str(const struct pdf_obj *o); /* "" when not a name */
size_t pdf_array_len(const struct pdf_obj *o);
struct pdf_obj *pdf_array_get_raw(const struct pdf_obj *o, size_t i);

#endif
