..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: GPL-3.0-only

Hardware
########

VIEWE UEDX32480035E-WB-A
************************

- ESP32-S3 with 16 MB flash and 8 MB octal PSRAM
- 3.5" 480x320 ST7796S display with CHSC6X touch controller, used in
  landscape
- SD card slot on the SDMMC host, 4-bit bus
- The BOOT button turns the page

The board's overlay is
``app/boards/uedx32480035e_wb_a_esp32s3_procpu.overlay`` (display and touch
orientation, SD bus) and its configuration
``app/boards/uedx32480035e_wb_a_esp32s3_procpu.conf`` (caches, PSRAM, SD
card and FAT, LVGL flush thread).

If the picture is upside down for the way the board is mounted, change
``madctl`` to ``0xe8`` and use ``inverted-y`` instead of ``inverted-x`` for
the touch controller, as explained in the overlay.

Speed
*****

Rendering a letter size page at 480 px wide takes 20 to 150 ms on a desktop;
on the ESP32-S3 at 240 MHz expect seconds. The board configuration therefore
uses the largest caches the SoC offers (code runs from flash, every page
buffer lives in PSRAM, both go through the caches), runs the octal PSRAM and
the flash at 80 MHz, and compiles with ``-O3``. The text view extracts text
instead of rendering and is the fast way to read on the device.

SD card
*******

.. list-table::
   :header-rows: 1

   * - Signal
     - GPIO
   * - CLK
     - GPIO14
   * - CMD
     - GPIO17
   * - D0
     - GPIO16
   * - D1
     - GPIO18
   * - D2
     - GPIO15
   * - D3
     - GPIO21

The slot is routed through the GPIO matrix. With the 4-bit bus the data
lines only transfer reliably in default speed mode: reads fail with ``-EIO``
at 40 and 52 MHz and work at 25 MHz, while the 1-bit bus works up to 52 MHz
but is slower overall. The overlay therefore sets ``max-bus-freq`` to
25 MHz with ``bus-width = <4>``.

Two settings the Zephyr driver does not expose are applied by the
application before the card is initialised (``app/src/sd_tuning.c``,
``CONFIG_ZPDF_APP_SD_TUNING``):

- ``CONFIG_ZPDF_APP_SD_DRIVE_STRENGTH``: pad drive strength of the six slot
  pins (0 weakest to 3 strongest, -1 leaves the default).
- ``CONFIG_ZPDF_APP_SD_DIN_PHASE``: input delay phase of the SDMMC host,
  which high speed mode needs on boards whose slot goes through the GPIO
  matrix.

``patches/zephyr-sdhc-esp32-din-delay-phase.patch`` makes the input delay
phase a Kconfig option of Zephyr's ESP32 SD host driver and applies it
whenever the card clock changes. It is a contribution to Zephyr (and under
Zephyr's Apache-2.0 licence); apply it to the Zephyr tree when you want to
try high speed mode:

.. code-block:: sh

   git -C zephyr am ../zpdf/patches/zephyr-sdhc-esp32-din-delay-phase.patch

Other boards
************

The reader needs a display with touch behind Zephyr's display and input
APIs, a block device with a FAT file system for the documents (any disk
that ``CONFIG_ZPDF_APP_DISK_NAME`` names), and enough heap for the
document and the rendered tiles: a 480x320 tile at 300 % is about 1.7 MB
plus the renderer's masks, so a few megabytes of external RAM are
expected. The ``native_sim`` target
(``app/boards/native_sim_native_64.conf`` and ``.overlay``) shows the
minimum set of options for a new board.
