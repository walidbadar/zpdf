/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "worker.h"
#include <zpdf/pdf_render.h>
#include <zpdf/pdf_text.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

void *pdf_malloc(size_t n);
void *pdf_realloc(void *p, size_t n);
void pdf_free(void *p);

LOG_MODULE_REGISTER(worker, LOG_LEVEL_INF);

K_THREAD_STACK_DEFINE(worker_stack, CONFIG_ZPDF_APP_RENDER_STACK_SIZE);

static struct k_work_q queue;
static struct k_work work;
static struct k_mutex lock;

struct request {
	enum worker_kind kind;
	struct pdf_doc *doc;
	size_t page;
	int zoom_pct;
	float scale;
	int x, y, w, h;
};

static struct request pending;
static bool pending_valid;
static struct worker_result result;
static bool result_valid;
static atomic_t busy;

static void free_result(struct worker_result *r)
{
	pdf_free(r->px);
	pdf_free(r->text);
	r->px = NULL;
	r->text = NULL;
}

static void handler(struct k_work *w)
{
	ARG_UNUSED(w);
	for (;;) {
		struct request req;

		k_mutex_lock(&lock, K_FOREVER);
		if (!pending_valid) {
			atomic_set(&busy, 0);
			k_mutex_unlock(&lock);
			return;
		}
		req = pending;
		pending_valid = false;
		k_mutex_unlock(&lock);

		struct worker_result r = {.kind = req.kind, .page = req.page, .zoom_pct = req.zoom_pct};
		int64_t t0 = k_uptime_get();

		if (req.kind == WORKER_RENDER) {
			{
				struct pdf_bitmap bm;

				r.x = req.x;
				r.y = req.y;
				r.error = pdf_render_region(req.doc, req.page, req.scale, req.x, req.y, req.w, req.h, &bm);
				if (r.error != 0) {
					LOG_WRN("render failed: scale %d/1000 region %d,%d %dx%d", (int)(req.scale * 1000),
						req.x, req.y, req.w, req.h);
				}
				if (r.error == 0) {
					/*
					 * Convert to RGB565 in place: the 2-byte pixel i is written
					 * at offset 2i while the 3-byte source sits at 3i, so a
					 * forward pass never overwrites unread data. No second
					 * buffer is needed at the peak of memory use.
					 */
					size_t n = (size_t)bm.w * (size_t)bm.h;
					uint16_t *dst = (uint16_t *)bm.rgb;

					for (size_t i = 0; i < n; i++) {
						const uint8_t *src = bm.rgb + i * 3;
						uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) |
									(src[2] >> 3));

						dst[i] = v;
					}
					uint16_t *shrunk = pdf_realloc(bm.rgb, n * 2);

					r.px = shrunk != NULL ? shrunk : dst;
					r.w = bm.w;
					r.h = bm.h;
				}
			}
		} else if (req.kind == WORKER_TEXT) {
			r.error = pdf_page_text(req.doc, req.page, &r.text);
		}
		r.ms = (uint32_t)(k_uptime_get() - t0);
		LOG_INF("%s page %u (%d%%, %dx%d at %d,%d) done in %u ms (%d)",
			req.kind == WORKER_RENDER ? "render" : "text", (unsigned)req.page + 1, req.zoom_pct, r.w, r.h,
			r.x, r.y, r.ms, r.error);

		k_mutex_lock(&lock, K_FOREVER);
		if (result_valid) {
			free_result(&result);
		}
		result = r;
		result_valid = true;
		k_mutex_unlock(&lock);
	}
}

void worker_init(void)
{
	k_mutex_init(&lock);
	k_work_init(&work, handler);
	k_work_queue_init(&queue);
	k_work_queue_start(&queue, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
			   CONFIG_ZPDF_APP_RENDER_PRIORITY, NULL);
	k_thread_name_set(k_work_queue_thread_get(&queue), "pdf_worker");
}

static void submit(const struct request *req)
{
	k_mutex_lock(&lock, K_FOREVER);
	pending = *req;
	pending_valid = true;
	atomic_set(&busy, 1);
	k_mutex_unlock(&lock);
	k_work_submit_to_queue(&queue, &work);
}

void worker_request_render(struct pdf_doc *doc, size_t page, float scale, int zoom_pct, int x, int y,
			   int w, int h)
{
	struct request req = {.kind = WORKER_RENDER, .doc = doc, .page = page, .zoom_pct = zoom_pct,
			      .scale = scale, .x = x, .y = y, .w = w, .h = h};

	submit(&req);
}

void worker_request_text(struct pdf_doc *doc, size_t page)
{
	struct request req = {.kind = WORKER_TEXT, .doc = doc, .page = page, .zoom_pct = 100};

	submit(&req);
}

bool worker_take_result(struct worker_result *out)
{
	bool got = false;

	k_mutex_lock(&lock, K_FOREVER);
	if (result_valid) {
		*out = result;
		result_valid = false;
		got = true;
	}
	k_mutex_unlock(&lock);
	return got;
}

void worker_drain(void)
{
	struct k_work_sync sync;

	k_mutex_lock(&lock, K_FOREVER);
	pending_valid = false;
	k_mutex_unlock(&lock);
	k_work_flush(&work, &sync);
	k_mutex_lock(&lock, K_FOREVER);
	if (result_valid) {
		free_result(&result);
		result_valid = false;
	}
	k_mutex_unlock(&lock);
}

bool worker_busy(void)
{
	return atomic_get(&busy) != 0;
}
