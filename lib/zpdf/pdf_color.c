/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_color.h"
#include "pdf_mem.h"
#include <math.h>
#include <string.h>

static struct pdf_colorspace *new_cs(enum pdf_cs_type type, int ncomps)
{
	struct pdf_colorspace *cs = pdf_calloc(sizeof(*cs));

	if (cs != NULL) {
		cs->type = type;
		cs->ncomps = ncomps;
	}
	return cs;
}

struct pdf_colorspace *pdf_colorspace_device(enum pdf_cs_type type)
{
	switch (type) {
	case PDF_CS_GRAY:
		return new_cs(PDF_CS_GRAY, 1);
	case PDF_CS_RGB:
		return new_cs(PDF_CS_RGB, 3);
	case PDF_CS_CMYK:
		return new_cs(PDF_CS_CMYK, 4);
	default:
		return NULL;
	}
}

void pdf_colorspace_free(struct pdf_colorspace *cs)
{
	if (cs == NULL) {
		return;
	}
	pdf_colorspace_free(cs->base);
	pdf_free(cs->lookup);
	pdf_function_free(cs->tint);
	pdf_free(cs);
}

static struct pdf_colorspace *load(struct pdf_doc *doc, struct pdf_obj *obj, struct pdf_obj *resources,
				   int depth)
{
	obj = pdf_resolve(doc, obj);
	if (obj == NULL || depth > 8) {
		return NULL;
	}
	if (obj->type == PDF_NAME) {
		const char *n = pdf_name_str(obj);

		if (strcmp(n, "DeviceGray") == 0 || strcmp(n, "G") == 0 || strcmp(n, "CalGray") == 0) {
			return new_cs(PDF_CS_GRAY, 1);
		}
		if (strcmp(n, "DeviceRGB") == 0 || strcmp(n, "RGB") == 0 || strcmp(n, "CalRGB") == 0) {
			return new_cs(PDF_CS_RGB, 3);
		}
		if (strcmp(n, "DeviceCMYK") == 0 || strcmp(n, "CMYK") == 0) {
			return new_cs(PDF_CS_CMYK, 4);
		}
		if (strcmp(n, "Pattern") == 0) {
			return new_cs(PDF_CS_PATTERN, 1);
		}
		if (strcmp(n, "Indexed") == 0 || strcmp(n, "I") == 0) {
			return NULL;
		}
		/* named resource */
		struct pdf_obj *csd = pdf_get(doc, resources, "ColorSpace");
		struct pdf_obj *o = pdf_get(doc, csd, n);

		if (o != NULL && o != obj) {
			return load(doc, o, NULL, depth + 1);
		}
		return NULL;
	}
	if (obj->type != PDF_ARRAY || obj->arr.n == 0) {
		return NULL;
	}
	struct pdf_obj *fam = pdf_array_get(doc, obj, 0);
	const char *n = pdf_name_str(fam);

	if (obj->arr.n == 1) {
		return load(doc, fam, resources, depth + 1);
	}
	if (strcmp(n, "ICCBased") == 0) {
		struct pdf_obj *s = pdf_array_get(doc, obj, 1);
		int ncomp = (int)pdf_int(pdf_get(doc, s, "N"));

		if (ncomp != 1 && ncomp != 3 && ncomp != 4) {
			struct pdf_obj *alt = pdf_get(doc, s, "Alternate");
			struct pdf_colorspace *a = alt ? load(doc, alt, resources, depth + 1) : NULL;

			if (a != NULL) {
				return a;
			}
			ncomp = 3;
		}
		return new_cs(ncomp == 1 ? PDF_CS_GRAY : (ncomp == 4 ? PDF_CS_CMYK : PDF_CS_RGB), ncomp);
	}
	if (strcmp(n, "CalRGB") == 0) {
		return new_cs(PDF_CS_RGB, 3);
	}
	if (strcmp(n, "CalGray") == 0) {
		return new_cs(PDF_CS_GRAY, 1);
	}
	if (strcmp(n, "CalCMYK") == 0) {
		return new_cs(PDF_CS_CMYK, 4);
	}
	if (strcmp(n, "Lab") == 0) {
		struct pdf_colorspace *cs = new_cs(PDF_CS_LAB, 3);
		struct pdf_obj *d = pdf_array_get(doc, obj, 1);

		if (cs == NULL) {
			return NULL;
		}
		cs->lab_range[0] = -100;
		cs->lab_range[1] = 100;
		cs->lab_range[2] = -100;
		cs->lab_range[3] = 100;
		cs->lab_wp[0] = 0.9505f;
		cs->lab_wp[1] = 1.0f;
		cs->lab_wp[2] = 1.089f;
		struct pdf_obj *r = pdf_get(doc, d, "Range");
		struct pdf_obj *wp = pdf_get(doc, d, "WhitePoint");

		for (int i = 0; i < 4 && i < (int)pdf_array_len(r); i++) {
			cs->lab_range[i] = pdf_num(pdf_array_get(doc, r, (size_t)i));
		}
		for (int i = 0; i < 3 && i < (int)pdf_array_len(wp); i++) {
			cs->lab_wp[i] = pdf_num(pdf_array_get(doc, wp, (size_t)i));
		}
		return cs;
	}
	if (strcmp(n, "Indexed") == 0 || strcmp(n, "I") == 0) {
		struct pdf_colorspace *base = load(doc, pdf_array_get(doc, obj, 1), resources, depth + 1);

		if (base == NULL) {
			return NULL;
		}
		struct pdf_colorspace *cs = new_cs(PDF_CS_INDEXED, 1);

		if (cs == NULL) {
			pdf_colorspace_free(base);
			return NULL;
		}
		cs->base = base;
		cs->hival = (int)pdf_int(pdf_array_get(doc, obj, 2));
		if (cs->hival < 0) {
			cs->hival = 0;
		}
		if (cs->hival > 255) {
			cs->hival = 255;
		}
		size_t need = (size_t)(cs->hival + 1) * (size_t)base->ncomps;
		struct pdf_obj *lk = pdf_array_get(doc, obj, 3);

		cs->lookup = pdf_calloc(need);
		if (cs->lookup == NULL) {
			pdf_colorspace_free(cs);
			return NULL;
		}
		if (lk != NULL && lk->type == PDF_STRING) {
			memcpy(cs->lookup, lk->str.data, lk->str.len < need ? lk->str.len : need);
		} else if (lk != NULL && lk->type == PDF_STREAM) {
			uint8_t *d;
			size_t dl;

			if (pdf_stream_decode(doc, lk, &d, &dl) >= 0) {
				memcpy(cs->lookup, d, dl < need ? dl : need);
				pdf_free(d);
			}
		}
		return cs;
	}
	if (strcmp(n, "Separation") == 0 || strcmp(n, "DeviceN") == 0) {
		bool devn = n[0] == 'D';
		struct pdf_obj *names = pdf_array_get(doc, obj, 1);
		int ncomp = devn ? (int)pdf_array_len(names) : 1;

		if (ncomp < 1) {
			ncomp = 1;
		}
		if (ncomp > 32) {
			ncomp = 32;
		}
		struct pdf_colorspace *cs = new_cs(PDF_CS_SEPARATION, ncomp);

		if (cs == NULL) {
			return NULL;
		}
		if (!devn && pdf_is_name(names, "None")) {
			cs->is_none = true;
		}
		if (!devn && pdf_is_name(names, "All")) {
			cs->is_all = true;
		}
		cs->base = load(doc, pdf_array_get(doc, obj, 2), resources, depth + 1);
		cs->tint = pdf_function_load(doc, pdf_array_get(doc, obj, 3));
		if (cs->base == NULL) {
			cs->base = new_cs(PDF_CS_GRAY, 1);
		}
		return cs;
	}
	if (strcmp(n, "Pattern") == 0) {
		struct pdf_colorspace *cs = new_cs(PDF_CS_PATTERN, 1);

		if (cs != NULL) {
			cs->base = load(doc, pdf_array_get(doc, obj, 1), resources, depth + 1);
		}
		return cs;
	}
	if (strcmp(n, "DeviceGray") == 0 || strcmp(n, "DeviceRGB") == 0 || strcmp(n, "DeviceCMYK") == 0) {
		return load(doc, fam, resources, depth + 1);
	}
	return NULL;
}

struct pdf_colorspace *pdf_colorspace_load(struct pdf_doc *doc, struct pdf_obj *obj,
					   struct pdf_obj *resources)
{
	return load(doc, obj, resources, 0);
}

void pdf_colorspace_initial(const struct pdf_colorspace *cs, float *comps)
{
	for (int i = 0; i < cs->ncomps; i++) {
		comps[i] = 0.0f;
	}
	if (cs->type == PDF_CS_CMYK) {
		comps[3] = 1.0f;
	} else if (cs->type == PDF_CS_SEPARATION) {
		for (int i = 0; i < cs->ncomps; i++) {
			comps[i] = 1.0f;
		}
	}
}

void pdf_colorspace_decode_default(const struct pdf_colorspace *cs, int bpc, float *decode)
{
	float maxv = (float)((1u << bpc) - 1);

	for (int i = 0; i < cs->ncomps; i++) {
		decode[i * 2] = 0.0f;
		decode[i * 2 + 1] = 1.0f;
	}
	if (cs->type == PDF_CS_INDEXED) {
		decode[1] = maxv;
	} else if (cs->type == PDF_CS_LAB) {
		decode[0] = 0;
		decode[1] = 100;
		decode[2] = cs->lab_range[0];
		decode[3] = cs->lab_range[1];
		decode[4] = cs->lab_range[2];
		decode[5] = cs->lab_range[3];
	}
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float lab_inv(float t)
{
	return t > 6.0f / 29.0f ? t * t * t : 3.0f * (6.0f / 29.0f) * (6.0f / 29.0f) * (t - 4.0f / 29.0f);
}

void pdf_colorspace_to_rgb(const struct pdf_colorspace *cs, const float *comps, float rgb[3])
{
	switch (cs->type) {
	case PDF_CS_GRAY:
		rgb[0] = rgb[1] = rgb[2] = clamp01(comps[0]);
		break;
	case PDF_CS_RGB:
		rgb[0] = clamp01(comps[0]);
		rgb[1] = clamp01(comps[1]);
		rgb[2] = clamp01(comps[2]);
		break;
	case PDF_CS_CMYK: {
		float k = clamp01(comps[3]);

		rgb[0] = (1.0f - clamp01(comps[0])) * (1.0f - k);
		rgb[1] = (1.0f - clamp01(comps[1])) * (1.0f - k);
		rgb[2] = (1.0f - clamp01(comps[2])) * (1.0f - k);
		break;
	}
	case PDF_CS_LAB: {
		float L = comps[0], a = comps[1], b = comps[2];
		float m = (L + 16.0f) / 116.0f;
		float l = m + a / 500.0f, n = m - b / 200.0f;
		float X = cs->lab_wp[0] * lab_inv(l), Y = cs->lab_wp[1] * lab_inv(m), Z = cs->lab_wp[2] * lab_inv(n);
		float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
		float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
		float bb = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;

		/* gamma */
		r = r <= 0.0031308f ? 12.92f * r : 1.055f * powf(r > 0 ? r : 0, 1 / 2.4f) - 0.055f;
		g = g <= 0.0031308f ? 12.92f * g : 1.055f * powf(g > 0 ? g : 0, 1 / 2.4f) - 0.055f;
		bb = bb <= 0.0031308f ? 12.92f * bb : 1.055f * powf(bb > 0 ? bb : 0, 1 / 2.4f) - 0.055f;
		rgb[0] = clamp01(r);
		rgb[1] = clamp01(g);
		rgb[2] = clamp01(bb);
		break;
	}
	case PDF_CS_INDEXED: {
		int idx = (int)(comps[0] + 0.5f);

		if (idx < 0) {
			idx = 0;
		}
		if (idx > cs->hival) {
			idx = cs->hival;
		}
		float bc[32];
		int nb = cs->base->ncomps;

		if (nb > 32) {
			nb = 32;
		}
		float decode[64];

		pdf_colorspace_decode_default(cs->base, 8, decode);
		for (int i = 0; i < nb; i++) {
			float v = cs->lookup[idx * cs->base->ncomps + i] / 255.0f;

			bc[i] = decode[i * 2] + v * (decode[i * 2 + 1] - decode[i * 2]);
		}
		pdf_colorspace_to_rgb(cs->base, bc, rgb);
		break;
	}
	case PDF_CS_SEPARATION: {
		if (cs->is_none) {
			rgb[0] = rgb[1] = rgb[2] = 1.0f;
			break;
		}
		float out[PDF_FUNC_MAX_OUT];
		int nout = 0;

		if (cs->tint != NULL && pdf_function_eval(cs->tint, comps, cs->ncomps, out, &nout) == 0 &&
		    nout >= cs->base->ncomps) {
			pdf_colorspace_to_rgb(cs->base, out, rgb);
		} else {
			/* no usable tint transform: treat as gray ink */
			float t = clamp01(comps[0]);

			rgb[0] = rgb[1] = rgb[2] = 1.0f - t;
		}
		break;
	}
	case PDF_CS_PATTERN:
	default:
		rgb[0] = rgb[1] = rgb[2] = 0.5f;
		break;
	}
}
