/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Font resources (section 9): code splitting, widths and Unicode mapping.
 */
#ifndef ZPDF_PDF_FONT_H
#define ZPDF_PDF_FONT_H

#include <zpdf/pdf_doc.h>

struct cmap_range {
	uint32_t lo, hi;
	uint32_t dst; /* CID, or Unicode code point of lo */
	uint8_t nbytes;
};

struct cmap_single {
	uint32_t code;
	uint8_t nbytes;
	char utf8[12]; /* multi character destinations (ligatures) */
};

struct codespace {
	uint32_t lo, hi;
	uint8_t nbytes;
};

struct cmap {
	struct codespace cs[16];
	int ncs;
	struct cmap_range *ranges;
	size_t nranges, ranges_cap;
	struct cmap_single *singles;
	size_t nsingles, singles_cap;
	bool identity;
};

struct width_range {
	uint32_t first, last;
	float w; /* text space units (already divided by 1000) */
};

struct pdf_font {
	bool type0;
	bool type3;
	bool vertical;
	bool has_tounicode;
	bool symbolic;

	struct cmap enc;       /* code -> CID (type0) */
	struct cmap tounicode; /* code -> Unicode */
	uint16_t simple_enc[256]; /* code -> Unicode for simple fonts, 0 = none */

	/* simple fonts */
	int first_char;
	float *widths; /* widths[first_char..first_char+nwidths) */
	int nwidths;
	float missing_width;
	/* composite fonts */
	struct width_range *wranges;
	size_t nwranges;
	float default_width;
	float type3_scale; /* FontMatrix[0] * 1000 */

	/* fallback through the embedded font program (loaded on first use) */
	struct pdf_doc *doc;
	struct pdf_obj *descriptor;
	struct pdf_obj *cid_to_gid; /* stream, or NULL for identity */
	bool program_tried;
	uint32_t *gid_unicode;
	size_t ngid;
	uint8_t *cid2gid;
	size_t cid2gid_len;
};

struct pdf_font *pdf_font_load(struct pdf_doc *doc, struct pdf_obj *fontdict);
void pdf_font_free(struct pdf_font *f);

/* Split the next character code off a string. Returns bytes consumed (>= 1). */
int pdf_font_next_code(const struct pdf_font *f, const uint8_t *s, size_t n, uint32_t *code,
		       uint32_t *cid);
/* Horizontal advance in text space units for a font size of 1. */
float pdf_font_width(const struct pdf_font *f, uint32_t code, uint32_t cid);
/* UTF-8 text for a code; returns bytes written (0 when unknown). out >= 16 bytes. */
int pdf_font_to_utf8(struct pdf_font *f, uint32_t code, uint32_t cid, char *out);

#endif
