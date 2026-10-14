/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Compact Font Format (Type 2 charstrings): FontFile3 subtypes Type1C and
 * CIDFontType0C, and the CFF table of OpenType fonts.
 */
#ifndef ZPDF_PDF_CFF_H
#define ZPDF_PDF_CFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cff_font;

struct cff_outline_cb {
	void (*move)(void *ud, float x, float y);
	void (*line)(void *ud, float x, float y);
	void (*cubic)(void *ud, float x1, float y1, float x2, float y2, float x3, float y3);
	void (*close)(void *ud);
};

struct cff_font *cff_open(const uint8_t *data, size_t len);
void cff_free(struct cff_font *f);
bool cff_is_cid(const struct cff_font *f);
uint16_t cff_nglyphs(const struct cff_font *f);
void cff_font_matrix(const struct cff_font *f, float m[6]);
uint16_t cff_gid_for_cid(const struct cff_font *f, uint32_t cid);
uint16_t cff_gid_for_name(const struct cff_font *f, const char *name);
/* Through the font's built-in encoding (standard encoding when none). */
uint16_t cff_gid_for_code(const struct cff_font *f, uint8_t code);
/* Outline with points transformed by m (font units as per the font matrix). */
int cff_glyph_outline(const struct cff_font *f, uint16_t gid, const float m[6],
		      const struct cff_outline_cb *cb, void *ud);

#endif
