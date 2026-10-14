/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Image XObjects and inline images decoded to RGB plus alpha.
 */
#ifndef ZPDF_PDF_IMAGE_H
#define ZPDF_PDF_IMAGE_H

#include <zpdf/pdf_doc.h>

struct pdf_image {
	int w, h;
	uint8_t *rgb;   /* 3 bytes per pixel; NULL for a stencil mask */
	uint8_t *alpha; /* 1 byte per pixel, or NULL when opaque */
	bool is_mask;   /* stencil: paint the fill colour through alpha */
	bool unsupported; /* codec not available: draw a placeholder */
};

/*
 * Decode an image stream. @resources resolves named colour spaces (inline
 * images). Soft masks (/SMask), stencil masks (/Mask stream) and colour key
 * masks (/Mask array) become the alpha plane.
 */
struct pdf_image *pdf_image_load(struct pdf_doc *doc, struct pdf_obj *stream, struct pdf_obj *resources);
/*
 * As above, with the size the image will occupy on the device (pixels).
 * Images much larger than that are decoded or reduced to about twice the
 * hint, which bounds memory and time on small targets. 0 = no hint.
 */
struct pdf_image *pdf_image_load_hint(struct pdf_doc *doc, struct pdf_obj *stream, struct pdf_obj *resources,
				      int hint_w, int hint_h);
void pdf_image_free(struct pdf_image *img);

#endif
