/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef ZPDF_APP_UI_H
#define ZPDF_APP_UI_H

#include <stdbool.h>

/* Build the screens and show the document list. Call with LVGL ready. */
void ui_init(void);

/* Requests from other threads (buttons); handled on the LVGL thread. */
void ui_request_next_page(void);
void ui_request_prev_page(void);

/* Set the page zoom in percent of fit-to-width (LVGL thread only). */
void ui_set_zoom(int pct);

/* Switch between the page view and the text view (LVGL thread only). */
void ui_set_text_view(bool text);

/* Run pending requests; call from the thread that runs lv_timer_handler(). */
void ui_poll(void);

#endif
