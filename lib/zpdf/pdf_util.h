/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef ZPDF_PDF_UTIL_H
#define ZPDF_PDF_UTIL_H

#include <stddef.h>
#include <stdint.h>

#if defined(__ZEPHYR__)
#include <zephyr/logging/log.h>
#define PDF_LOG_REGISTER() LOG_MODULE_DECLARE(zpdf, CONFIG_ZPDF_LOG_LEVEL)
#define PDF_ERR(...) LOG_ERR(__VA_ARGS__)
#define PDF_WRN(...) LOG_WRN(__VA_ARGS__)
#define PDF_INF(...) LOG_INF(__VA_ARGS__)
#else
#include <stdio.h>
#define PDF_LOG_REGISTER()
#define PDF_ERR(...) do { fprintf(stderr, "zpdf: E: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PDF_WRN(...) do { fprintf(stderr, "zpdf: W: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PDF_INF(...) do { } while (0)
#endif

/* Encode one code point as UTF-8; returns the number of bytes (1..4, 0 if invalid). */
static inline int pdf_utf8_put(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		if (cp >= 0xD800 && cp <= 0xDFFF) {
			cp = 0xFFFD;
		}
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (cp <= 0x10FFFF) {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

/* Decode one UTF-8 code point; returns bytes consumed (>=1). */
static inline int pdf_utf8_get(const char *s, size_t n, uint32_t *cp)
{
	uint8_t c = (uint8_t)s[0];

	if (c < 0x80 || n < 2) {
		*cp = c;
		return 1;
	}
	if ((c & 0xE0) == 0xC0 && n >= 2) {
		*cp = ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
		return 2;
	}
	if ((c & 0xF0) == 0xE0 && n >= 3) {
		*cp = ((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
		return 3;
	}
	if ((c & 0xF8) == 0xF0 && n >= 4) {
		*cp = ((c & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) |
		      (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
		return 4;
	}
	*cp = c;
	return 1;
}

#endif
