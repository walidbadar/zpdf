/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * DEFLATE (RFC 1951) decoder with zlib (RFC 1950) framing detection.
 * Written to be tolerant: a truncated or corrupt stream still returns the
 * bytes decoded so far, since damaged PDF content streams are common.
 */
#ifndef ZPDF_INFLATE_H
#define ZPDF_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Decompress @in. *out receives a buffer from pdf_malloc() that the caller
 * frees with pdf_free(); *out_len its length.
 *
 * Returns 0 on success, 1 when the stream was damaged but partial output is
 * available (out is still valid) and a negative value when nothing could be
 * decoded (out is NULL).
 */
int inflate_buf(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/*
 * Streaming variant: decoded bytes are handed to @sink as they are
 * produced (a 64 KB window is kept internally), so large streams never
 * exist in memory as a whole. The sink returns 0 to continue or negative
 * to stop. Same return values as inflate_buf (plus the sink's error).
 */
typedef int (*inflate_sink_fn)(void *ud, const uint8_t *data, size_t len);
int inflate_stream(const uint8_t *in, size_t in_len, inflate_sink_fn sink, void *ud);

#endif
