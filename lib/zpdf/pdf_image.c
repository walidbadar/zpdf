/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_image.h"
#include "pdf_color.h"
#include "pdf_jpeg.h"
#include "pdf_mem.h"
#include "pdf_filter.h"
#include "inflate.h"
#include "pdf_util.h"
#include <string.h>

PDF_LOG_REGISTER();

#define MAX_IMAGE_PIXELS (16u << 20)
#ifndef PDF_IMAGE_MAX_RAW
#define PDF_IMAGE_MAX_RAW (24u << 20)
#endif
/* Flate images with more raw bytes than this are decoded row by row. */
#ifndef PDF_IMAGE_STREAM_THRESHOLD
#define PDF_IMAGE_STREAM_THRESHOLD (256u << 10)
#endif

static int jpeg_denominator(int w, int h, int hint_w, int hint_h);

/* Inline image dictionaries use abbreviations. */
static struct pdf_obj *iget(struct pdf_doc *doc, struct pdf_obj *s, const char *key, const char *abbr)
{
	struct pdf_obj *o = pdf_get(doc, s, key);

	if (o == NULL && abbr != NULL) {
		o = pdf_get(doc, s, abbr);
	}
	return o;
}

static bool is_jpeg_filter(struct pdf_doc *doc, struct pdf_obj *s, bool *jpx)
{
	struct pdf_obj *f = iget(doc, s, "Filter", "F");

	*jpx = false;
	if (f == NULL) {
		return false;
	}
	struct pdf_obj *last = f;

	if (f->type == PDF_ARRAY && f->arr.n > 0) {
		last = pdf_array_get(doc, f, f->arr.n - 1);
	}
	if (pdf_is_name(last, "DCTDecode") || pdf_is_name(last, "DCT")) {
		return true;
	}
	if (pdf_is_name(last, "JPXDecode")) {
		*jpx = true;
	}
	return false;
}

static uint32_t get_sample(const uint8_t *row, int idx, int bpc)
{
	switch (bpc) {
	case 8:
		return row[idx];
	case 1:
		return (row[idx >> 3] >> (7 - (idx & 7))) & 1;
	case 2:
		return (row[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
	case 4:
		return (row[idx >> 1] >> (4 - 4 * (idx & 1))) & 15;
	case 16:
		return ((uint32_t)row[idx * 2] << 8) | row[idx * 2 + 1];
	default:
		return 0;
	}
}

/* Decode the gray plane of a mask image (SMask or stencil /Mask) at its own size. */
static int load_flate_streaming(struct pdf_doc *doc, struct pdf_obj *s, struct pdf_image *img,
				struct pdf_colorspace *cs, int bpc, const float *decode, bool custom_decode,
				int hint_w, int hint_h);

static uint8_t *decode_gray_plane(struct pdf_doc *doc, struct pdf_obj *s, int *w, int *h, bool stencil,
				  int hint_w, int hint_h)
{
	*w = (int)pdf_int(iget(doc, s, "Width", "W"));
	*h = (int)pdf_int(iget(doc, s, "Height", "H"));
	int bpc = stencil ? 1 : (int)pdf_int(iget(doc, s, "BitsPerComponent", "BPC"));

	if (*w <= 0 || *h <= 0 || (size_t)*w * (size_t)*h > MAX_IMAGE_PIXELS) {
		return NULL;
	}
	if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) {
		return NULL;
	}
	float d0 = 0.0f, d1 = 1.0f;
	struct pdf_obj *dec = iget(doc, s, "Decode", "D");

	if (dec != NULL && pdf_array_len(dec) >= 2) {
		d0 = pdf_num(pdf_array_get(doc, dec, 0));
		d1 = pdf_num(pdf_array_get(doc, dec, 1));
	}
	/* large Flate planes: stream through the image loader at reduced size */
	if (!stencil && hint_w > 0 && hint_h > 0) {
		struct pdf_colorspace *gray = pdf_colorspace_device(PDF_CS_GRAY);
		struct pdf_image tmp = {.w = *w, .h = *h};
		float decode2[2] = {d0, d1};

		if (gray != NULL) {
			int rc2 = load_flate_streaming(doc, s, &tmp, gray, bpc, decode2, d0 != 0.0f || d1 != 1.0f,
						       hint_w, hint_h);

			pdf_colorspace_free(gray);
			if (rc2 == 0 && tmp.rgb != NULL) {
				size_t n = (size_t)tmp.w * (size_t)tmp.h;
				uint8_t *plane = pdf_malloc(n);

				if (plane != NULL) {
					for (size_t i = 0; i < n; i++) {
						plane[i] = tmp.rgb[i * 3];
					}
				}
				pdf_free(tmp.rgb);
				*w = tmp.w;
				*h = tmp.h;
				return plane;
			}
			pdf_free(tmp.rgb);
		}
	}
	uint8_t *out = pdf_malloc((size_t)*w * (size_t)*h);

	if (out == NULL) {
		return NULL;
	}
	bool jpx;
	uint8_t *data;
	size_t len;
	int rc = pdf_stream_decode(doc, s, &data, &len);

	if (rc < 0) {
		pdf_free(out);
		return NULL;
	}
	if (rc == 1 && is_jpeg_filter(doc, s, &jpx)) {
		uint8_t *pix;
		int jw, jh, nc;
		bool adobe;
		int denom = jpeg_denominator(*w, *h, hint_w, hint_h);

		if (pdf_jpeg_decode_scaled(data, len, denom, &pix, &jw, &jh, &nc, &adobe) == 0) {
			/* the plane takes the decoded size */
			pdf_free(out);
			out = pdf_malloc((size_t)jw * (size_t)jh);
			if (out == NULL) {
				pdf_free(pix);
				pdf_free(data);
				return NULL;
			}
			for (int y = 0; y < jh; y++) {
				for (int x = 0; x < jw; x++) {
					uint8_t v = pix[((size_t)y * (size_t)jw + (size_t)x) * (size_t)nc];

					out[(size_t)y * (size_t)jw + (size_t)x] = d0 > d1 ? 255 - v : v;
				}
			}
			*w = jw;
			*h = jh;
			pdf_free(pix);
		} else {
			memset(out, 255, (size_t)*w * (size_t)*h);
		}
		pdf_free(data);
		return out;
	}
	if (rc == 1) {
		memset(out, 255, (size_t)*w * (size_t)*h);
		pdf_free(data);
		return out;
	}
	size_t rowbytes = ((size_t)*w * (size_t)bpc + 7) / 8;
	uint32_t maxv = bpc == 16 ? 65535 : ((1u << bpc) - 1);

	for (int y = 0; y < *h; y++) {
		const uint8_t *row = data + (size_t)y * rowbytes;
		bool have = (size_t)(y + 1) * rowbytes <= len;

		for (int x = 0; x < *w; x++) {
			uint32_t v = have ? get_sample(row, x, bpc) : (stencil ? 1 : maxv);
			float f = (float)v / (float)maxv;

			f = d0 + f * (d1 - d0);
			if (stencil) {
				/* stencil: sample 0 paints (after Decode), i.e. alpha 255 where f == 0 */
				out[(size_t)y * (size_t)*w + (size_t)x] = f < 0.5f ? 255 : 0;
			} else {
				int iv = (int)(f * 255.0f + 0.5f);

				out[(size_t)y * (size_t)*w + (size_t)x] = (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
			}
		}
	}
	pdf_free(data);
	return out;
}

static void attach_alpha(struct pdf_image *img, const uint8_t *plane, int pw, int ph)
{
	img->alpha = pdf_malloc((size_t)img->w * (size_t)img->h);
	if (img->alpha == NULL) {
		return;
	}
	for (int y = 0; y < img->h; y++) {
		int sy = (int)((int64_t)y * ph / img->h);

		for (int x = 0; x < img->w; x++) {
			int sx = (int)((int64_t)x * pw / img->w);

			img->alpha[(size_t)y * (size_t)img->w + (size_t)x] = plane[(size_t)sy * (size_t)pw + (size_t)sx];
		}
	}
}

#ifndef CONFIG_ZPDF_IMAGE_MEM_BYTES
#define CONFIG_ZPDF_IMAGE_MEM_BYTES (64u << 20)
#endif

/*
 * DCT scale denominator for a JPEG: decode at the smallest size that still
 * covers the device size (down to 1/8), and never beyond the memory budget
 * (the decoder holds the component planes and the RGB output, about 6
 * bytes per decoded pixel).
 */
static int jpeg_denominator(int w, int h, int hint_w, int hint_h)
{
	int d = 1;

	if (hint_w > 0 && hint_h > 0) {
		while (d < 8 && (w / (d * 2)) >= hint_w && (h / (d * 2)) >= hint_h) {
			d *= 2;
		}
	}
	while (d < 8 && ((size_t)(w / d) * (size_t)(h / d) * 6) > CONFIG_ZPDF_IMAGE_MEM_BYTES) {
		d *= 2;
	}
	return d;
}

/* Box-filter an RGB (and alpha) image down by an integer factor, in place of the planes. */
static void downscale(struct pdf_image *img, int f)
{
	if (f <= 1) {
		return;
	}
	int nw = img->w / f, nh = img->h / f;

	if (nw < 1 || nh < 1) {
		return;
	}
	uint8_t *rgb = img->rgb ? pdf_malloc((size_t)nw * (size_t)nh * 3) : NULL;
	uint8_t *alpha = img->alpha ? pdf_malloc((size_t)nw * (size_t)nh) : NULL;

	if ((img->rgb && rgb == NULL) || (img->alpha && alpha == NULL)) {
		pdf_free(rgb);
		pdf_free(alpha);
		return;
	}
	for (int y = 0; y < nh; y++) {
		for (int x = 0; x < nw; x++) {
			unsigned acc[4] = {0, 0, 0, 0};

			for (int j = 0; j < f; j++) {
				const uint8_t *row = img->rgb ? img->rgb + ((size_t)(y * f + j) * img->w + (size_t)x * f) * 3 : NULL;
				const uint8_t *arow = img->alpha ? img->alpha + (size_t)(y * f + j) * img->w + (size_t)x * f : NULL;

				for (int i = 0; i < f; i++) {
					if (row) {
						acc[0] += row[i * 3];
						acc[1] += row[i * 3 + 1];
						acc[2] += row[i * 3 + 2];
					}
					if (arow) {
						acc[3] += arow[i];
					}
				}
			}
			unsigned n = (unsigned)(f * f);

			if (rgb) {
				rgb[((size_t)y * nw + x) * 3] = (uint8_t)(acc[0] / n);
				rgb[((size_t)y * nw + x) * 3 + 1] = (uint8_t)(acc[1] / n);
				rgb[((size_t)y * nw + x) * 3 + 2] = (uint8_t)(acc[2] / n);
			}
			if (alpha) {
				alpha[(size_t)y * nw + x] = (uint8_t)(acc[3] / n);
			}
		}
	}
	pdf_free(img->rgb);
	pdf_free(img->alpha);
	img->rgb = rgb;
	img->alpha = alpha;
	img->w = nw;
	img->h = nh;
}

/* Convert one row of packed samples to RGB888. */
struct row_conv {
	const struct pdf_colorspace *cs;
	int nc, bpc;
	uint32_t maxv;
	float decode[64];
	bool use_lut;
	uint8_t lut[256][3];
	bool direct_rgb8, direct_gray8;
};

static void row_conv_init(struct row_conv *rc, const struct pdf_colorspace *cs, int bpc, const float *decode,
			  bool custom_decode)
{
	rc->cs = cs;
	rc->nc = cs->ncomps;
	rc->bpc = bpc;
	rc->maxv = bpc == 16 ? 65535 : ((1u << bpc) - 1);
	memcpy(rc->decode, decode, sizeof(float) * (size_t)rc->nc * 2);
	rc->use_lut = (rc->nc == 1 && bpc <= 8);
	if (rc->use_lut) {
		for (uint32_t v = 0; v <= rc->maxv; v++) {
			float comps[1], rgb[3];

			comps[0] = decode[0] + (float)v / (float)rc->maxv * (decode[1] - decode[0]);
			pdf_colorspace_to_rgb(cs, comps, rgb);
			rc->lut[v][0] = (uint8_t)(rgb[0] * 255.0f + 0.5f);
			rc->lut[v][1] = (uint8_t)(rgb[1] * 255.0f + 0.5f);
			rc->lut[v][2] = (uint8_t)(rgb[2] * 255.0f + 0.5f);
		}
	}
	rc->direct_rgb8 = (cs->type == PDF_CS_RGB && bpc == 8 && !custom_decode);
	rc->direct_gray8 = (cs->type == PDF_CS_GRAY && bpc == 8 && !custom_decode);
}

static void row_convert(const struct row_conv *rc, const uint8_t *row, int w, uint8_t *o)
{
	if (rc->direct_rgb8) {
		memcpy(o, row, (size_t)w * 3);
		return;
	}
	if (rc->direct_gray8) {
		for (int x = 0; x < w; x++) {
			o[x * 3] = o[x * 3 + 1] = o[x * 3 + 2] = row[x];
		}
		return;
	}
	for (int x = 0; x < w; x++) {
		if (rc->use_lut) {
			uint32_t v = get_sample(row, x, rc->bpc);

			memcpy(o + x * 3, rc->lut[v], 3);
			continue;
		}
		float comps[32], rgb[3];

		for (int c = 0; c < rc->nc; c++) {
			uint32_t v = get_sample(row, x * rc->nc + c, rc->bpc);

			comps[c] = rc->decode[c * 2] + (float)v / (float)rc->maxv * (rc->decode[c * 2 + 1] - rc->decode[c * 2]);
		}
		pdf_colorspace_to_rgb(rc->cs, comps, rgb);
		o[x * 3] = (uint8_t)(rgb[0] * 255.0f + 0.5f);
		o[x * 3 + 1] = (uint8_t)(rgb[1] * 255.0f + 0.5f);
		o[x * 3 + 2] = (uint8_t)(rgb[2] * 255.0f + 0.5f);
	}
}

/*
 * Row sink for streaming Flate images: bytes arrive in arbitrary chunks,
 * are cut into raw rows, un-predicted, converted to RGB and accumulated
 * f x f into the (downscaled) output image.
 */
struct row_sink {
	struct pdf_image *img;
	struct row_conv conv;
	int srcw, srch, f;
	size_t rowbytes;  /* packed samples per row */
	int pred, pcolors, pbpc, pcolumns;
	size_t in_rowbytes; /* rowbytes + 1 for PNG predictors */
	uint8_t *raw, *prev, *rgbrow;
	size_t fill;
	uint32_t *acc;
	int acc_rows;
	int srcy, outy;
	int outw, outh;
	bool oom;
};

static void sink_emit_row(struct row_sink *rs)
{
	uint8_t *row = rs->raw;

	if (rs->pred >= 10) {
		/* PNG: filter byte then samples; un-filter against the previous row */
		uint8_t ft = row[0];
		uint8_t *cur = row + 1;
		size_t bpp = ((size_t)rs->pcolors * rs->pbpc + 7) / 8;

		if (bpp == 0) {
			bpp = 1;
		}
		for (size_t i = 0; i < rs->rowbytes; i++) {
			uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
			uint8_t b = rs->prev[i];
			uint8_t c = (i >= bpp) ? rs->prev[i - bpp] : 0;
			uint8_t x = cur[i];

			switch (ft) {
			case 1:
				x += a;
				break;
			case 2:
				x += b;
				break;
			case 3:
				x += (uint8_t)(((unsigned)a + b) / 2);
				break;
			case 4: {
				int p = (int)a + b - c;
				int pa = p > a ? p - a : a - p;
				int pb = p > b ? p - b : b - p;
				int pc = p > c ? p - c : c - p;

				x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
				break;
			}
			default:
				break;
			}
			cur[i] = x;
		}
		memcpy(rs->prev, cur, rs->rowbytes);
		row = cur;
	} else if (rs->pred == 2 && rs->pbpc == 8) {
		size_t bpp = (size_t)rs->pcolors;

		for (size_t i = bpp; i < rs->rowbytes; i++) {
			row[i] += row[i - bpp];
		}
	}
	if (rs->srcy >= rs->srch) {
		return;
	}
	row_convert(&rs->conv, row, rs->srcw, rs->rgbrow);
	/* accumulate f columns into outw bins */
	for (int x = 0; x < rs->outw; x++) {
		for (int i = 0; i < rs->f; i++) {
			const uint8_t *p = rs->rgbrow + (size_t)(x * rs->f + i) * 3;

			rs->acc[x * 3] += p[0];
			rs->acc[x * 3 + 1] += p[1];
			rs->acc[x * 3 + 2] += p[2];
		}
	}
	rs->acc_rows++;
	rs->srcy++;
	if (rs->acc_rows == rs->f && rs->outy < rs->outh) {
		uint8_t *o = rs->img->rgb + (size_t)rs->outy * (size_t)rs->outw * 3;
		unsigned n = (unsigned)(rs->f * rs->f);

		for (int x = 0; x < rs->outw * 3; x++) {
			o[x] = (uint8_t)(rs->acc[x] / n);
		}
		memset(rs->acc, 0, sizeof(uint32_t) * (size_t)rs->outw * 3);
		rs->acc_rows = 0;
		rs->outy++;
	}
}

static int sink_bytes(void *ud, const uint8_t *data, size_t len)
{
	struct row_sink *rs = ud;

	while (len > 0) {
		if (rs->srcy >= rs->srch) {
			return 0; /* extra data after the last row */
		}
		size_t want = rs->in_rowbytes - rs->fill;
		size_t take = len < want ? len : want;

		memcpy(rs->raw + rs->fill, data, take);
		rs->fill += take;
		data += take;
		len -= take;
		if (rs->fill == rs->in_rowbytes) {
			sink_emit_row(rs);
			rs->fill = 0;
		}
	}
	return 0;
}

/* Returns 0 on success, 1 when this image does not qualify for streaming, negative on error. */
static int load_flate_streaming(struct pdf_doc *doc, struct pdf_obj *s, struct pdf_image *img,
				struct pdf_colorspace *cs, int bpc, const float *decode, bool custom_decode,
				int hint_w, int hint_h)
{
	struct pdf_obj *filter = iget(doc, s, "Filter", "F");
	struct pdf_obj *parms = iget(doc, s, "DecodeParms", "DP");

	if (filter != NULL && filter->type == PDF_ARRAY) {
		if (filter->arr.n != 1) {
			return 1;
		}
		filter = pdf_array_get(doc, filter, 0);
		if (parms != NULL && parms->type == PDF_ARRAY) {
			parms = pdf_array_get(doc, parms, 0);
		}
	}
	if (!pdf_is_name(filter, "FlateDecode") && !pdf_is_name(filter, "Fl")) {
		return 1;
	}
	size_t rowbytes = ((size_t)img->w * (size_t)cs->ncomps * (size_t)bpc + 7) / 8;

	if (rowbytes * (size_t)img->h < PDF_IMAGE_STREAM_THRESHOLD) {
		return 1;
	}
	struct row_sink *rs = pdf_calloc(sizeof(*rs));

	if (rs == NULL) {
		return -1;
	}
	rs->img = img;
	rs->srcw = img->w;
	rs->srch = img->h;
	rs->rowbytes = rowbytes;
	rs->f = 1;
	if (hint_w > 0 && hint_h > 0 && img->w >= hint_w * 2 && img->h >= hint_h * 2) {
		int f = img->w / (hint_w * 2);
		int fy = img->h / (hint_h * 2);

		if (fy < f) {
			f = fy;
		}
		rs->f = f > 1 ? f : 1;
	}
	while (((size_t)(img->w / rs->f) * (size_t)(img->h / rs->f) * 3) > CONFIG_ZPDF_IMAGE_MEM_BYTES &&
	       rs->f < 64) {
		rs->f++;
	}
	rs->outw = img->w / rs->f;
	rs->outh = img->h / rs->f;
	rs->pred = parms ? (int)pdf_int(pdf_get(doc, parms, "Predictor")) : 1;
	rs->pcolors = parms ? (int)pdf_int(pdf_get(doc, parms, "Colors")) : 1;
	rs->pbpc = parms ? (int)pdf_int(pdf_get(doc, parms, "BitsPerComponent")) : 8;
	rs->pcolumns = parms ? (int)pdf_int(pdf_get(doc, parms, "Columns")) : 1;
	if (rs->pcolors <= 0) {
		rs->pcolors = 1;
	}
	if (rs->pbpc <= 0) {
		rs->pbpc = 8;
	}
	rs->in_rowbytes = rowbytes + (rs->pred >= 10 ? 1 : 0);
	rs->raw = pdf_malloc(rs->in_rowbytes);
	rs->prev = pdf_calloc(rowbytes);
	rs->rgbrow = pdf_malloc((size_t)img->w * 3);
	rs->acc = pdf_calloc(sizeof(uint32_t) * (size_t)rs->outw * 3);
	uint8_t *out = pdf_malloc((size_t)rs->outw * (size_t)rs->outh * 3);

	if (rs->raw == NULL || rs->prev == NULL || rs->rgbrow == NULL || rs->acc == NULL || out == NULL) {
		pdf_free(rs->raw);
		pdf_free(rs->prev);
		pdf_free(rs->rgbrow);
		pdf_free(rs->acc);
		pdf_free(out);
		pdf_free(rs);
		return -1;
	}
	memset(out, 255, (size_t)rs->outw * (size_t)rs->outh * 3);
	pdf_free(img->rgb);
	img->rgb = out;
	row_conv_init(&rs->conv, cs, bpc, decode, custom_decode);
	int rc = inflate_stream(s->stream.data, s->stream.len, sink_bytes, rs);

	/* a partial final accumulation (truncated data) is left white */
	img->w = rs->outw;
	img->h = rs->outh;
	pdf_free(rs->raw);
	pdf_free(rs->prev);
	pdf_free(rs->rgbrow);
	pdf_free(rs->acc);
	pdf_free(rs);
	return rc < 0 ? -1 : 0;
}

struct pdf_image *pdf_image_load(struct pdf_doc *doc, struct pdf_obj *s, struct pdf_obj *resources)
{
	return pdf_image_load_hint(doc, s, resources, 0, 0);
}

struct pdf_image *pdf_image_load_hint(struct pdf_doc *doc, struct pdf_obj *s, struct pdf_obj *resources,
				      int hint_w, int hint_h)
{
	if (s == NULL || s->type != PDF_STREAM) {
		return NULL;
	}
	struct pdf_image *img = pdf_calloc(sizeof(*img));

	if (img == NULL) {
		return NULL;
	}
	img->w = (int)pdf_int(iget(doc, s, "Width", "W"));
	img->h = (int)pdf_int(iget(doc, s, "Height", "H"));
	if (img->w <= 0 || img->h <= 0 || (size_t)img->w * (size_t)img->h > MAX_IMAGE_PIXELS) {
		pdf_free(img);
		return NULL;
	}
	struct pdf_obj *im = iget(doc, s, "ImageMask", "IM");

	if (im != NULL && im->type == PDF_BOOL && im->b) {
		int w, h;
		uint8_t *plane = decode_gray_plane(doc, s, &w, &h, true, 0, 0);

		img->is_mask = true;
		if (plane == NULL) {
			pdf_free(img);
			return NULL;
		}
		img->alpha = plane;
		img->w = w;
		img->h = h;
		if (hint_w > 0 && hint_h > 0 && img->w >= hint_w * 4 && img->h >= hint_h * 4) {
			int f = img->w / (hint_w * 2);
			int fy = img->h / (hint_h * 2);

			if (fy < f) {
				f = fy;
			}
			downscale(img, f);
		}
		return img;
	}
	int bpc = (int)pdf_int(iget(doc, s, "BitsPerComponent", "BPC"));
	bool jpx;
	bool jpeg = is_jpeg_filter(doc, s, &jpx);
	struct pdf_obj *cso = iget(doc, s, "ColorSpace", "CS");
	struct pdf_colorspace *cs = cso ? pdf_colorspace_load(doc, cso, resources) : NULL;
	size_t npix = (size_t)img->w * (size_t)img->h;

	/* the RGB buffer is allocated by the path that knows the final size */
	struct pdf_obj *dec = iget(doc, s, "Decode", "D");
	float decode[64];
	bool custom_decode = false;
	uint8_t *data = NULL;
	size_t len = 0;
	int rc;

	if (!jpeg && cs != NULL && cs->ncomps <= 32 &&
	    (bpc == 1 || bpc == 2 || bpc == 4 || bpc == 8 || bpc == 16)) {
		pdf_colorspace_decode_default(cs, bpc, decode);
		if (dec != NULL && (int)pdf_array_len(dec) >= cs->ncomps * 2) {
			for (int i = 0; i < cs->ncomps * 2; i++) {
				decode[i] = pdf_num(pdf_array_get(doc, dec, (size_t)i));
			}
			custom_decode = true;
		}
		int src = load_flate_streaming(doc, s, img, cs, bpc, decode, custom_decode, hint_w, hint_h);

		if (src == 0) {
			goto masks;
		}
		if (src < 0) {
			goto placeholder;
		}
		custom_decode = false;
	}
	rc = pdf_stream_decode(doc, s, &data, &len);
	if (rc < 0) {
		goto placeholder;
	}
	if (rc == 0 && len > PDF_IMAGE_MAX_RAW) {
		goto placeholder; /* a raw image this large does not fit small targets */
	}

	if (jpeg && rc == 1) {
		uint8_t *pix;
		int jw, jh, nc;
		bool adobe;
		int denom = jpeg_denominator(img->w, img->h, hint_w, hint_h);

		if (pdf_jpeg_decode_scaled(data, len, denom, &pix, &jw, &jh, &nc, &adobe) != 0) {
			goto placeholder;
		}
		/* the decoded size is the image size from here on */
		img->w = jw;
		img->h = jh;
		npix = (size_t)jw * (size_t)jh;
		img->rgb = pdf_malloc(npix * 3);
		if (img->rgb == NULL) {
			pdf_free(pix);
			goto placeholder;
		}
		bool invert = (dec != NULL && pdf_array_len(dec) >= 2 &&
			       pdf_num(pdf_array_get(doc, dec, 0)) > pdf_num(pdf_array_get(doc, dec, 1)));
		/* Adobe CMYK JPEGs are stored inverted */
		if (nc == 4 && adobe) {
			invert = !invert;
		}
		bool indexed = cs != NULL && cs->type == PDF_CS_INDEXED;

		for (int y = 0; y < img->h; y++) {
			int sy = y < jh ? y : jh - 1;

			for (int x = 0; x < img->w; x++) {
				int sx = x < jw ? x : jw - 1;
				const uint8_t *p = pix + ((size_t)sy * (size_t)jw + (size_t)sx) * (size_t)nc;
				uint8_t *o = img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;

				if (nc == 1) {
					uint8_t v = invert ? 255 - p[0] : p[0];

					if (indexed) {
						float comps[1] = {(float)v}, rgb[3];

						pdf_colorspace_to_rgb(cs, comps, rgb);
						o[0] = (uint8_t)(rgb[0] * 255.0f + 0.5f);
						o[1] = (uint8_t)(rgb[1] * 255.0f + 0.5f);
						o[2] = (uint8_t)(rgb[2] * 255.0f + 0.5f);
					} else if (cs != NULL && cs->type == PDF_CS_SEPARATION) {
						float comps[1] = {v / 255.0f}, rgb[3];

						pdf_colorspace_to_rgb(cs, comps, rgb);
						o[0] = (uint8_t)(rgb[0] * 255.0f + 0.5f);
						o[1] = (uint8_t)(rgb[1] * 255.0f + 0.5f);
						o[2] = (uint8_t)(rgb[2] * 255.0f + 0.5f);
					} else {
						o[0] = o[1] = o[2] = v;
					}
				} else if (nc == 3) {
					o[0] = invert ? 255 - p[0] : p[0];
					o[1] = invert ? 255 - p[1] : p[1];
					o[2] = invert ? 255 - p[2] : p[2];
				} else {
					float c = p[0] / 255.0f, m = p[1] / 255.0f, yy = p[2] / 255.0f, k = p[3] / 255.0f;

					if (invert) {
						c = 1 - c;
						m = 1 - m;
						yy = 1 - yy;
						k = 1 - k;
					}
					o[0] = (uint8_t)((1 - c) * (1 - k) * 255.0f + 0.5f);
					o[1] = (uint8_t)((1 - m) * (1 - k) * 255.0f + 0.5f);
					o[2] = (uint8_t)((1 - yy) * (1 - k) * 255.0f + 0.5f);
				}
			}
		}
		pdf_free(pix);
		pdf_free(data);
		data = NULL;
	} else if (rc == 1) {
		/* JPX, CCITT, JBIG2: not available */
		goto placeholder;
	} else {
		if (cs == NULL) {
			/* guess from the data size */
			cs = pdf_colorspace_device(PDF_CS_GRAY);
			if (cs == NULL) {
				goto placeholder;
			}
		}
		if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) {
			bpc = 8;
		}
		int nc = cs->ncomps;

		if (nc > 32) {
			goto placeholder;
		}
		img->rgb = pdf_malloc(npix * 3);
		if (img->rgb == NULL) {
			goto placeholder;
		}
		pdf_colorspace_decode_default(cs, bpc, decode);
		if (dec != NULL && (int)pdf_array_len(dec) >= nc * 2) {
			for (int i = 0; i < nc * 2; i++) {
				decode[i] = pdf_num(pdf_array_get(doc, dec, (size_t)i));
			}
			custom_decode = true;
		}
		size_t rowbytes = ((size_t)img->w * (size_t)nc * (size_t)bpc + 7) / 8;
		struct row_conv *conv = pdf_malloc(sizeof(*conv));

		if (conv == NULL) {
			goto placeholder;
		}
		row_conv_init(conv, cs, bpc, decode, custom_decode);
		for (int y = 0; y < img->h; y++) {
			const uint8_t *row = data + (size_t)y * rowbytes;
			bool have = (size_t)(y + 1) * rowbytes <= len;
			uint8_t *o = img->rgb + (size_t)y * (size_t)img->w * 3;

			if (!have) {
				/* truncated data: leave remaining rows white */
				memset(o, 255, (size_t)img->w * 3);
				continue;
			}
			row_convert(conv, row, img->w, o);
		}
		pdf_free(conv);
		pdf_free(data);
		data = NULL;
	}

masks:
	/* soft mask / stencil mask / colour key mask */
	npix = (size_t)img->w * (size_t)img->h;
	(void)npix;
	struct pdf_obj *sm = pdf_get(doc, s, "SMask");
	struct pdf_obj *mk = pdf_get(doc, s, "Mask");

	if (sm != NULL && sm->type == PDF_STREAM) {
		int mw, mh;
		uint8_t *plane = decode_gray_plane(doc, sm, &mw, &mh, false, img->w, img->h);

		if (plane != NULL) {
			attach_alpha(img, plane, mw, mh);
			pdf_free(plane);
		}
	} else if (mk != NULL && mk->type == PDF_STREAM) {
		int mw, mh;
		uint8_t *plane = decode_gray_plane(doc, mk, &mw, &mh, true, 0, 0);

		if (plane != NULL) {
			/* stencil mask: 255 marks masked-out (unpainted) areas after our decode */
			for (size_t i = 0; i < (size_t)mw * (size_t)mh; i++) {
				plane[i] = 255 - plane[i];
			}
			attach_alpha(img, plane, mw, mh);
			pdf_free(plane);
		}
	} else if (mk != NULL && mk->type == PDF_ARRAY && !jpeg && cs != NULL && cs->ncomps <= 4) {
		/* colour key masking: compare raw samples; approximate through the decoded RGB */
		(void)0;
	}
	pdf_colorspace_free(cs);
	if (hint_w > 0 && hint_h > 0 && img->w >= hint_w * 4 && img->h >= hint_h * 4) {
		int f = img->w / (hint_w * 2);
		int fy = img->h / (hint_h * 2);

		if (fy < f) {
			f = fy;
		}
		if (f > 1) {
			downscale(img, f);
		}
	}
	return img;

placeholder:
	pdf_free(data);
	pdf_colorspace_free(cs);
	pdf_free(img->rgb);
	/* a small grey box stands in for the image */
	img->w = 8;
	img->h = 8;
	img->rgb = pdf_malloc(8 * 8 * 3);
	if (img->rgb == NULL) {
		pdf_free(img->alpha);
		pdf_free(img);
		return NULL;
	}
	memset(img->rgb, 0x80, 8 * 8 * 3);
	img->unsupported = true;
	return img;
}

void pdf_image_free(struct pdf_image *img)
{
	if (img == NULL) {
		return;
	}
	pdf_free(img->rgb);
	pdf_free(img->alpha);
	pdf_free(img);
}
