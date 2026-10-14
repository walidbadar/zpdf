# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zpdf_reader LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/ui.c
  src/worker.c
)

# Where the documents come from: the FAT file system (SD card) on hardware,
# files of the host named on the command line on native_sim.
target_sources_ifdef(CONFIG_FILE_SYSTEM app PRIVATE src/storage_fs.c)
target_sources_ifdef(CONFIG_ZPDF_APP_SD_TUNING app PRIVATE src/sd_tuning.c)
target_sources_ifdef(CONFIG_BOARD_NATIVE_SIM app PRIVATE src/storage_host.c src/screenshot_host.c)
