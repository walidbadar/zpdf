/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Text extraction: runs a page's content stream and returns its text in
 * content order as UTF-8, with spaces and line breaks derived from glyph
 * positions.
 */
#ifndef ZPDF_PDF_TEXT_H
#define ZPDF_PDF_TEXT_H

#include <zpdf/pdf_doc.h>

/*
 * Extract the text of page @index. *out receives a NUL terminated UTF-8
 * string from pdf_malloc() (free with pdf_text_free). Returns 0 on success,
 * -ENOENT style negative values on failure. A page without text yields an
 * empty string, not an error.
 */
int pdf_page_text(struct pdf_doc *doc, size_t index, char **out);
void pdf_text_free(char *text);

#endif
