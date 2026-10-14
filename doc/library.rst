..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: GPL-3.0-only

The library
###########

``lib/zpdf`` is a Zephyr module with a plain C API that parses PDF files,
renders pages into RGB bitmaps and extracts the text of a page as UTF-8. It
is portable C with no dependency beyond libc and ``math.h``, uses ``float``
only, and also builds on a host for the tests and the tools.

Enable ``CONFIG_ZPDF=y``. In a project outside this repository add the
repository to the west manifest or pass
``-DZEPHYR_EXTRA_MODULES=/path/to/zpdf``.

Minimal example
***************

.. code-block:: c

   #include <zpdf/pdf_doc.h>
   #include <zpdf/pdf_text.h>
   #include <zpdf/pdf_render.h>

   char err[128];
   struct pdf_doc *doc = pdf_open(buf, len, err, sizeof(err)); /* buf stays owned by the caller */
   size_t n = pdf_page_count(doc);

   char *text;
   if (pdf_page_text(doc, 0, &text) == 0) {
       /* UTF-8 text of the first page */
       pdf_text_free(text);
   }

   float w, h;
   struct pdf_bitmap bm;
   pdf_page_size(doc, 0, &w, &h);                       /* points */
   if (pdf_render_page(doc, 0, 480.0f / w, &bm) == 0) { /* 480 px wide, RGB888 */
       pdf_bitmap_free(&bm);
   }
   pdf_close(doc);

The document buffer must stay valid and unchanged until ``pdf_close()``:
the parser reads objects from it on demand instead of copying the file.

API overview
************

The reference is ``include/zpdf/pdf_doc.h``, ``pdf_text.h``, ``pdf_render.h``
and ``pdf_obj.h``.

Document
========

.. list-table::
   :widths: 45 55

   * - ``pdf_open()`` / ``pdf_close()``
     - Parse the cross-reference data of a document in memory, or rebuild
       it by scanning when it is damaged; ``err`` receives the reason when
       it fails
   * - ``pdf_is_encrypted()``
     - True for encrypted documents, whose content cannot be shown
   * - ``pdf_page_count()`` / ``pdf_page()``
     - The pages of the page tree, up to ``CONFIG_ZPDF_MAX_PAGES``
   * - ``pdf_page_attr()``
     - A page attribute, following inheritance from the parent nodes
   * - ``pdf_page_content()``
     - The decoded, concatenated content streams of a page
   * - ``pdf_info_string()``
     - An entry of the document information dictionary as UTF-8

Text and rendering
==================

.. list-table::
   :widths: 45 55

   * - ``pdf_page_text()`` / ``pdf_text_free()``
     - The text of a page as UTF-8, in content order, with spaces and line
       breaks derived from the glyph positions
   * - ``pdf_page_size()``
     - Width and height of a page in points, rotation applied
   * - ``pdf_render_page()``
     - Render a whole page at a scale (device pixels per point) into an
       RGB888 bitmap
   * - ``pdf_render_region()``
     - Render a rectangle of the page at a scale; what the reader uses for
       its tiles
   * - ``pdf_bitmap_free()``
     - Release a rendered bitmap

Object model
============

.. list-table::
   :widths: 45 55

   * - ``pdf_resolve()``
     - Follow an indirect reference to the object, loading and caching it
   * - ``pdf_get()`` / ``pdf_array_get()``
     - A dictionary entry or array item, resolved
   * - ``pdf_stream_decode()``
     - Decode a stream through its filter chain
   * - ``pdf_dict_get_raw()``, ``pdf_array_get_raw()``, ``pdf_array_len()``,
       ``pdf_is_name()``, ``pdf_name_str()``
     - Access without resolving references
   * - ``pdf_obj_new*()``, ``pdf_dict_put()``, ``pdf_array_push()``,
       ``pdf_obj_free()``
     - Build objects, for tools and tests
   * - ``pdf_doc_aux_get()`` / ``pdf_doc_aux_put()`` / ``pdf_doc_aux_unref()``
     - Per-document cache of decoded data keyed by object (font programs),
       bounded by ``CONFIG_ZPDF_AUX_CACHE_BYTES``

What is handled
***************

Parsing
=======

- Cross-reference tables and cross-reference streams, incremental updates,
  hybrid files, object streams; damaged files are rebuilt by scanning for
  objects.
- Stream filters: FlateDecode (own DEFLATE decoder), LZWDecode,
  ASCII85Decode, ASCIIHexDecode, RunLengthDecode, PNG and TIFF predictors.
- Page tree with inherited attributes, content streams and form XObjects.
- Fonts: simple fonts (Type1, TrueType, Type3) with Standard, WinAnsi and
  MacRoman encodings and ``/Differences``, composite Type0 fonts with
  Identity or embedded CMaps, ``/ToUnicode`` CMaps, glyph widths (including
  the metrics of the standard 14 fonts) for word spacing.
- Text layout: text in content order with spaces and line breaks derived
  from glyph positions; duplicates from fake-bold rendering are dropped.

Rendering
=========

- Anti-aliased scanline rasterizer for fills (nonzero and even-odd) and
  strokes with joins, caps and dash patterns; clipping by rectangles and
  arbitrary paths; constant alpha and the separable blend modes.
- Text from embedded TrueType (``glyf``, including composites and the
  ``post`` table), CFF / Type2 charstrings (Type1C, CIDFontType0C, OpenType
  CFF, CID-keyed with FDSelect) and Type 3 glyph procedures. Non-embedded
  fonts (the standard 14 and Type1 programs) are drawn with built-in subsets
  of Liberation Sans, Serif and Mono and DejaVu Sans, chosen by name and
  descriptor flags.
- Images at 1, 2, 4, 8 and 16 bits per component in every colour space,
  ``Decode`` arrays, stencil masks, soft masks (``/SMask``) and explicit
  masks, inline images, and baseline JPEG (DCTDecode) with DCT-domain
  downscaling so photographs are decoded near their on-screen size.
- Axial, radial and function-based shadings (``sh`` and shading patterns),
  tiling patterns, luminosity soft masks from transparency groups, and
  annotation appearance streams (form fields, stamps).

Output quality was checked against ``pdftoppm`` on a 41-page report: the
mean per-pixel difference is 2 to 9 of 255, with the difference coming from
glyph weight (no hinting) rather than content.

Not handled
===========

Encrypted documents (reported, nothing shown), predefined CJK CMaps,
progressive JPEG, JPX, CCITT and JBIG2 images (a grey placeholder is drawn),
mesh shadings (types 4 to 7 paint a flat colour), knockout and isolated
group semantics, and text clipping modes. Text of fonts without a ToUnicode
map and without a usable encoding or font program shows as U+FFFD.

Memory
******

The library allocates from ``pdf_malloc()``, which is the external PSRAM
heap on Espressif SoCs with ``CONFIG_ESP_SPIRAM`` and the system heap
elsewhere. A rendered region needs 3 bytes per pixel plus one byte per pixel
for each active clip or soft mask.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Kconfig
     - Purpose
   * - ``ZPDF_MAX_PAGES``
     - Maximum number of pages collected from the page tree
   * - ``ZPDF_AUX_CACHE_BYTES``
     - Budget for decoded font programs kept between pages
   * - ``ZPDF_IMAGE_MEM_BYTES``
     - Budget for decoding one image; larger images are decoded at a
       reduced size (3 MB on the board)
   * - ``ZPDF_GLYPH_CACHE_BYTES``
     - Budget for rasterized glyphs during one page render
   * - ``ZPDF_LOG_LEVEL``
     - Log level of the library (0 none, 1 errors, 2 warnings, 3 info)

Tests
*****

.. code-block:: sh

   west twister -p native_sim/native/64 -T zpdf/tests

The suite in ``tests/lib/zpdf`` runs on ``native_sim`` and
``qemu_cortex_m3`` against small documents generated by
``tests/lib/zpdf/gen_test_pdfs.py`` (cross-reference tables and streams,
object streams, filters, encodings).

Host tools
**********

``tools/pdftext`` prints the text of a page and dumps the structures the
parser sees, for comparing with ``pdftotext``; ``tools/pdfrender`` renders a
page to a PPM file, for comparing with ``pdftoppm``.

.. code-block:: sh

   make -C zpdf/tools/pdftext
   zpdf/tools/pdftext/pdftext file.pdf               # all pages
   zpdf/tools/pdftext/pdftext file.pdf 3             # page 3
   zpdf/tools/pdftext/pdftext file.pdf 3 --dump      # raw content stream
   zpdf/tools/pdftext/pdftext file.pdf 3 --fonts     # font dictionaries
   zpdf/tools/pdftext/pdftext file.pdf 3 --xobjects  # images, forms, shadings, patterns
   zpdf/tools/pdftext/pdftext file.pdf 42 --obj      # dump object 42

   make -C zpdf/tools/pdfrender
   zpdf/tools/pdfrender/pdfrender file.pdf 3 page3.ppm 480   # render page 3, 480 px wide

``tools/mkfont/mkfont.py`` regenerates the built-in fallback fonts in
``lib/zpdf/fonts/`` from the Liberation and DejaVu TrueType files installed
on the host, keeping only the glyphs of the Latin, Greek and symbol ranges
the standard 14 fonts cover.
