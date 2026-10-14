# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZPDF_MODULE_DIR}/include)

zephyr_library_sources(
  pdf_mem.c
  inflate.c
  pdf_obj.c
  pdf_lex.c
  pdf_filter.c
  pdf_doc.c
  pdf_encodings.c
  pdf_font.c
  pdf_stdfonts.c
  pdf_ttf.c
  pdf_cff.c
  pdf_raster.c
  pdf_func.c
  pdf_color.c
  pdf_jpeg.c
  pdf_image.c
  pdf_render.c
  fonts/font_sans.c
  fonts/font_sans_bold.c
  fonts/font_sans_italic.c
  fonts/font_sans_bolditalic.c
  fonts/font_serif.c
  fonts/font_serif_bold.c
  fonts/font_serif_italic.c
  fonts/font_serif_bolditalic.c
  fonts/font_mono.c
  fonts/font_mono_bold.c
  fonts/font_symbol.c
  pdf_text.c
)
