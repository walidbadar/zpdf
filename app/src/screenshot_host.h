/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef ZPDF_APP_SCREENSHOT_HOST_H
#define ZPDF_APP_SCREENSHOT_HOST_H

#if defined(CONFIG_BOARD_NATIVE_SIM)
void screenshot_poll(void);
#else
static inline void screenshot_poll(void)
{
}
#endif

#endif
