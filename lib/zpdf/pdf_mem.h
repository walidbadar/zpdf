/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Allocator used by the PDF library. On Zephyr with PSRAM every allocation
 * goes to the external heap so multi-megabyte documents fit; elsewhere it is
 * the C library heap.
 */
#ifndef ZPDF_PDF_MEM_H
#define ZPDF_PDF_MEM_H

#include <stddef.h>

void *pdf_malloc(size_t n);
void *pdf_calloc(size_t n);
void *pdf_realloc(void *p, size_t n);
void pdf_free(void *p);
char *pdf_strndup(const char *s, size_t n);

#endif
