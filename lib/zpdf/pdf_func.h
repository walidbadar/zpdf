/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * PDF functions (section 7.10): sampled (0), exponential (2), stitching
 * (3) and PostScript calculator (4), plus arrays of functions.
 */
#ifndef ZPDF_PDF_FUNC_H
#define ZPDF_PDF_FUNC_H

#include <zpdf/pdf_doc.h>

#define PDF_FUNC_MAX_OUT 32

struct pdf_function;

/* @obj: function dictionary/stream, or an array of them (one per output). */
struct pdf_function *pdf_function_load(struct pdf_doc *doc, struct pdf_obj *obj);
void pdf_function_free(struct pdf_function *f);
/* Evaluate; *nout receives the number of outputs written (<= PDF_FUNC_MAX_OUT). */
int pdf_function_eval(const struct pdf_function *f, const float *in, int nin, float *out, int *nout);
int pdf_function_num_outputs(const struct pdf_function *f);

#endif
