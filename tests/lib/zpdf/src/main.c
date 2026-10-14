/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Parser tests on small generated documents (see gen_test_pdfs.py).
 */
#include <zephyr/ztest.h>
#include <zpdf/pdf_doc.h>
#include <zpdf/pdf_text.h>
#include <zpdf/pdf_render.h>
#include "inflate.h"
#include "pdf_mem.h"
#include <string.h>
#include "test_pdfs.h"

static void check_hello(const uint8_t *data, size_t len, const char *what)
{
	char err[48] = "";
	struct pdf_doc *doc = pdf_open(data, len, err, sizeof(err));

	zassert_not_null(doc, "%s: open failed: %s", what, err);
	zassert_equal(pdf_page_count(doc), 1, "%s: page count", what);
	zassert_false(pdf_is_encrypted(doc), "%s: not encrypted", what);

	char *text = NULL;

	zassert_equal(pdf_page_text(doc, 0, &text), 0, "%s: extract", what);
	zassert_not_null(text, "%s: text", what);
	/* "Hello Zephyr", then "PDF reader" from a TJ array with a word gap, then a new line */
	zassert_str_equal(text, "Hello Zephyr\nPDF reader\nsecond line", "%s: got '%s'", what, text);
	pdf_text_free(text);
	pdf_close(doc);
}

ZTEST(zpdf, test_plain_xref_table)
{
	check_hello(pdf_plain, sizeof(pdf_plain), "plain");
}

ZTEST(zpdf, test_xref_stream_and_object_streams)
{
	check_hello(pdf_xref_stream, sizeof(pdf_xref_stream), "xref stream");
}

ZTEST(zpdf, test_damaged_startxref_is_reconstructed)
{
	check_hello(pdf_broken_startxref, sizeof(pdf_broken_startxref), "broken startxref");
}

ZTEST(zpdf, test_truncated_file_is_reconstructed)
{
	check_hello(pdf_truncated, sizeof(pdf_truncated), "truncated");
}

ZTEST(zpdf, test_type0_font_with_tounicode)
{
	char err[48] = "";
	struct pdf_doc *doc = pdf_open(pdf_type0, sizeof(pdf_type0), err, sizeof(err));

	zassert_not_null(doc, "open failed: %s", err);
	char *text = NULL;

	zassert_equal(pdf_page_text(doc, 0, &text), 0);
	/* codes 1..5: Z, e f g (bfrange), fi ligature (U+FB01) */
	zassert_str_equal(text, "Zefg\xEF\xAC\x81", "got '%s'", text);
	pdf_text_free(text);
	pdf_close(doc);
}

ZTEST(zpdf, test_page_attributes)
{
	struct pdf_doc *doc = pdf_open(pdf_plain, sizeof(pdf_plain), NULL, 0);

	zassert_not_null(doc);
	struct pdf_obj *page = pdf_page(doc, 0);
	struct pdf_obj *mb = pdf_page_attr(doc, page, "MediaBox");

	zassert_not_null(mb);
	zassert_equal(mb->type, PDF_ARRAY);
	zassert_equal(pdf_array_len(mb), 4);
	zassert_equal((int)pdf_num(pdf_array_get(doc, mb, 2)), 300);
	zassert_is_null(pdf_page(doc, 1));
	pdf_close(doc);
}

ZTEST(zpdf, test_rejects_garbage)
{
	static const uint8_t junk[64] = "this is not a pdf file at all, just some text to fill space";
	char err[48] = "";

	zassert_is_null(pdf_open(junk, sizeof(junk), err, sizeof(err)));
	zassert_true(strlen(err) > 0);
	zassert_is_null(pdf_open(pdf_plain, 8, err, sizeof(err)));
}

ZTEST(zpdf, test_inflate_stored_and_fixed)
{
	/* zlib stream of "Hello Hello Hello" with a fixed Huffman block */
	static const uint8_t z[] = {0x78, 0x9c, 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xf0, 0x40,
				    0x90, 0x00, 0x36, 0x0e, 0x06, 0x1d};
	uint8_t *out = NULL;
	size_t out_len = 0;

	zassert_equal(inflate_buf(z, sizeof(z), &out, &out_len), 0);
	zassert_equal(out_len, 17);
	zassert_mem_equal(out, "Hello Hello Hello", 17);
	pdf_free(out);

	/* truncated input: partial output is still returned */
	zassert_equal(inflate_buf(z, 8, &out, &out_len), 1);
	zassert_true(out_len > 0 && out_len < 17);
	zassert_mem_equal(out, "Hello", 5);
	pdf_free(out);

	/* garbage */
	static const uint8_t bad[] = {0x78, 0x9c, 0xff, 0xff, 0xff, 0xff};

	zassert_true(inflate_buf(bad, sizeof(bad), &out, &out_len) < 0);
	zassert_is_null(out);
}

ZTEST(zpdf, test_render_plain_page)
{
	if (!IS_ENABLED(CONFIG_ZPDF_TEST_RENDER)) {
		ztest_test_skip();
	}
	struct pdf_doc *doc = pdf_open(pdf_plain, sizeof(pdf_plain), NULL, 0);
	float w, h;

	zassert_not_null(doc);
	zassert_equal(pdf_page_size(doc, 0, &w, &h), 0);
	zassert_equal((int)w, 300);
	zassert_equal((int)h, 200);

	struct pdf_bitmap bm;

	zassert_equal(pdf_render_page(doc, 0, 1.0f, &bm), 0);
	zassert_equal(bm.w, 300);
	zassert_equal(bm.h, 200);
	zassert_not_null(bm.rgb);

	/* "Hello Zephyr" at 20,150 (14 pt Helvetica): dark pixels in that band, none at the bottom */
	long dark_text = 0, dark_bottom = 0;

	for (int y = 200 - 150 - 12; y < 200 - 150 + 2; y++) {
		for (int x = 20; x < 120; x++) {
			if (bm.rgb[(y * bm.w + x) * 3] < 128) {
				dark_text++;
			}
		}
	}
	for (int y = 180; y < 200; y++) {
		for (int x = 0; x < 300; x++) {
			if (bm.rgb[(y * bm.w + x) * 3] < 128) {
				dark_bottom++;
			}
		}
	}
	zassert_true(dark_text > 50, "text pixels: %ld", dark_text);
	zassert_equal(dark_bottom, 0, "stray pixels: %ld", dark_bottom);
	pdf_bitmap_free(&bm);
	pdf_close(doc);
}

ZTEST(zpdf, test_render_type0_glyphs)
{
	if (!IS_ENABLED(CONFIG_ZPDF_TEST_RENDER)) {
		ztest_test_skip();
	}
	struct pdf_doc *doc = pdf_open(pdf_type0, sizeof(pdf_type0), NULL, 0);
	struct pdf_bitmap bm;

	zassert_not_null(doc);
	/* the font has no program: glyphs come from the built-in fallback font */
	zassert_equal(pdf_render_page(doc, 0, 2.0f, &bm), 0);
	long dark = 0;

	for (size_t i = 0; i < (size_t)bm.w * bm.h; i++) {
		if (bm.rgb[i * 3] < 128) {
			dark++;
		}
	}
	zassert_true(dark > 20, "glyph pixels: %ld", dark);
	pdf_bitmap_free(&bm);
	pdf_close(doc);
}

ZTEST_SUITE(zpdf, NULL, NULL, NULL, NULL, NULL);
