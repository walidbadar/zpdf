/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef ZPDF_PDF_ENCODINGS_H
#define ZPDF_PDF_ENCODINGS_H

#include <stdint.h>

enum pdf_base_encoding {
	PDF_ENC_STANDARD,
	PDF_ENC_WINANSI,
	PDF_ENC_MACROMAN,
};

void pdf_encoding_fill(enum pdf_base_encoding enc, uint16_t *table);
uint32_t pdf_glyph_to_unicode(const char *name);

#endif

#include <stdbool.h>
/*
 * Widths (1/1000 em) for ASCII 0x20..0x7E of the standard font family that
 * best matches @base_font; NULL with *fixed set for monospaced fonts.
 */
const uint16_t *pdf_std_font_widths(const char *base_font, int flags, bool *fixed);
