/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <zpdf/pdf_obj.h>
#include "pdf_mem.h"
#include <string.h>

struct pdf_obj *pdf_obj_new(enum pdf_type type)
{
	struct pdf_obj *o = pdf_calloc(sizeof(*o));

	if (o != NULL) {
		o->type = type;
	}
	return o;
}

struct pdf_obj *pdf_obj_new_int(int64_t v)
{
	struct pdf_obj *o = pdf_obj_new(PDF_INT);

	if (o != NULL) {
		o->i = v;
	}
	return o;
}

struct pdf_obj *pdf_obj_new_real(float v)
{
	struct pdf_obj *o = pdf_obj_new(PDF_REAL);

	if (o != NULL) {
		o->r = v;
	}
	return o;
}

struct pdf_obj *pdf_obj_new_name(const char *name, size_t len)
{
	struct pdf_obj *o = pdf_obj_new(PDF_NAME);

	if (o == NULL) {
		return NULL;
	}
	o->str.data = (uint8_t *)pdf_strndup(name, len);
	if (o->str.data == NULL) {
		pdf_free(o);
		return NULL;
	}
	o->str.len = len;
	return o;
}

struct pdf_obj *pdf_obj_new_string(const uint8_t *data, size_t len)
{
	struct pdf_obj *o = pdf_obj_new(PDF_STRING);

	if (o == NULL) {
		return NULL;
	}
	o->str.data = pdf_malloc(len + 1);
	if (o->str.data == NULL) {
		pdf_free(o);
		return NULL;
	}
	memcpy(o->str.data, data, len);
	o->str.data[len] = 0;
	o->str.len = len;
	return o;
}

struct pdf_obj *pdf_obj_new_op(const char *op, size_t len)
{
	struct pdf_obj *o = pdf_obj_new(PDF_OP);

	if (o == NULL) {
		return NULL;
	}
	if (len >= sizeof(o->op)) {
		len = sizeof(o->op) - 1;
	}
	memcpy(o->op, op, len);
	o->op[len] = 0;
	return o;
}

void pdf_obj_free(struct pdf_obj *o)
{
	if (o == NULL) {
		return;
	}
	switch (o->type) {
	case PDF_STRING:
	case PDF_NAME:
		pdf_free(o->str.data);
		break;
	case PDF_ARRAY:
		for (size_t i = 0; i < o->arr.n; i++) {
			pdf_obj_free(o->arr.items[i]);
		}
		pdf_free(o->arr.items);
		break;
	case PDF_DICT:
		for (size_t i = 0; i < o->dict.n; i++) {
			pdf_free(o->dict.items[i].key);
			pdf_obj_free(o->dict.items[i].val);
		}
		pdf_free(o->dict.items);
		break;
	case PDF_STREAM:
		pdf_obj_free(o->stream.dict);
		break;
	default:
		break;
	}
	pdf_free(o);
}

int pdf_array_push(struct pdf_obj *arr, struct pdf_obj *item)
{
	if (arr->arr.n == arr->arr.cap) {
		size_t ncap = arr->arr.cap ? arr->arr.cap * 2 : 8;
		struct pdf_obj **n = pdf_realloc(arr->arr.items, ncap * sizeof(*n));

		if (n == NULL) {
			pdf_obj_free(item);
			return -1;
		}
		arr->arr.items = n;
		arr->arr.cap = ncap;
	}
	arr->arr.items[arr->arr.n++] = item;
	return 0;
}

int pdf_dict_put(struct pdf_obj *dict, const char *key, struct pdf_obj *val)
{
	for (size_t i = 0; i < dict->dict.n; i++) {
		if (strcmp(dict->dict.items[i].key, key) == 0) {
			pdf_obj_free(dict->dict.items[i].val);
			dict->dict.items[i].val = val;
			return 0;
		}
	}
	if (dict->dict.n == dict->dict.cap) {
		size_t ncap = dict->dict.cap ? dict->dict.cap * 2 : 8;
		struct pdf_dict_entry *n = pdf_realloc(dict->dict.items, ncap * sizeof(*n));

		if (n == NULL) {
			pdf_obj_free(val);
			return -1;
		}
		dict->dict.items = n;
		dict->dict.cap = ncap;
	}
	char *k = pdf_strndup(key, strlen(key));

	if (k == NULL) {
		pdf_obj_free(val);
		return -1;
	}
	dict->dict.items[dict->dict.n].key = k;
	dict->dict.items[dict->dict.n].val = val;
	dict->dict.n++;
	return 0;
}

struct pdf_obj *pdf_dict_get_raw(const struct pdf_obj *d, const char *key)
{
	if (d == NULL) {
		return NULL;
	}
	if (d->type == PDF_STREAM) {
		d = d->stream.dict;
		if (d == NULL) {
			return NULL;
		}
	}
	if (d->type != PDF_DICT) {
		return NULL;
	}
	for (size_t i = 0; i < d->dict.n; i++) {
		if (strcmp(d->dict.items[i].key, key) == 0) {
			return d->dict.items[i].val;
		}
	}
	return NULL;
}

bool pdf_is_name(const struct pdf_obj *o, const char *name)
{
	return o != NULL && o->type == PDF_NAME && strcmp((const char *)o->str.data, name) == 0;
}

bool pdf_is_op(const struct pdf_obj *o, const char *op)
{
	return o != NULL && o->type == PDF_OP && strcmp(o->op, op) == 0;
}

const char *pdf_name_str(const struct pdf_obj *o)
{
	if (o == NULL || o->type != PDF_NAME) {
		return "";
	}
	return (const char *)o->str.data;
}

size_t pdf_array_len(const struct pdf_obj *o)
{
	return (o != NULL && o->type == PDF_ARRAY) ? o->arr.n : 0;
}

struct pdf_obj *pdf_array_get_raw(const struct pdf_obj *o, size_t i)
{
	if (o == NULL || o->type != PDF_ARRAY || i >= o->arr.n) {
		return NULL;
	}
	return o->arr.items[i];
}
