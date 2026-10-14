..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: GPL-3.0-only

Getting started
###############

Requirements
************

- Zephyr 4.5 (the manifest tracks ``main``) and the Zephyr SDK.
- The ``lvgl`` module for the reader, ``fatfs`` for the SD card and
  ``hal_espressif`` for the ESP32-S3. The manifest of this repository pulls
  in exactly these.
- For the ESP32-S3 target, the Espressif binary blobs:

  .. code-block:: sh

     west blobs fetch hal_espressif

- For ``native_sim``, the SDL2 development package (the window stands in
  for the display).
- The library itself needs nothing beyond libc and ``math.h``. A page
  render needs 3 bytes per pixel of the rendered region plus one byte per
  pixel for each active clip or soft mask, from the heap that
  ``pdf_malloc()`` uses; on Espressif SoCs with ``CONFIG_ESP_SPIRAM`` that is
  the external PSRAM.

Setting up a workspace
**********************

The repository is a west manifest repository and a Zephyr module at the
same time, laid out like Zephyr's `example-application
<https://github.com/zephyrproject-rtos/example-application>`_. Either create
a workspace from it:

.. code-block:: sh

   west init -m https://github.com/walidbadar/zpdf --mr main zpdf-workspace
   cd zpdf-workspace && west update

or build it inside an existing workspace by passing the repository as an
extra module:

.. code-block:: sh

   west build -b uedx32480035e_wb_a/esp32s3/procpu -d build/esp32s3 zpdf/app -- \
       -DZEPHYR_EXTRA_MODULES=$PWD/zpdf

If the workspace manifest does not list the ``fatfs`` module, add it to the
list: ``-DZEPHYR_EXTRA_MODULES="$PWD/zpdf;$PWD/modules/fs/fatfs"``.

To use only the library from another project, add the repository to that
project's west manifest or pass ``-DZEPHYR_EXTRA_MODULES=/path/to/zpdf`` and
enable ``CONFIG_ZPDF=y``.

Repository layout
*****************

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Path
     - Contents
   * - ``app/``
     - The reader application: LVGL UI, SD card storage, render worker,
       ``native_sim`` host storage and screenshots
   * - ``lib/zpdf/``
     - The library: parser, filters, fonts, images, rasterizer, renderer,
       text extraction, built-in fallback fonts
   * - ``include/zpdf/``
     - Public headers of the library
   * - ``tests/lib/zpdf/``
     - ztest suite of the library (``native_sim`` and ``qemu_cortex_m3``)
   * - ``tools/pdftext/``, ``tools/pdfrender/``
     - Host command line tools: text of a page, render of a page
   * - ``tools/mkfont/``
     - Generator of the built-in fallback fonts
   * - ``patches/``
     - Zephyr patch for the ESP32-S3 SD host (see :doc:`hardware`)
   * - ``doc/``
     - This documentation

Building the documentation
**************************

.. code-block:: sh

   pip install -r doc/requirements.txt
   sphinx-build -b html doc doc/_build/html

Then open ``doc/_build/html/index.html``.
