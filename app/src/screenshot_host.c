/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * native_sim only: --shot=<file.ppm> writes the screen a moment after boot
 * (and after --open showed a page), for documentation and visual checks.
 * --page=<n>, --fit, --zoom=<pct> and --text choose what is shown of the
 * opened document.
 */
#include "screenshot_host.h"
#include "ui.h"
#include <zephyr/kernel.h>
#include <lvgl.h>
#include <posix_native_task.h>
#include <cmdline.h>
#include <stdio.h>
#include <stdint.h>
#include <zephyr/sys/printk.h>

static char *shot_path;
static unsigned int shot_delay_ms;
static unsigned int zoom_pct;
static unsigned int page_no;
static bool fit_page;
static bool text_view;
static bool done, zoomed;

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{
			.option = "shot",
			.name = "file.ppm",
			.type = 's',
			.dest = (void *)&shot_path,
			.descript = "Write a screenshot (binary PPM) and keep running",
		},
		{
			.option = "page",
			.name = "n",
			.type = 'u',
			.dest = (void *)&page_no,
			.descript = "Turn to this page (1-based) of the opened document before the screenshot",
		},
		{
			.option = "zoom",
			.name = "pct",
			.type = 'u',
			.dest = (void *)&zoom_pct,
			.descript = "Zoom the opened page to this percentage before the screenshot",
		},
		{
			.is_switch = true,
			.option = "fit",
			.type = 'b',
			.dest = (void *)&fit_page,
			.descript = "Fit the opened page to the view before the screenshot",
		},
		{
			.is_switch = true,
			.option = "text",
			.type = 'b',
			.dest = (void *)&text_view,
			.descript = "Show the text view of the opened page before the screenshot",
		},
		{
			.option = "shot-delay",
			.name = "ms",
			.type = 'u',
			.dest = (void *)&shot_delay_ms,
			.descript = "Delay before the screenshot (default 1500 ms)",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 10);

void screenshot_poll(void)
{
	static bool announced;

	if (!announced) {
		announced = true;
		/* Numeric options that were not given are left at UINT32_MAX */
		if (shot_delay_ms == UINT32_MAX) {
			shot_delay_ms = 1500;
		}
		if (zoom_pct == UINT32_MAX) {
			zoom_pct = 0;
		}
		if (page_no == UINT32_MAX || page_no == 0) {
			page_no = 1;
		}
		printk("screenshot: option %s, delay %u ms\n", shot_path ? shot_path : "(none)", shot_delay_ms);
	}
	/* One page turn per poll: the request is consumed by the next ui_poll() */
	if (page_no > 1 && k_uptime_get() >= shot_delay_ms / 4) {
		page_no--;
		ui_request_next_page();
	}
	if (!zoomed && (zoom_pct > 0 || fit_page || text_view) && k_uptime_get() >= shot_delay_ms / 2) {
		zoomed = true;
		if (text_view) {
			ui_set_text_view(true);
		} else {
			ui_set_zoom(fit_page ? 0 : (int)zoom_pct);
		}
	}
	if (done || shot_path == NULL || k_uptime_get() < shot_delay_ms) {
		return;
	}
	done = true;

	lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);

	if (buf == NULL) {
		printk("screenshot: snapshot failed\n");
		return;
	}
	FILE *fp = fopen(shot_path, "wb");

	if (fp == NULL) {
		printk("screenshot: cannot write %s\n", shot_path);
		lv_draw_buf_destroy(buf);
		return;
	}
	uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;

	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)buf->data + y * stride;

		for (uint32_t x = 0; x < w; x++) {
			/* LVGL RGB888 is stored as B, G, R */
			uint8_t px[3] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};

			fwrite(px, 1, 3, fp);
		}
	}
	fclose(fp);
	lv_draw_buf_destroy(buf);
	printk("screenshot: wrote %s (%ux%u)\n", shot_path, w, h);
}
