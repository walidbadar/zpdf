..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: GPL-3.0-only

Architecture
############

Layers
******

.. code-block:: none

   +--------------------------------------------------------------+
   |  app/          LVGL UI, document list, render worker,        |
   |                SD card / host storage, screenshots           |
   +--------------------------------------------------------------+
   |  include/zpdf/   pdf_doc.h  pdf_text.h  pdf_render.h  pdf_obj.h |
   +--------------------------------------------------------------+
   |  lib/zpdf/                                                   |
   |    pdf_render   content stream interpreter, graphics state,  |
   |                 patterns, shadings, transparency groups      |
   |    pdf_raster   anti-aliased scanline rasterizer, clipping   |
   |    pdf_font     font dictionaries, encodings, CMaps, widths  |
   |    pdf_ttf      TrueType glyphs       pdf_cff   CFF / Type2  |
   |    pdf_image    image XObjects        pdf_jpeg  baseline DCT |
   |    pdf_color    colour spaces         pdf_func  PDF functions|
   |    pdf_text     text extraction and layout                   |
   |    pdf_doc      xref, object streams, page tree, caches      |
   |    pdf_lex / pdf_obj   tokenizer and object model            |
   |    pdf_filter / inflate   stream filters, DEFLATE            |
   |    pdf_mem      allocator (PSRAM on Espressif SoCs)          |
   |    fonts/       built-in Liberation and DejaVu subsets       |
   +--------------------------------------------------------------+
   |  Zephyr: kernel, LVGL, display and input drivers, SDHC,      |
   |          FAT file system                                     |
   +--------------------------------------------------------------+

The library has no Zephyr dependency: the same sources build with a host
compiler for the tests and the tools in ``tools/``. ``pdf_mem.c`` is the only
file that knows where it runs, choosing the PSRAM heap on Espressif SoCs.

Threads of the reader
*********************

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Thread
     - Role
   * - main
     - Runs LVGL, the touch handlers and the document list; polls the
       worker's results and delivers tiles and text to the screen
   * - render worker
     - Owns the open document: renders tiles and extracts text on request.
       A new request replaces a pending one, so quick zooms and page turns
       do not queue up work. ``CONFIG_ZPDF_APP_RENDER_PRIORITY`` keeps it
       below the UI
   * - input thread
     - Dispatches touch events; raised above the worker so panning stays
       smooth while a tile renders
   * - LVGL flush thread
     - On the ESP32-S3, sends the previous frame over SPI while the next one
       is drawn (``CONFIG_LV_Z_FLUSH_THREAD``)

Rendering in tiles
******************

The reader never renders a whole page at high zoom. It asks
``pdf_render_region()`` for a tile of twice the visible area around the
current scroll position, converts it to RGB565 for LVGL, and renders a new
tile only when panning leaves the current one. This bounds the memory and
time of a zoom level: at 300 % of a 480 px wide view the tile is about
1.7 MB, plus the renderer's clip and soft masks, which are tile-sized too.

Memory
******

On the ESP32-S3 the document, every decoded stream, the rendered tiles and
the renderer's masks live in the 8 MB PSRAM; the display's render buffers
and LVGL's widget pool stay in internal RAM. Three budgets keep large
documents inside it:

- ``CONFIG_ZPDF_IMAGE_MEM_BYTES`` caps the memory for decoding one image.
  JPEGs are downscaled in the DCT domain and Flate images by box filtering
  rows, so a scanned page decodes near its on-screen size.
- ``CONFIG_ZPDF_AUX_CACHE_BYTES`` keeps decoded font programs between pages
  and drops the oldest unused ones.
- ``CONFIG_ZPDF_GLYPH_CACHE_BYTES`` caches rasterized glyphs during one page
  render.

Storage
*******

``app/src/storage_fs.c`` lists and reads documents from the FAT or exFAT
file system on the SD card; ``app/src/storage_host.c`` replaces it on
``native_sim`` with files of the host named on the command line. Both read
the whole file into ``pdf_malloc()`` memory, because the parser reads
objects from the buffer on demand.
