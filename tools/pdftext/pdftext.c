/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * pdftext FILE [PAGE]: print the text of one page (1-based) or of all pages.
 */
#include <zpdf/pdf_doc.h>
#include <zpdf/pdf_text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_obj(struct pdf_doc *doc, struct pdf_obj *o, int depth)
{
	if (o == NULL) {
		printf("null");
		return;
	}
	switch (o->type) {
	case PDF_NULL: printf("null"); break;
	case PDF_BOOL: printf(o->b ? "true" : "false"); break;
	case PDF_INT: printf("%lld", (long long)o->i); break;
	case PDF_REAL: printf("%g", o->r); break;
	case PDF_STRING: printf("(%.*s)", (int)(o->str.len > 40 ? 40 : o->str.len), o->str.data); break;
	case PDF_NAME: printf("/%s", o->str.data); break;
	case PDF_REF: printf("%u %u R", o->ref.num, o->ref.gen); break;
	case PDF_OP: printf("<op %s>", o->op); break;
	case PDF_ARRAY:
		printf("[");
		for (size_t i = 0; i < o->arr.n; i++) {
			if (i) printf(" ");
			dump_obj(doc, depth > 0 ? pdf_resolve(doc, o->arr.items[i]) : o->arr.items[i], depth - 1);
		}
		printf("]");
		break;
	case PDF_DICT:
	case PDF_STREAM: {
		struct pdf_obj *d = o->type == PDF_STREAM ? o->stream.dict : o;
		printf("<<");
		for (size_t i = 0; i < d->dict.n; i++) {
			printf(" /%s ", d->dict.items[i].key);
			dump_obj(doc, depth > 0 ? pdf_resolve(doc, d->dict.items[i].val) : d->dict.items[i].val, depth - 1);
		}
		printf(" >>%s", o->type == PDF_STREAM ? " stream" : "");
		break;
	}
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.pdf [page]\n", argv[0]);
		return 2;
	}
	FILE *fp = fopen(argv[1], "rb");

	if (fp == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);

	fseek(fp, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)len + 1);

	if (buf == NULL || fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		fprintf(stderr, "read failed\n");
		return 1;
	}
	fclose(fp);

	char err[64];
	struct pdf_doc *doc = pdf_open(buf, (size_t)len, err, sizeof(err));

	if (doc == NULL) {
		fprintf(stderr, "open failed: %s\n", err);
		return 1;
	}
	size_t n = pdf_page_count(doc);
	char *title = pdf_info_string(doc, "Title");

	fprintf(stderr, "%zu pages%s%s%s\n", n, title ? ", title: " : "", title ? title : "",
		pdf_is_encrypted(doc) ? " (encrypted)" : "");
	pdf_text_free(title);

	size_t first = 0, last = n;

	if (argc > 3 && strcmp(argv[3], "--fonts") == 0) {
		struct pdf_obj *page = pdf_page(doc, (size_t)atoi(argv[2]) - 1);
		struct pdf_obj *res = pdf_page_attr(doc, page, "Resources");
		struct pdf_obj *fonts = pdf_get(doc, res, "Font");

		if (fonts != NULL && fonts->type == PDF_DICT) {
			for (size_t i = 0; i < fonts->dict.n; i++) {
				printf("/%s: ", fonts->dict.items[i].key);
				dump_obj(doc, pdf_resolve(doc, fonts->dict.items[i].val), 2);
				printf("\n");
			}
		}
		pdf_close(doc);
		free(buf);
		return 0;
	}
	if (argc > 3 && strcmp(argv[3], "--obj") == 0) {
		struct pdf_obj ref = {.type = PDF_REF, .ref = {(uint32_t)atoi(argv[2]), 0}};

		dump_obj(doc, pdf_resolve(doc, &ref), argc > 4 ? atoi(argv[4]) : 2);
		printf("\n");
		pdf_close(doc);
		free(buf);
		return 0;
	}
	if (argc > 3 && strcmp(argv[3], "--xobjects") == 0) {
		struct pdf_obj *page = pdf_page(doc, (size_t)atoi(argv[2]) - 1);
		struct pdf_obj *res = pdf_page_attr(doc, page, "Resources");
		struct pdf_obj *xo = pdf_get(doc, res, "XObject");
		struct pdf_obj *sh = pdf_get(doc, res, "Shading");
		struct pdf_obj *cs = pdf_get(doc, res, "ColorSpace");
		struct pdf_obj *gs = pdf_get(doc, res, "ExtGState");
		struct pdf_obj *pat = pdf_get(doc, res, "Pattern");
		struct pdf_obj *lists[] = {xo, sh, cs, gs, pat};
		const char *names[] = {"XObject", "Shading", "ColorSpace", "ExtGState", "Pattern"};

		for (int k = 0; k < 5; k++) {
			if (lists[k] == NULL || lists[k]->type != PDF_DICT) {
				continue;
			}
			for (size_t i = 0; i < lists[k]->dict.n; i++) {
				printf("%s /%s: ", names[k], lists[k]->dict.items[i].key);
				dump_obj(doc, pdf_resolve(doc, lists[k]->dict.items[i].val), 1);
				printf("\n");
			}
		}
		pdf_close(doc);
		free(buf);
		return 0;
	}
	if (argc > 3 && strcmp(argv[3], "--dump") == 0) {
		uint8_t *content;
		size_t clen;

		if (pdf_page_content(doc, pdf_page(doc, (size_t)atoi(argv[2]) - 1), &content, &clen) == 0) {
			fwrite(content, 1, clen, stdout);
			pdf_text_free((char *)content);
		}
		pdf_close(doc);
		free(buf);
		return 0;
	}

	if (argc > 2) {
		first = (size_t)atoi(argv[2]) - 1;
		last = first + 1;
		if (first >= n) {
			fprintf(stderr, "no such page\n");
			return 1;
		}
	}
	for (size_t i = first; i < last; i++) {
		char *text;
		int rc = pdf_page_text(doc, i, &text);

		if (rc < 0) {
			fprintf(stderr, "page %zu: error %d\n", i + 1, rc);
			continue;
		}
		if (last - first > 1) {
			printf("=== page %zu ===\n", i + 1);
		}
		printf("%s\n", text);
		pdf_text_free(text);
	}
	pdf_close(doc);
	free(buf);
	return 0;
}
