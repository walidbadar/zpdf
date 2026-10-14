/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Page rendering to an RGB bitmap.
 */
#ifndef ZPDF_PDF_RENDER_H
#define ZPDF_PDF_RENDER_H

#include <zpdf/pdf_doc.h>

struct pdf_bitmap {
	int w, h;
	int stride;   /* bytes per row */
	uint8_t *rgb; /* 3 bytes per pixel, from pdf_malloc() */
};

/* Page size in points after /Rotate. Returns 0 or negative. */
int pdf_page_size(struct pdf_doc *doc, size_t index, float *w, float *h);

/*
 * Render page @index at @scale device pixels per point into *out (its
 * buffer is allocated here; free with pdf_bitmap_free). Unsupported
 * features degrade gracefully: unknown image codecs draw grey boxes,
 * unknown shading types a flat average colour.
 */
int pdf_render_page(struct pdf_doc *doc, size_t index, float scale, struct pdf_bitmap *out);

/*
 * Render only the device rectangle (x0, y0, w, h) of the page scaled by
 * @scale: the same output as cropping pdf_render_page()'s bitmap, but with
 * memory proportional to the region. Used for zoomed, tiled viewing.
 */
int pdf_render_region(struct pdf_doc *doc, size_t index, float scale, int x0, int y0, int w, int h,
		      struct pdf_bitmap *out);
void pdf_bitmap_free(struct pdf_bitmap *bm);

#endif
