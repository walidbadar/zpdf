/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef ZPDF_PDF_FILTER_H
#define ZPDF_PDF_FILTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Each decoder returns a pdf_malloc'd buffer in *out; negative on failure. */
int pdf_lzw_decode(const uint8_t *in, size_t n, int early, uint8_t **out, size_t *out_len);
int pdf_a85_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int pdf_ahx_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int pdf_rl_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
/* In place; returns the new length. */
size_t pdf_apply_predictor(uint8_t *data, size_t n, int predictor, int colors, int bpc, int columns);

#endif
