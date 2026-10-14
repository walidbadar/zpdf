/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_mem.h"
#include <string.h>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zpdf, CONFIG_ZPDF_LOG_LEVEL);
#if defined(CONFIG_ESP_SPIRAM)
#include <zephyr/multi_heap/shared_multi_heap.h>

void *pdf_malloc(size_t n)
{
	void *p = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, n ? n : 1);

	if (p == NULL) {
		LOG_WRN("PSRAM allocation of %zu bytes failed", n);
	}
	return p;
}

void *pdf_realloc(void *p, size_t n)
{
	if (p == NULL) {
		return pdf_malloc(n);
	}
	void *np = shared_multi_heap_realloc(SMH_REG_ATTR_EXTERNAL, p, n ? n : 1);

	if (np == NULL) {
		LOG_WRN("PSRAM reallocation to %zu bytes failed", n);
	}
	return np;
}

void pdf_free(void *p)
{
	if (p != NULL) {
		shared_multi_heap_free(p);
	}
}
#else
void *pdf_malloc(size_t n)
{
	return k_malloc(n ? n : 1);
}

void *pdf_realloc(void *p, size_t n)
{
	return k_realloc(p, n ? n : 1);
}

void pdf_free(void *p)
{
	k_free(p);
}
#endif
#else
#include <stdlib.h>

void *pdf_malloc(size_t n)
{
	return malloc(n ? n : 1);
}

void *pdf_realloc(void *p, size_t n)
{
	return realloc(p, n ? n : 1);
}

void pdf_free(void *p)
{
	free(p);
}
#endif

void *pdf_calloc(size_t n)
{
	void *p = pdf_malloc(n);

	if (p != NULL) {
		memset(p, 0, n);
	}
	return p;
}

char *pdf_strndup(const char *s, size_t n)
{
	char *d = pdf_malloc(n + 1);

	if (d != NULL) {
		memcpy(d, s, n);
		d[n] = '\0';
	}
	return d;
}
