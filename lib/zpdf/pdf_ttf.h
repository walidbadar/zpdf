/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Minimal TrueType / OpenType parsing: the 'cmap' table, inverted to a
 * glyph index -> Unicode map. Used for fonts without a /ToUnicode CMap.
 */
#ifndef ZPDF_PDF_TTF_H
#define ZPDF_PDF_TTF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Build map[gid] = Unicode code point (0 when unknown) from a font program.
 * *map is pdf_malloc'd with *n entries. Returns the number of mapped
 * glyphs, 0 when the font has no usable cmap, negative on error.
 */
int pdf_ttf_gid_to_unicode(const uint8_t *data, size_t len, uint32_t **map, size_t *n);

#endif

/* ---- glyph outlines ------------------------------------------------- */

struct ttf_font {
	const uint8_t *data;
	size_t len;
	uint32_t glyf_off, glyf_len;
	uint32_t loca_off, loca_len;
	uint32_t cmap_off, cmap_len;
	uint32_t post_off, post_len;
	uint32_t hmtx_off, hmtx_len;
	uint16_t nglyphs;
	uint16_t upem;
	uint16_t nhmetrics;
	bool long_loca;
	bool has_glyf;
	/* cmap subtable offsets inside the cmap table (0 = absent) */
	uint32_t cmap31, cmap30, cmap10, cmap310;
};

struct ttf_outline_cb {
	void (*move)(void *ud, float x, float y);
	void (*line)(void *ud, float x, float y);
	void (*quad)(void *ud, float cx, float cy, float x, float y);
	void (*close)(void *ud);
};

/* Parse the table directory. Returns 0, or -1 when this is not a TrueType font with glyf. */
int ttf_open(struct ttf_font *f, const uint8_t *data, size_t len);
/* Look a code up in one of the cmap subtables: 31 (Unicode), 30 (symbol), 10 (Mac). 0 = none. */
uint16_t ttf_cmap_lookup(const struct ttf_font *f, int which, uint32_t code);
bool ttf_has_cmap(const struct ttf_font *f, int which);
/* Glyph index for a PostScript glyph name through the post table (0 when absent). */
uint16_t ttf_post_lookup(const struct ttf_font *f, const char *name);
/* Advance width in font units (used for fallback fonts without /Widths). */
uint16_t ttf_advance(const struct ttf_font *f, uint16_t gid);
/*
 * Emit the outline of a glyph. Points are transformed by m (a b c d e f),
 * so the caller passes font-units-to-device directly. Composite glyphs are
 * resolved. Returns 0, or -1 for an invalid glyph.
 */
int ttf_glyph_outline(const struct ttf_font *f, uint16_t gid, const float m[6],
		      const struct ttf_outline_cb *cb, void *ud);
