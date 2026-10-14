# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zpdf_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# the inflate unit test uses the library's internal header
target_include_directories(app PRIVATE ${ZEPHYR_ZPDF_MODULE_DIR}/lib/zpdf)
