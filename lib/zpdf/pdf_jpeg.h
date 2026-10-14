/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Baseline (sequential Huffman) JPEG decoder for DCTDecode streams.
 */
#ifndef ZPDF_PDF_JPEG_H
#define ZPDF_PDF_JPEG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Decode to interleaved 8-bit samples: 1 (gray), 3 (RGB, converted from
 * YCbCr) or 4 (CMYK / YCCK converted to CMYK) components. *adobe is set
 * when an Adobe APP14 marker was present (such CMYK data is stored
 * inverted). Returns 0, or negative for unsupported (progressive, 12-bit,
 * arithmetic) or corrupt data.
 */
int pdf_jpeg_decode(const uint8_t *data, size_t len, uint8_t **out, int *w, int *h, int *ncomp,
		    bool *adobe);
/*
 * Like pdf_jpeg_decode but with the output reduced by @denom (1, 2, 4 or 8)
 * in each direction, computed in the DCT domain so large photographs can
 * be decoded at roughly their on-screen size with little memory.
 */
int pdf_jpeg_decode_scaled(const uint8_t *data, size_t len, int denom, uint8_t **out, int *w, int *h,
			   int *ncomp, bool *adobe);
/* Only the header: dimensions and component count. */
int pdf_jpeg_info(const uint8_t *data, size_t len, int *w, int *h, int *ncomp, bool *progressive);

#endif
