/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Document access: cross reference, indirect objects, streams, page tree.
 */
#ifndef ZPDF_PDF_DOC_H
#define ZPDF_PDF_DOC_H

#include <zpdf/pdf_obj.h>

struct pdf_doc;

/*
 * Open a document held entirely in memory. The buffer must stay valid (and
 * unchanged) until pdf_close(); the parser keeps pointers into it.
 * On failure NULL is returned and err (if given) receives a short reason.
 */
struct pdf_doc *pdf_open(const uint8_t *buf, size_t len, char *err, size_t err_len);
void pdf_close(struct pdf_doc *doc);

/* True when the file carries a /Encrypt dictionary; its text is unreadable here. */
bool pdf_is_encrypted(const struct pdf_doc *doc);

/* Resolve a reference; other objects are returned as they are. Owned by the document. */
struct pdf_obj *pdf_resolve(struct pdf_doc *doc, struct pdf_obj *o);
/* Dictionary (or stream dictionary) lookup with reference resolution. */
struct pdf_obj *pdf_get(struct pdf_doc *doc, struct pdf_obj *dict, const char *key);
struct pdf_obj *pdf_array_get(struct pdf_doc *doc, struct pdf_obj *arr, size_t i);

size_t pdf_page_count(struct pdf_doc *doc);
struct pdf_obj *pdf_page(struct pdf_doc *doc, size_t index);
/* Page attribute lookup honouring inheritance through /Parent. */
struct pdf_obj *pdf_page_attr(struct pdf_doc *doc, struct pdf_obj *page, const char *key);

/*
 * Decode a stream through its filter chain. *out is pdf_malloc'd.
 * Returns 0 on success, 1 if an image-only filter (DCT, JPX, CCITT, JBIG2)
 * was left unapplied (out holds the still-encoded data), negative on error.
 */
int pdf_stream_decode(struct pdf_doc *doc, struct pdf_obj *stream, uint8_t **out, size_t *out_len);

/* Concatenated, decoded content stream(s) of a page. */
int pdf_page_content(struct pdf_doc *doc, struct pdf_obj *page, uint8_t **out, size_t *out_len);

/* Document info strings (Title etc.) decoded to UTF-8; NULL when absent. Caller frees. */
char *pdf_info_string(struct pdf_doc *doc, const char *key);

/* Font cache hooks used by the text extractor (opaque to applications). */
void *pdf_doc_font_cache_get(struct pdf_doc *doc, const struct pdf_obj *fontdict);
void pdf_doc_font_cache_put(struct pdf_doc *doc, const struct pdf_obj *fontdict, void *font);

/*
 * Reference counted cache of derived data keyed by an object pointer
 * (decoded font programs for the renderer). get() takes a reference that
 * unref() releases; entries without references are evicted, oldest first,
 * when the cache exceeds CONFIG_ZPDF_AUX_CACHE_BYTES.
 */
void *pdf_doc_aux_get(struct pdf_doc *doc, const void *key);
int pdf_doc_aux_put(struct pdf_doc *doc, const void *key, void *data, size_t size, void (*free_fn)(void *));
void pdf_doc_aux_unref(struct pdf_doc *doc, const void *key);

#endif
