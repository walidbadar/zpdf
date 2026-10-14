/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Colour spaces (section 8.6) reduced to sRGB.
 */
#ifndef ZPDF_PDF_COLOR_H
#define ZPDF_PDF_COLOR_H

#include <zpdf/pdf_doc.h>
#include "pdf_func.h"

enum pdf_cs_type {
	PDF_CS_GRAY,
	PDF_CS_RGB,
	PDF_CS_CMYK,
	PDF_CS_LAB,
	PDF_CS_INDEXED,
	PDF_CS_SEPARATION, /* also DeviceN */
	PDF_CS_PATTERN,
};

struct pdf_colorspace {
	enum pdf_cs_type type;
	int ncomps;
	struct pdf_colorspace *base; /* Indexed base, Separation alternate, Pattern underlying */
	uint8_t *lookup;             /* Indexed: (hival + 1) * base->ncomps bytes */
	int hival;
	struct pdf_function *tint;
	float lab_range[4];
	float lab_wp[3];
	bool is_none; /* Separation /None: paints nothing */
	bool is_all;
	int refs; /* shared between graphics states by the renderer */
};

/*
 * @obj is a name or array; names other than the device spaces are looked
 * up in the /ColorSpace resources dictionary. Returns NULL when unknown.
 */
struct pdf_colorspace *pdf_colorspace_load(struct pdf_doc *doc, struct pdf_obj *obj,
					   struct pdf_obj *resources);
struct pdf_colorspace *pdf_colorspace_device(enum pdf_cs_type type);
void pdf_colorspace_free(struct pdf_colorspace *cs);
/* Initial colour of the space (black). */
void pdf_colorspace_initial(const struct pdf_colorspace *cs, float *comps);
/* Component range for image decoding: [0, max] per component. */
void pdf_colorspace_decode_default(const struct pdf_colorspace *cs, int bpc, float *decode);
void pdf_colorspace_to_rgb(const struct pdf_colorspace *cs, const float *comps, float rgb[3]);

#endif
