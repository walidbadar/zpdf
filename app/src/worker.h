/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Background worker that owns all access to the open document: page
 * rendering and text extraction run here so the UI thread stays
 * responsive. Results are polled from the UI thread.
 */
#ifndef ZPDF_APP_WORKER_H
#define ZPDF_APP_WORKER_H

#include <zpdf/pdf_doc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum worker_kind {
	WORKER_NONE = 0,
	WORKER_RENDER,
	WORKER_TEXT,
};

struct worker_result {
	enum worker_kind kind;
	size_t page;
	int zoom_pct;
	/* render: the tile (x, y, w, h) of the page at the requested scale */
	uint16_t *px; /* RGB565, from pdf_malloc; the receiver frees it */
	int x, y, w, h;
	/* text */
	char *text; /* from pdf_malloc; the receiver frees it */
	int error;
	uint32_t ms;
};

void worker_init(void);
/*
 * Replace any pending request: render the tile (x, y, w, h) of @page at
 * @scale device pixels per point. @zoom_pct only tags the result.
 */
void worker_request_render(struct pdf_doc *doc, size_t page, float scale, int zoom_pct, int x, int y,
			   int w, int h);
void worker_request_text(struct pdf_doc *doc, size_t page);
/* Fetch a finished result; returns false when none is ready. */
bool worker_take_result(struct worker_result *out);
/* Block until the worker is idle (before closing the document). */
void worker_drain(void);
bool worker_busy(void);

#endif
