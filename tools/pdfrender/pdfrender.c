/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * pdfrender FILE PAGE OUT.ppm [WIDTH]: render one page (1-based) to a
 * binary PPM, WIDTH pixels wide (default 480).
 */
#include <zpdf/pdf_doc.h>
#include <zpdf/pdf_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s file.pdf page out.ppm [width]\n", argv[0]);
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
	size_t page = (size_t)atoi(argv[2]) - 1;
	int width = argc > 4 ? atoi(argv[4]) : 480;
	float pw, ph;

	if (pdf_page_size(doc, page, &pw, &ph) < 0) {
		fprintf(stderr, "no such page\n");
		return 1;
	}
	float scale = (float)width / pw;
	struct pdf_bitmap bm;
	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	int rc = pdf_render_page(doc, page, scale, &bm);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (rc < 0) {
		fprintf(stderr, "render failed: %d\n", rc);
		return 1;
	}
	fprintf(stderr, "page %zu: %dx%d in %.0f ms\n", page + 1, bm.w, bm.h,
		(t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	FILE *out = fopen(argv[3], "wb");

	if (out == NULL) {
		perror(argv[3]);
		return 1;
	}
	fprintf(out, "P6\n%d %d\n255\n", bm.w, bm.h);
	for (int y = 0; y < bm.h; y++) {
		fwrite(bm.rgb + (size_t)y * (size_t)bm.stride, 1, (size_t)bm.w * 3, out);
	}
	fclose(out);
	pdf_bitmap_free(&bm);
	pdf_close(doc);
	free(buf);
	return 0;
}
