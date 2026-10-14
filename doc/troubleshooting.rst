..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: GPL-3.0-only

Troubleshooting
###############

The list says "No documents"
****************************

- Documents must be in the root directory of the card, with a ``.pdf``
  extension.
- The card must be FAT or exFAT formatted. A card with an unknown file
  system is reported, not formatted (``CONFIG_FS_FATFS_MOUNT_MKFS=n``).
- At most ``CONFIG_ZPDF_APP_MAX_FILES`` documents are listed.

The card is not detected, or reads fail with ``-EIO``
*****************************************************

- On the VIEWE board the 4-bit bus only works in default speed mode; the
  overlay sets 25 MHz. A faster clock in a custom overlay produces exactly
  this error (see :doc:`hardware`).
- Try a stronger pad drive (``CONFIG_ZPDF_APP_SD_DRIVE_STRENGTH``) or, with
  the patch from ``patches/`` applied to Zephyr, another input delay phase
  (``CONFIG_ZPDF_APP_SD_DIN_PHASE``).
- Some cards need a power cycle after a failed initialisation.

A document is refused as too large
**********************************

Documents are read into RAM as a whole; ``CONFIG_ZPDF_APP_MAX_FILE_SIZE``
(6 MB by default) leaves room in the 8 MB PSRAM for the decoded streams and
tiles. Raise it only together with a smaller ``CONFIG_ZPDF_IMAGE_MEM_BYTES``
or a lower ``CONFIG_ZPDF_APP_MAX_ZOOM_PCT``.

A page takes seconds to appear
******************************

That is the rendering speed of the ESP32-S3 for a full page of vector
graphics or a large image. Switch to the text view (``Aa``) for reading:
extraction is far faster than rendering. Pages with photographs decode
faster with a smaller ``CONFIG_ZPDF_IMAGE_MEM_BYTES``, at the cost of
sharpness when zoomed in.

Nothing is shown for a document
*******************************

- Encrypted documents are reported and not shown; the library does not
  implement the standard security handler.
- A damaged file is rebuilt by scanning for objects. When that fails the
  error from ``pdf_open()`` is logged; ``tools/pdftext`` on a host shows the
  same message and lets you dump the objects.

Text shows U+FFFD or is garbled
*******************************

The font has no ToUnicode map and no usable encoding or font program, so the
character codes cannot be mapped to text. The page view still renders the
glyphs. Predefined CJK CMaps are not included.

An image is a grey box
**********************

JPX (JPEG 2000), CCITT and JBIG2 images and progressive JPEG are not
decoded; a placeholder is drawn where they would be.

``native_sim`` does not open a window
*************************************

The SDL2 development package must be installed before Zephyr is configured.
Install it and build again with ``west build -p``. Screenshots (``--shot``)
do not need a visible window; the image is taken from LVGL's buffer.

Comparing with another renderer
*******************************

.. code-block:: sh

   make -C zpdf/tools/pdfrender
   zpdf/tools/pdfrender/pdfrender file.pdf 3 ours.ppm 480
   pdftoppm -r 56 -f 3 -l 3 -ppm file.pdf theirs
   compare -metric MAE ours.ppm theirs-03.ppm diff.png

Differences in glyph weight are expected (no hinting); differences in
content are bugs.
