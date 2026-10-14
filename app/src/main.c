/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * zpdf reader: PDF documents from the SD card on a 480x320 touch display.
 */
#include "storage.h"
#include "ui.h"
#include "worker.h"
#include "screenshot_host.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(zpdf_app, LOG_LEVEL_INF);

#if DT_HAS_COMPAT_STATUS_OKAY(gpio_keys)
/* The BOOT button turns the page (held together with touch, it goes back). */
static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_0 && evt->value == 1) {
		ui_request_next_page();
	}
}

INPUT_CALLBACK_DEFINE(NULL, button_cb, NULL);
#endif

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return 0;
	}
#if defined(CONFIG_ZPDF_APP_SD_TUNING)
	extern void sd_tuning_apply(void);

	sd_tuning_apply();
#endif
	int rc = storage_init();

	if (rc != 0) {
		LOG_WRN("storage unavailable (%d)", rc);
	}
	worker_init();
	ui_init();
	display_blanking_off(display);
	LOG_INF("zpdf reader ready");

	for (;;) {
		ui_poll();
		screenshot_poll();
		uint32_t next = lv_timer_handler();

		k_msleep(CLAMP(next, 1, 20));
	}
	return 0;
}
