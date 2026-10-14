/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * ESP32-S3 SD slot tuning the Zephyr driver does not expose: pad drive
 * strength of the slot pins (the driver only sets it on its own pin path,
 * not through pinctrl) and the input delay phase of the SDMMC host, which
 * high speed mode (40 MHz) needs on boards whose slot goes through the
 * GPIO matrix. Applied after the driver's init and before the card is
 * initialised.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/gpio_ll.h>
#include <hal/sdmmc_ll.h>
#include <soc/sdmmc_struct.h>

LOG_MODULE_REGISTER(sd_tuning, LOG_LEVEL_INF);

static const int sd_pins[] = {14, 15, 16, 17, 18, 21}; /* CLK, D2, D0, CMD, D1, D3 */

void sd_tuning_apply(void)
{
#if CONFIG_ZPDF_APP_SD_DRIVE_STRENGTH >= 0
	for (size_t i = 0; i < ARRAY_SIZE(sd_pins); i++) {
		gpio_ll_set_drive_capability(&GPIO, sd_pins[i], (gpio_drive_cap_t)CONFIG_ZPDF_APP_SD_DRIVE_STRENGTH);
	}
	LOG_INF("SD pad drive strength %d", CONFIG_ZPDF_APP_SD_DRIVE_STRENGTH);
#endif
#if SDMMC_LL_DELAY_PHASE_SUPPORTED
	sdmmc_ll_set_din_delay_phase(&SDMMC, (sdmmc_ll_delay_phase_t)CONFIG_ZPDF_APP_SD_DIN_PHASE,
				     SDMMC_LL_SPEED_MODE_LS);
	LOG_INF("SD input delay phase %d", CONFIG_ZPDF_APP_SD_DIN_PHASE);
#endif
}
