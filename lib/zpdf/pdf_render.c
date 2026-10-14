/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Content stream painter (sections 8 and 9). Device space is the output
 * bitmap, y down; the page is mapped by base_ctm. Everything is drawn in
 * place with alpha blending; clipping and soft masks are coverage planes.
 */
#include <zpdf/pdf_render.h>
#include "pdf_font.h"
#include "pdf_lex.h"
#include "pdf_mem.h"
#include "pdf_util.h"
#include "pdf_raster.h"
#include "pdf_color.h"
#include "pdf_func.h"
#include "pdf_image.h"
#include "pdf_ttf.h"
#include "pdf_cff.h"
#include "pdf_encodings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PDF_LOG_REGISTER();

#ifndef CONFIG_ZPDF_GLYPH_CACHE_BYTES
#define CONFIG_ZPDF_GLYPH_CACHE_BYTES (2u << 20)
#endif
#define GLYPH_CACHE_SLOTS 1024
#define GLYPH_CACHE_MAX_PX 96

#define GS_MAX 48
#define OPERANDS 32
#define MAX_FORM_DEPTH 14
#define MAX_OPS 4000000
#define MAX_DASH 16
#define MIN_LINE_WIDTH 0.8f

/* built-in fallback fonts (tools/mkfont) */
extern const unsigned char zpdf_font_sans[], zpdf_font_sans_bold[], zpdf_font_sans_italic[],
	zpdf_font_sans_bolditalic[], zpdf_font_serif[], zpdf_font_serif_bold[], zpdf_font_serif_italic[],
	zpdf_font_serif_bolditalic[], zpdf_font_mono[], zpdf_font_mono_bold[], zpdf_font_symbol[];
extern const unsigned int zpdf_font_sans_len, zpdf_font_sans_bold_len, zpdf_font_sans_italic_len,
	zpdf_font_sans_bolditalic_len, zpdf_font_serif_len, zpdf_font_serif_bold_len,
	zpdf_font_serif_italic_len, zpdf_font_serif_bolditalic_len, zpdf_font_mono_len,
	zpdf_font_mono_bold_len, zpdf_font_symbol_len;

/* ---------------------------------------------------------------------- */
/* small matrix helpers (a b c d e f)                                      */

static void mat_mul(const float m[6], const float n[6], float out[6])
{
	float r[6];

	r[0] = m[0] * n[0] + m[1] * n[2];
	r[1] = m[0] * n[1] + m[1] * n[3];
	r[2] = m[2] * n[0] + m[3] * n[2];
	r[3] = m[2] * n[1] + m[3] * n[3];
	r[4] = m[4] * n[0] + m[5] * n[2] + n[4];
	r[5] = m[4] * n[1] + m[5] * n[3] + n[5];
	memcpy(out, r, sizeof(r));
}

static void mat_apply(const float m[6], float x, float y, float *ox, float *oy)
{
	*ox = m[0] * x + m[2] * y + m[4];
	*oy = m[1] * x + m[3] * y + m[5];
}

static bool mat_invert(const float m[6], float out[6])
{
	float det = m[0] * m[3] - m[1] * m[2];

	if (fabsf(det) < 1e-12f) {
		return false;
	}
	float id = 1.0f / det;

	out[0] = m[3] * id;
	out[1] = -m[1] * id;
	out[2] = -m[2] * id;
	out[3] = m[0] * id;
	out[4] = -(m[4] * out[0] + m[5] * out[2]);
	out[5] = -(m[4] * out[1] + m[5] * out[3]);
	return true;
}

static float mat_scale(const float m[6])
{
	return sqrtf(fabsf(m[0] * m[3] - m[1] * m[2]));
}

static const float identity[6] = {1, 0, 0, 1, 0, 0};

/* ---------------------------------------------------------------------- */
/* coverage planes                                                        */

struct mask {
	uint8_t *data;
	int refs;
};

struct rctx;

static struct mask *mask_new(struct rctx *c);
static void mask_unref(struct mask *m)
{
	if (m != NULL && --m->refs == 0) {
		pdf_free(m->data);
		pdf_free(m);
	}
}

static struct mask *mask_ref(struct mask *m)
{
	if (m != NULL) {
		m->refs++;
	}
	return m;
}

/* ---------------------------------------------------------------------- */
/* fonts for rendering                                                    */

enum glyph_src {
	SRC_NONE,
	SRC_TTF,
	SRC_CFF,
	SRC_TYPE3,
};

/* A decoded and parsed font program, shared by every font dictionary using it. */
struct font_program {
	uint8_t *data;
	size_t len;
	enum glyph_src kind;
	struct ttf_font ttf;
	struct cff_font *cff;
};

static void font_program_free(void *p)
{
	struct font_program *fp = p;

	if (fp == NULL) {
		return;
	}
	cff_free(fp->cff);
	pdf_free(fp->data);
	pdf_free(fp);
}

struct rfont {
	struct pdf_obj *dict;
	struct pdf_font *pf;
	enum glyph_src src;
	struct ttf_font builtin_ttf;
	const struct ttf_font *ttf; /* &builtin_ttf or the program's */
	struct cff_font *cff;
	struct font_program *prog;  /* from the document cache, referenced */
	const void *prog_key;
	struct pdf_doc *doc;
	float fm[6];      /* glyph space -> text space (1/1000 or 1/upem) */
	bool builtin;
	bool symbolic;
	char *names[256]; /* /Differences glyph names (simple fonts) */
	bool has_names;
	uint8_t *cid2gid;
	size_t cid2gid_len;
	/* type 3 */
	struct pdf_obj *charprocs;
	struct pdf_obj *t3res;
	/* cache of code -> gid */
	int32_t gid_cache[256];
};

/* ---------------------------------------------------------------------- */
/* graphics state                                                         */

struct gstate {
	float ctm[6];
	struct rast_clip clip;
	struct mask *clip_mask;
	struct mask *smask;
	float fill_rgb[3], stroke_rgb[3];
	struct pdf_colorspace *fill_cs, *stroke_cs;
	struct pdf_obj *fill_pat, *stroke_pat;
	float fill_alpha, stroke_alpha;
	int blend;
	float line_width;
	int line_cap, line_join;
	float dash[MAX_DASH];
	int ndash;
	float dash_phase;
	/* text state */
	struct rfont *font;
	float font_size, tc, tw, th, tl, trise;
	int tmode;
};

struct dpath {
	float *pts;   /* x,y pairs */
	uint8_t *ops; /* per op: 0 move 1 line 2 curve (3 points) 3 close */
	size_t npts, nops, cap_pts, cap_ops;
	float start_x, start_y, cur_x, cur_y;
	bool oom;
};

struct imgcache {
	struct pdf_obj *stream;
	struct pdf_image *img;
};

struct glyph_entry {
	const void *font;
	uint32_t gid;
	int32_t ma, mb, mc, md; /* matrix * 256 */
	uint8_t subx;           /* horizontal subpixel position in quarters */
	bool used;
	bool empty;
	int16_t x0, y0;
	uint16_t w, h;
	uint8_t *cov;
};

struct rctx {
	struct pdf_doc *doc;
	int w, h;
	struct glyph_entry *glyphs;
	size_t glyph_bytes;
	long glyph_hits, glyph_misses;
	uint8_t *rgb;
	float base_ctm[6];
	float pattern_base[6];
	struct gstate gs[GS_MAX];
	int gsp;
	struct dpath path;
	int pending_clip; /* 0 none, 1 nonzero, 2 even-odd */
	float tm[6], tlm[6];
	struct rast_path rp;
	struct rast_scratch scratch;
	struct rfont **fonts;
	int nfonts, fonts_cap;
	struct imgcache imgs[8];
	int depth;
	struct pdf_obj *form_path[MAX_FORM_DEPTH];
	long ops;
	bool ignore_color; /* inside an uncoloured tiling pattern */
	int t3_depth;
	/* soft mask and alpha pinned for a transparency group form */
	struct mask *group_mask;
	float group_alpha;
};

static struct mask *mask_new(struct rctx *c)
{
	struct mask *m = pdf_calloc(sizeof(*m));

	if (m == NULL) {
		return NULL;
	}
	m->data = pdf_calloc((size_t)c->w * (size_t)c->h);
	if (m->data == NULL) {
		pdf_free(m);
		return NULL;
	}
	m->refs = 1;
	return m;
}

static struct gstate *cur(struct rctx *c)
{
	return &c->gs[c->gsp];
}

static void cs_unref(struct pdf_colorspace *cs)
{
	if (cs != NULL && --cs->refs <= 0) {
		pdf_colorspace_free(cs);
	}
}

static struct pdf_colorspace *cs_ref(struct pdf_colorspace *cs)
{
	if (cs != NULL) {
		cs->refs++;
	}
	return cs;
}

static void gs_release(struct gstate *g)
{
	mask_unref(g->clip_mask);
	mask_unref(g->smask);
	cs_unref(g->fill_cs);
	cs_unref(g->stroke_cs);
	g->clip_mask = NULL;
	g->smask = NULL;
	g->fill_cs = NULL;
	g->stroke_cs = NULL;
}

static void gs_push(struct rctx *c)
{
	if (c->gsp + 1 >= GS_MAX) {
		return;
	}
	struct gstate *g = cur(c);

	c->gs[c->gsp + 1] = *g;
	c->gsp++;
	mask_ref(g->clip_mask);
	mask_ref(g->smask);
	cs_ref(g->fill_cs);
	cs_ref(g->stroke_cs);
}

static void gs_pop(struct rctx *c)
{
	if (c->gsp == 0) {
		return;
	}
	gs_release(cur(c));
	c->gsp--;
}

/* ---------------------------------------------------------------------- */
/* pixel blending                                                         */

struct paint {
	struct rctx *c;
	uint8_t rgb[3];
	int alpha; /* 0..255 */
	const uint8_t *clip;
	const uint8_t *smask;
	const uint8_t *group_mask;
	int blend;
	/* optional per-pixel colour source (images, shadings) */
	bool (*source)(void *sud, int x, int y, uint8_t rgb[3], int *a);
	void *sud;
	/* optional destination mask instead of the framebuffer */
	uint8_t *dst_mask;
};

static inline uint8_t blend_channel(int mode, int s, int d)
{
	switch (mode) {
	case 1: /* multiply */
		return (uint8_t)((s * d + 127) / 255);
	case 2: /* screen */
		return (uint8_t)(s + d - (s * d + 127) / 255);
	case 3: /* darken */
		return (uint8_t)(s < d ? s : d);
	case 4: /* lighten */
		return (uint8_t)(s > d ? s : d);
	case 5: /* difference */
		return (uint8_t)(s > d ? s - d : d - s);
	case 6: /* exclusion */
		return (uint8_t)(s + d - (2 * s * d + 127) / 255);
	default:
		return (uint8_t)s;
	}
}

static void span_paint(void *ud, int y, int x0, const uint8_t *cov, int n)
{
	struct paint *p = ud;
	struct rctx *c = p->c;

	if (y < 0 || y >= c->h) {
		return;
	}
	size_t row = (size_t)y * (size_t)c->w;

	for (int i = 0; i < n; i++) {
		int x = x0 + i;

		if (x < 0 || x >= c->w) {
			continue;
		}
		int a = cov[i];

		if (a == 0) {
			continue;
		}
		a = a * p->alpha / 255;
		if (p->clip != NULL) {
			a = a * p->clip[row + (size_t)x] / 255;
		}
		if (p->smask != NULL) {
			a = a * p->smask[row + (size_t)x] / 255;
		}
		if (p->group_mask != NULL) {
			a = a * p->group_mask[row + (size_t)x] / 255;
		}
		if (a == 0) {
			continue;
		}
		uint8_t src[3];

		if (p->source != NULL) {
			int sa = 255;

			if (!p->source(p->sud, x, y, src, &sa)) {
				continue;
			}
			a = a * sa / 255;
			if (a == 0) {
				continue;
			}
		} else {
			src[0] = p->rgb[0];
			src[1] = p->rgb[1];
			src[2] = p->rgb[2];
		}
		if (p->dst_mask != NULL) {
			/* coverage accumulation into a mask plane */
			int old = p->dst_mask[row + (size_t)x];
			int v = old + a - old * a / 255;

			p->dst_mask[row + (size_t)x] = (uint8_t)(v > 255 ? 255 : v);
			continue;
		}
		uint8_t *d = c->rgb + (row + (size_t)x) * 3;

		for (int k = 0; k < 3; k++) {
			int s = p->blend ? blend_channel(p->blend, src[k], d[k]) : src[k];

			d[k] = (uint8_t)(d[k] + ((s - d[k]) * a + 127) / 255);
		}
	}
}

static void paint_init(struct paint *p, struct rctx *c, const float rgb[3], float alpha, bool stroke)
{
	struct gstate *g = cur(c);

	memset(p, 0, sizeof(*p));
	p->c = c;
	p->rgb[0] = (uint8_t)(rgb[0] * 255.0f + 0.5f);
	p->rgb[1] = (uint8_t)(rgb[1] * 255.0f + 0.5f);
	p->rgb[2] = (uint8_t)(rgb[2] * 255.0f + 0.5f);
	p->alpha = (int)(alpha * c->group_alpha * 255.0f + 0.5f);
	if (p->alpha > 255) {
		p->alpha = 255;
	}
	p->clip = g->clip_mask ? g->clip_mask->data : NULL;
	p->smask = g->smask ? g->smask->data : NULL;
	p->group_mask = c->group_mask ? c->group_mask->data : NULL;
	p->blend = g->blend;
	(void)stroke;
}

/* ---------------------------------------------------------------------- */
/* path construction (device space)                                       */

static void dpath_reset(struct dpath *p)
{
	p->npts = p->nops = 0;
	p->oom = false;
}

static void dpath_free(struct dpath *p)
{
	pdf_free(p->pts);
	pdf_free(p->ops);
	memset(p, 0, sizeof(*p));
}

static bool dpath_reserve(struct dpath *p, size_t pts, size_t ops)
{
	if (p->npts + pts > p->cap_pts) {
		size_t ncap = p->cap_pts ? p->cap_pts * 2 : 256;

		while (ncap < p->npts + pts) {
			ncap *= 2;
		}
		float *np = pdf_realloc(p->pts, ncap * 2 * sizeof(float));

		if (np == NULL) {
			p->oom = true;
			return false;
		}
		p->pts = np;
		p->cap_pts = ncap;
	}
	if (p->nops + ops > p->cap_ops) {
		size_t ncap = p->cap_ops ? p->cap_ops * 2 : 128;

		while (ncap < p->nops + ops) {
			ncap *= 2;
		}
		uint8_t *no = pdf_realloc(p->ops, ncap);

		if (no == NULL) {
			p->oom = true;
			return false;
		}
		p->ops = no;
		p->cap_ops = ncap;
	}
	return true;
}

static void dpath_add(struct dpath *p, uint8_t op, const float *xy, int n)
{
	if (p->oom || !dpath_reserve(p, (size_t)n, 1)) {
		return;
	}
	p->ops[p->nops++] = op;
	for (int i = 0; i < n; i++) {
		p->pts[p->npts * 2] = xy[i * 2];
		p->pts[p->npts * 2 + 1] = xy[i * 2 + 1];
		p->npts++;
	}
	if (n > 0) {
		p->cur_x = xy[(n - 1) * 2];
		p->cur_y = xy[(n - 1) * 2 + 1];
	}
	if (op == 0) {
		p->start_x = p->cur_x;
		p->start_y = p->cur_y;
	}
	if (op == 3) {
		p->cur_x = p->start_x;
		p->cur_y = p->start_y;
	}
}

static void path_moveto(struct rctx *c, float x, float y)
{
	float d[2];

	mat_apply(cur(c)->ctm, x, y, &d[0], &d[1]);
	dpath_add(&c->path, 0, d, 1);
}

static void path_lineto(struct rctx *c, float x, float y)
{
	float d[2];

	if (c->path.nops == 0) {
		path_moveto(c, x, y);
		return;
	}
	mat_apply(cur(c)->ctm, x, y, &d[0], &d[1]);
	dpath_add(&c->path, 1, d, 1);
}

static void path_curveto(struct rctx *c, float x1, float y1, float x2, float y2, float x3, float y3)
{
	float d[6];

	if (c->path.nops == 0) {
		path_moveto(c, x1, y1);
	}
	mat_apply(cur(c)->ctm, x1, y1, &d[0], &d[1]);
	mat_apply(cur(c)->ctm, x2, y2, &d[2], &d[3]);
	mat_apply(cur(c)->ctm, x3, y3, &d[4], &d[5]);
	dpath_add(&c->path, 2, d, 3);
}

static void path_close(struct rctx *c)
{
	if (c->path.nops > 0 && c->path.ops[c->path.nops - 1] != 3) {
		dpath_add(&c->path, 3, NULL, 0);
	}
}

/* current point in user space (for v/y operators) */
static void path_current_user(struct rctx *c, float *x, float *y)
{
	float inv[6];

	if (mat_invert(cur(c)->ctm, inv)) {
		mat_apply(inv, c->path.cur_x, c->path.cur_y, x, y);
	} else {
		*x = *y = 0;
	}
}

/* Convert the device path into rasterizer edges. */
static void dpath_to_rast(const struct dpath *p, struct rast_path *rp)
{
	size_t pi = 0;

	rast_path_reset(rp);
	for (size_t i = 0; i < p->nops; i++) {
		switch (p->ops[i]) {
		case 0:
			rast_move_to(rp, p->pts[pi * 2], p->pts[pi * 2 + 1]);
			pi++;
			break;
		case 1:
			rast_line_to(rp, p->pts[pi * 2], p->pts[pi * 2 + 1]);
			pi++;
			break;
		case 2:
			rast_curve_to(rp, p->pts[pi * 2], p->pts[pi * 2 + 1], p->pts[pi * 2 + 2],
				      p->pts[pi * 2 + 3], p->pts[pi * 2 + 4], p->pts[pi * 2 + 5]);
			pi += 3;
			break;
		case 3:
			rast_close(rp);
			break;
		default:
			break;
		}
	}
	rast_close(rp);
}

/* Is the path a single axis-aligned rectangle? Returns its device bounds. */
static bool dpath_is_rect(const struct dpath *p, float *x0, float *y0, float *x1, float *y1)
{
	if (p->nops < 4 || p->nops > 6) {
		return false;
	}
	if (p->ops[0] != 0) {
		return false;
	}
	size_t npts = 0;

	for (size_t i = 0; i < p->nops; i++) {
		if (p->ops[i] == 2) {
			return false;
		}
		if (p->ops[i] == 0 && i > 0) {
			return false;
		}
		if (p->ops[i] == 1 || p->ops[i] == 0) {
			npts++;
		}
	}
	if (npts != 4 && npts != 5) {
		return false;
	}
	if (npts == 5 && (p->pts[8] != p->pts[0] || p->pts[9] != p->pts[1])) {
		return false;
	}
	const float *q = p->pts;
	bool ok = (q[0] == q[6] && q[2] == q[4] && q[1] == q[3] && q[5] == q[7]) ||
		  (q[1] == q[7] && q[3] == q[5] && q[0] == q[2] && q[4] == q[6]);

	if (!ok) {
		return false;
	}
	*x0 = *x1 = q[0];
	*y0 = *y1 = q[1];
	for (int i = 1; i < 4; i++) {
		if (q[i * 2] < *x0) {
			*x0 = q[i * 2];
		}
		if (q[i * 2] > *x1) {
			*x1 = q[i * 2];
		}
		if (q[i * 2 + 1] < *y0) {
			*y0 = q[i * 2 + 1];
		}
		if (q[i * 2 + 1] > *y1) {
			*y1 = q[i * 2 + 1];
		}
	}
	return true;
}

/* ---------------------------------------------------------------------- */
/* clipping                                                               */

static void clip_intersect_rect(struct gstate *g, float x0, float y0, float x1, float y1)
{
	int ix0 = (int)floorf(x0 + 0.5f), iy0 = (int)floorf(y0 + 0.5f);
	int ix1 = (int)floorf(x1 + 0.5f), iy1 = (int)floorf(y1 + 0.5f);

	if (ix0 > g->clip.x0) {
		g->clip.x0 = ix0;
	}
	if (iy0 > g->clip.y0) {
		g->clip.y0 = iy0;
	}
	if (ix1 < g->clip.x1) {
		g->clip.x1 = ix1;
	}
	if (iy1 < g->clip.y1) {
		g->clip.y1 = iy1;
	}
	if (g->clip.x1 < g->clip.x0) {
		g->clip.x1 = g->clip.x0;
	}
	if (g->clip.y1 < g->clip.y0) {
		g->clip.y1 = g->clip.y0;
	}
}

static void span_mask_write(void *ud, int y, int x0, const uint8_t *cov, int n)
{
	struct paint *p = ud;
	struct rctx *c = p->c;

	if (y < 0 || y >= c->h) {
		return;
	}
	for (int i = 0; i < n; i++) {
		int x = x0 + i;

		if (x >= 0 && x < c->w) {
			p->dst_mask[(size_t)y * (size_t)c->w + (size_t)x] = cov[i];
		}
	}
}

/* Intersect the clip with the rasterized path in c->rp. */
static void clip_with_rast(struct rctx *c, struct rast_path *rp, bool evenodd)
{
	struct gstate *g = cur(c);
	struct mask *m = mask_new(c);

	if (m == NULL) {
		return;
	}
	struct paint p = {.c = c, .dst_mask = m->data};

	rast_fill_scratch(rp, evenodd, &g->clip, span_mask_write, &p, &c->scratch);
	if (g->clip_mask != NULL) {
		size_t n = (size_t)c->w * (size_t)c->h;

		for (size_t i = 0; i < n; i++) {
			m->data[i] = (uint8_t)((m->data[i] * g->clip_mask->data[i] + 127) / 255);
		}
		mask_unref(g->clip_mask);
	}
	g->clip_mask = m;
	clip_intersect_rect(g, floorf(rp->minx), floorf(rp->miny), ceilf(rp->maxx) + 1, ceilf(rp->maxy) + 1);
}

static void apply_pending_clip(struct rctx *c)
{
	if (c->pending_clip == 0) {
		return;
	}
	bool evenodd = c->pending_clip == 2;

	c->pending_clip = 0;
	float x0, y0, x1, y1;

	if (c->path.nops == 0) {
		cur(c)->clip.x1 = cur(c)->clip.x0;
		return;
	}
	if (dpath_is_rect(&c->path, &x0, &y0, &x1, &y1)) {
		clip_intersect_rect(cur(c), x0, y0, x1, y1);
		return;
	}
	dpath_to_rast(&c->path, &c->rp);
	clip_with_rast(c, &c->rp, evenodd);
}

/* ---------------------------------------------------------------------- */
/* shadings                                                               */

struct shading {
	int type;
	float coords[6];
	float t0, t1;
	bool extend0, extend1;
	uint8_t lut[256][3];
	bool has_lut;
	uint8_t avg[3];
	float inv[6]; /* device -> shading space */
	struct pdf_function *fn;
	struct pdf_colorspace *cs;
	float domain[4];
	float fn_matrix_inv[6];
	bool has_fn_matrix;
};

static bool shading_source(void *sud, int x, int y, uint8_t rgb[3], int *a)
{
	struct shading *s = sud;
	float px, py;

	mat_apply(s->inv, (float)x + 0.5f, (float)y + 0.5f, &px, &py);
	*a = 255;
	if (s->type == 2) {
		float dx = s->coords[2] - s->coords[0], dy = s->coords[3] - s->coords[1];
		float den = dx * dx + dy * dy;
		float t = den > 0 ? ((px - s->coords[0]) * dx + (py - s->coords[1]) * dy) / den : 0;

		if (t < 0) {
			if (!s->extend0) {
				return false;
			}
			t = 0;
		}
		if (t > 1) {
			if (!s->extend1) {
				return false;
			}
			t = 1;
		}
		memcpy(rgb, s->lut[(int)(t * 255.0f)], 3);
		return true;
	}
	if (s->type == 3) {
		float x0 = s->coords[0], y0 = s->coords[1], r0 = s->coords[2];
		float x1 = s->coords[3], y1 = s->coords[4], r1 = s->coords[5];
		float cdx = x1 - x0, cdy = y1 - y0, dr = r1 - r0;
		float pdx = px - x0, pdy = py - y0;
		float a2 = cdx * cdx + cdy * cdy - dr * dr;
		float b = pdx * cdx + pdy * cdy + r0 * dr;
		float cc = pdx * pdx + pdy * pdy - r0 * r0;
		float t;

		if (fabsf(a2) < 1e-6f) {
			if (fabsf(b) < 1e-9f) {
				return false;
			}
			t = cc / (2 * b);
		} else {
			float disc = b * b - a2 * cc;

			if (disc < 0) {
				return false;
			}
			float sq = sqrtf(disc);
			float t1 = (b + sq) / a2, t2 = (b - sq) / a2;

			if (t1 < t2) {
				float tmp = t1;

				t1 = t2;
				t2 = tmp;
			}
			/* prefer the larger s with r(s) >= 0 within the extension rules */
			t = t1;
			if (r0 + t * dr < 0 || (t > 1 && !s->extend1) || (t < 0 && !s->extend0)) {
				t = t2;
				if (r0 + t * dr < 0) {
					return false;
				}
			}
		}
		if (t < 0) {
			if (!s->extend0) {
				return false;
			}
			t = 0;
		}
		if (t > 1) {
			if (!s->extend1) {
				return false;
			}
			t = 1;
		}
		memcpy(rgb, s->lut[(int)(t * 255.0f)], 3);
		return true;
	}
	if (s->type == 1) {
		float fx = px, fy = py;

		if (s->has_fn_matrix) {
			mat_apply(s->fn_matrix_inv, px, py, &fx, &fy);
		}
		if (fx < s->domain[0] || fx > s->domain[1] || fy < s->domain[2] || fy > s->domain[3]) {
			return false;
		}
		float in[2] = {fx, fy}, out[PDF_FUNC_MAX_OUT], col[3];
		int nout = 0;

		if (s->fn == NULL || pdf_function_eval(s->fn, in, 2, out, &nout) != 0 || nout < s->cs->ncomps) {
			return false;
		}
		pdf_colorspace_to_rgb(s->cs, out, col);
		rgb[0] = (uint8_t)(col[0] * 255 + 0.5f);
		rgb[1] = (uint8_t)(col[1] * 255 + 0.5f);
		rgb[2] = (uint8_t)(col[2] * 255 + 0.5f);
		return true;
	}
	memcpy(rgb, s->avg, 3);
	return true;
}

static bool shading_load(struct rctx *c, struct pdf_obj *sh, const float m[6], struct shading *s)
{
	struct pdf_doc *doc = c->doc;

	memset(s, 0, sizeof(*s));
	s->type = (int)pdf_int(pdf_get(doc, sh, "ShadingType"));
	s->cs = pdf_colorspace_load(doc, pdf_get(doc, sh, "ColorSpace"), NULL);
	if (s->cs == NULL) {
		s->cs = pdf_colorspace_device(PDF_CS_RGB);
		if (s->cs == NULL) {
			return false;
		}
	}
	struct pdf_obj *fn = pdf_get(doc, sh, "Function");

	s->fn = fn ? pdf_function_load(doc, fn) : NULL;
	if (!mat_invert(m, s->inv)) {
		memcpy(s->inv, identity, sizeof(s->inv));
	}
	struct pdf_obj *co = pdf_get(doc, sh, "Coords");

	for (int i = 0; i < 6 && i < (int)pdf_array_len(co); i++) {
		s->coords[i] = pdf_num(pdf_array_get(doc, co, (size_t)i));
	}
	struct pdf_obj *dom = pdf_get(doc, sh, "Domain");

	s->t0 = 0;
	s->t1 = 1;
	s->domain[0] = s->domain[2] = 0;
	s->domain[1] = s->domain[3] = 1;
	if (pdf_array_len(dom) >= 2) {
		s->t0 = pdf_num(pdf_array_get(doc, dom, 0));
		s->t1 = pdf_num(pdf_array_get(doc, dom, 1));
		for (int i = 0; i < 4 && i < (int)pdf_array_len(dom); i++) {
			s->domain[i] = pdf_num(pdf_array_get(doc, dom, (size_t)i));
		}
	}
	struct pdf_obj *ext = pdf_get(doc, sh, "Extend");

	if (pdf_array_len(ext) >= 2) {
		struct pdf_obj *e0 = pdf_array_get(doc, ext, 0), *e1 = pdf_array_get(doc, ext, 1);

		s->extend0 = e0 && e0->type == PDF_BOOL && e0->b;
		s->extend1 = e1 && e1->type == PDF_BOOL && e1->b;
	}
	if (s->type == 1) {
		struct pdf_obj *fm = pdf_get(doc, sh, "Matrix");

		if (pdf_array_len(fm) >= 6) {
			float fmm[6];

			for (int i = 0; i < 6; i++) {
				fmm[i] = pdf_num(pdf_array_get(doc, fm, (size_t)i));
			}
			s->has_fn_matrix = mat_invert(fmm, s->fn_matrix_inv);
		}
	}
	/* colour lookup over t */
	int avg[3] = {0, 0, 0};

	for (int i = 0; i < 256; i++) {
		float t = s->t0 + (s->t1 - s->t0) * (float)i / 255.0f;
		float out[PDF_FUNC_MAX_OUT], col[3] = {0.5f, 0.5f, 0.5f};
		int nout = 0;

		if (s->fn != NULL && pdf_function_eval(s->fn, &t, 1, out, &nout) == 0 && nout >= s->cs->ncomps) {
			pdf_colorspace_to_rgb(s->cs, out, col);
		} else if (s->type >= 4) {
			/* mesh shadings carry colours in their data; use a neutral mid grey */
		}
		s->lut[i][0] = (uint8_t)(col[0] * 255 + 0.5f);
		s->lut[i][1] = (uint8_t)(col[1] * 255 + 0.5f);
		s->lut[i][2] = (uint8_t)(col[2] * 255 + 0.5f);
		avg[0] += s->lut[i][0];
		avg[1] += s->lut[i][1];
		avg[2] += s->lut[i][2];
	}
	s->avg[0] = (uint8_t)(avg[0] / 256);
	s->avg[1] = (uint8_t)(avg[1] / 256);
	s->avg[2] = (uint8_t)(avg[2] / 256);
	if (s->type >= 4 && s->fn == NULL) {
		/* mesh without function: sample a few colours from the stream would need
		 * full decoding; use the Background or mid grey */
		struct pdf_obj *bg = pdf_get(doc, sh, "Background");

		if (pdf_array_len(bg) >= (size_t)s->cs->ncomps) {
			float comps[32], col[3];

			for (int i = 0; i < s->cs->ncomps && i < 32; i++) {
				comps[i] = pdf_num(pdf_array_get(doc, bg, (size_t)i));
			}
			pdf_colorspace_to_rgb(s->cs, comps, col);
			s->avg[0] = (uint8_t)(col[0] * 255 + 0.5f);
			s->avg[1] = (uint8_t)(col[1] * 255 + 0.5f);
			s->avg[2] = (uint8_t)(col[2] * 255 + 0.5f);
		} else {
			s->avg[0] = s->avg[1] = s->avg[2] = 0x80;
		}
	}
	return true;
}

static void shading_release(struct shading *s)
{
	pdf_function_free(s->fn);
	pdf_colorspace_free(s->cs);
}

/* Paint a shading over the clip (and optionally a region mask), with alpha. */
static void paint_shading(struct rctx *c, struct pdf_obj *sh, const float m[6], struct mask *region,
			  float alpha)
{
	struct shading s;

	if (!shading_load(c, sh, m, &s)) {
		return;
	}
	struct gstate *g = cur(c);
	struct paint p;
	float white[3] = {1, 1, 1};

	paint_init(&p, c, white, alpha, false);
	p.source = shading_source;
	p.sud = &s;
	/* the region (path coverage) acts like an extra clip */
	struct mask *combined = NULL;

	if (region != NULL) {
		if (g->clip_mask != NULL) {
			combined = mask_new(c);
			if (combined != NULL) {
				size_t n = (size_t)c->w * (size_t)c->h;

				for (size_t i = 0; i < n; i++) {
					combined->data[i] = (uint8_t)((region->data[i] * g->clip_mask->data[i] + 127) / 255);
				}
				p.clip = combined->data;
			}
		} else {
			p.clip = region->data;
		}
	}
	/* BBox of the shading limits painting */
	struct rast_clip clip = g->clip;
	struct pdf_obj *bbox = pdf_get(c->doc, sh, "BBox");

	if (pdf_array_len(bbox) >= 4) {
		float bx0 = pdf_num(pdf_array_get(c->doc, bbox, 0)), by0 = pdf_num(pdf_array_get(c->doc, bbox, 1));
		float bx1 = pdf_num(pdf_array_get(c->doc, bbox, 2)), by1 = pdf_num(pdf_array_get(c->doc, bbox, 3));
		float xs[4], ys[4];

		mat_apply(m, bx0, by0, &xs[0], &ys[0]);
		mat_apply(m, bx1, by0, &xs[1], &ys[1]);
		mat_apply(m, bx1, by1, &xs[2], &ys[2]);
		mat_apply(m, bx0, by1, &xs[3], &ys[3]);
		float mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];

		for (int i = 1; i < 4; i++) {
			mnx = xs[i] < mnx ? xs[i] : mnx;
			mxx = xs[i] > mxx ? xs[i] : mxx;
			mny = ys[i] < mny ? ys[i] : mny;
			mxy = ys[i] > mxy ? ys[i] : mxy;
		}
		if ((int)floorf(mnx) > clip.x0) {
			clip.x0 = (int)floorf(mnx);
		}
		if ((int)ceilf(mxx) < clip.x1) {
			clip.x1 = (int)ceilf(mxx);
		}
		if ((int)floorf(mny) > clip.y0) {
			clip.y0 = (int)floorf(mny);
		}
		if ((int)ceilf(mxy) < clip.y1) {
			clip.y1 = (int)ceilf(mxy);
		}
	}
#if !defined(__ZEPHYR__) && defined(ZPDF_DEBUG)
	{
		float u, v;

		mat_apply(s.inv, (float)clip.x0, (float)clip.y0, &u, &v);
		fprintf(stderr, "sh type %d clip %d,%d-%d,%d m=[%g %g %g %g %g %g] corner->(%g,%g) coords %g %g %g %g ext %d %d\n",
			s.type, clip.x0, clip.y0, clip.x1, clip.y1, m[0], m[1], m[2], m[3], m[4], m[5], u, v,
			s.coords[0], s.coords[1], s.coords[2], s.coords[3], s.extend0, s.extend1);
	}
#endif
	uint8_t *cov = pdf_malloc((size_t)(clip.x1 > clip.x0 ? clip.x1 - clip.x0 : 1));

	if (cov != NULL) {
		memset(cov, 255, (size_t)(clip.x1 > clip.x0 ? clip.x1 - clip.x0 : 1));
		for (int y = clip.y0; y < clip.y1; y++) {
			span_paint(&p, y, clip.x0, cov, clip.x1 - clip.x0);
		}
		pdf_free(cov);
	}
	mask_unref(combined);
	shading_release(&s);
}

/* ---------------------------------------------------------------------- */
/* painting paths                                                         */

static void run_content(struct rctx *c, const uint8_t *data, size_t len, struct pdf_obj *resources);
static void paint_pattern(struct rctx *c, struct pdf_obj *pat, struct mask *region, bool stroke,
			  struct pdf_obj *resources);

static void fill_rast(struct rctx *c, struct rast_path *rp, bool evenodd, bool stroke,
		      struct pdf_obj *resources)
{
	struct gstate *g = cur(c);
	struct pdf_obj *pat = stroke ? g->stroke_pat : g->fill_pat;
	struct pdf_colorspace *cs = stroke ? g->stroke_cs : g->fill_cs;

	if (rast_path_empty(rp)) {
		return;
	}
	if (cs != NULL && cs->type == PDF_CS_PATTERN && !c->ignore_color) {
		if (pat == NULL) {
			return;
		}
		struct mask *region = mask_new(c);

		if (region == NULL) {
			return;
		}
		struct paint mp = {.c = c, .dst_mask = region->data};

		rast_fill_scratch(rp, evenodd, &g->clip, span_mask_write, &mp, &c->scratch);
		paint_pattern(c, pat, region, stroke, resources);
		mask_unref(region);
		return;
	}
	if (cs != NULL && cs->type == PDF_CS_SEPARATION && cs->is_none) {
		return;
	}
	struct paint p;

	paint_init(&p, c, stroke ? g->stroke_rgb : g->fill_rgb, stroke ? g->stroke_alpha : g->fill_alpha, stroke);
	rast_fill_scratch(rp, evenodd, &g->clip, span_paint, &p, &c->scratch);
}

static void fill_path(struct rctx *c, bool evenodd, struct pdf_obj *resources)
{
	dpath_to_rast(&c->path, &c->rp);
	fill_rast(c, &c->rp, evenodd, false, resources);
}

/* Flatten the device path into polylines and stroke them. */
struct polyline {
	float *xy;
	int n, cap;
};

static void pl_add(struct polyline *pl, float x, float y)
{
	if (pl->n == pl->cap) {
		int ncap = pl->cap ? pl->cap * 2 : 64;
		float *nx = pdf_realloc(pl->xy, (size_t)ncap * 2 * sizeof(float));

		if (nx == NULL) {
			return;
		}
		pl->xy = nx;
		pl->cap = ncap;
	}
	if (pl->n > 0 && pl->xy[(pl->n - 1) * 2] == x && pl->xy[(pl->n - 1) * 2 + 1] == y) {
		return;
	}
	pl->xy[pl->n * 2] = x;
	pl->xy[pl->n * 2 + 1] = y;
	pl->n++;
}

static void stroke_polyline(struct rctx *c, struct rast_path *out, const struct polyline *pl, bool closed,
			    float width, const struct gstate *g, float scale)
{
	(void)c;
	if (pl->n == 0) {
		return;
	}
	if (g->ndash == 0 || scale <= 0) {
		rast_stroke_polyline(out, pl->xy, pl->n, closed, width, g->line_cap, g->line_join);
		return;
	}
	/* dashes: walk the polyline, emitting "on" pieces */
	float pattern[MAX_DASH];
	int np = g->ndash;
	float total = 0;

	for (int i = 0; i < np; i++) {
		pattern[i] = g->dash[i] * scale;
		if (pattern[i] < 0) {
			pattern[i] = 0;
		}
		total += pattern[i];
	}
	if (total <= 0.01f) {
		rast_stroke_polyline(out, pl->xy, pl->n, closed, width, g->line_cap, g->line_join);
		return;
	}
	int di = 0;
	float remain = pattern[0];
	bool on = true;
	float phase = g->dash_phase * scale;

	while (phase > 0) {
		if (phase >= remain) {
			phase -= remain;
			di = (di + 1) % np;
			remain = pattern[di];
			on = !on;
		} else {
			remain -= phase;
			phase = 0;
		}
	}
	struct polyline piece = {0};
	int nseg = closed ? pl->n : pl->n - 1;
	int guard = 0;

	for (int i = 0; i < nseg; i++) {
		int j = (i + 1) % pl->n;
		float x0 = pl->xy[i * 2], y0 = pl->xy[i * 2 + 1], x1 = pl->xy[j * 2], y1 = pl->xy[j * 2 + 1];
		float seglen = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
		float pos = 0;

		if (on) {
			pl_add(&piece, x0, y0);
		}
		while (seglen - pos > remain && guard++ < 100000) {
			pos += remain;
			float t = pos / seglen;
			float px = x0 + (x1 - x0) * t, py = y0 + (y1 - y0) * t;

			if (on) {
				pl_add(&piece, px, py);
				rast_stroke_polyline(out, piece.xy, piece.n, false, width, g->line_cap, g->line_join);
				piece.n = 0;
			} else {
				pl_add(&piece, px, py);
				piece.n = 1;
				piece.xy[0] = px;
				piece.xy[1] = py;
			}
			on = !on;
			di = (di + 1) % np;
			remain = pattern[di];
			if (remain <= 0.01f) {
				remain = 0.01f;
			}
		}
		remain -= (seglen - pos);
		if (on) {
			pl_add(&piece, x1, y1);
		}
	}
	if (on && piece.n > 1) {
		rast_stroke_polyline(out, piece.xy, piece.n, false, width, g->line_cap, g->line_join);
	}
	pdf_free(piece.xy);
}

static void flatten_curve(struct polyline *pl, float x0, float y0, float x1, float y1, float x2, float y2,
			  float x3, float y3)
{
	float d = fabsf(x1 - x0) + fabsf(y1 - y0) + fabsf(x2 - x1) + fabsf(y2 - y1) + fabsf(x3 - x2) +
		  fabsf(y3 - y2);
	int n = (int)ceilf(sqrtf(d * 2.0f));

	if (n < 1) {
		n = 1;
	}
	if (n > 64) {
		n = 64;
	}
	for (int i = 1; i <= n; i++) {
		float t = (float)i / n, mt = 1.0f - t;
		float a = mt * mt * mt, b = 3 * mt * mt * t, cc = 3 * mt * t * t, dd = t * t * t;

		pl_add(pl, a * x0 + b * x1 + cc * x2 + dd * x3, a * y0 + b * y1 + cc * y2 + dd * y3);
	}
}

static void stroke_path(struct rctx *c, struct pdf_obj *resources)
{
	struct gstate *g = cur(c);
	float scale = mat_scale(g->ctm);
	float width = g->line_width * scale;

	if (width < MIN_LINE_WIDTH) {
		width = MIN_LINE_WIDTH;
	}
	rast_path_reset(&c->rp);
	struct polyline pl = {0};
	size_t pi = 0;
	bool closed = false;

	for (size_t i = 0; i < c->path.nops; i++) {
		uint8_t op = c->path.ops[i];

		if (op == 0) {
			if (pl.n > 0) {
				stroke_polyline(c, &c->rp, &pl, closed, width, g, scale);
			}
			pl.n = 0;
			closed = false;
			pl_add(&pl, c->path.pts[pi * 2], c->path.pts[pi * 2 + 1]);
			pi++;
		} else if (op == 1) {
			pl_add(&pl, c->path.pts[pi * 2], c->path.pts[pi * 2 + 1]);
			pi++;
		} else if (op == 2) {
			float x0 = pl.n ? pl.xy[(pl.n - 1) * 2] : c->path.pts[pi * 2];
			float y0 = pl.n ? pl.xy[(pl.n - 1) * 2 + 1] : c->path.pts[pi * 2 + 1];

			flatten_curve(&pl, x0, y0, c->path.pts[pi * 2], c->path.pts[pi * 2 + 1],
				      c->path.pts[pi * 2 + 2], c->path.pts[pi * 2 + 3], c->path.pts[pi * 2 + 4],
				      c->path.pts[pi * 2 + 5]);
			pi += 3;
		} else if (op == 3) {
			closed = true;
			if (pl.n > 0) {
				stroke_polyline(c, &c->rp, &pl, true, width, g, scale);
				/* a following segment without moveto starts at the subpath start */
				float sx = pl.xy[0], sy = pl.xy[1];

				pl.n = 0;
				pl_add(&pl, sx, sy);
				closed = false;
			}
		}
	}
	if (pl.n > 1) {
		stroke_polyline(c, &c->rp, &pl, closed, width, g, scale);
	} else if (pl.n == 1 && g->line_cap == 1) {
		rast_stroke_polyline(&c->rp, pl.xy, 1, false, width, 1, g->line_join);
	}
	pdf_free(pl.xy);
	fill_rast(c, &c->rp, false, true, resources);
}

static void end_path(struct rctx *c)
{
	apply_pending_clip(c);
	dpath_reset(&c->path);
}

/* ---------------------------------------------------------------------- */
/* colour operators                                                       */

static void set_cs(struct rctx *c, struct pdf_colorspace *cs, bool stroke)
{
	struct gstate *g = cur(c);

	if (cs == NULL) {
		return;
	}
	cs->refs = 1;
	if (stroke) {
		cs_unref(g->stroke_cs);
		g->stroke_cs = cs;
		g->stroke_pat = NULL;
	} else {
		cs_unref(g->fill_cs);
		g->fill_cs = cs;
		g->fill_pat = NULL;
	}
	float comps[32] = {0};

	if (cs->type != PDF_CS_PATTERN) {
		pdf_colorspace_initial(cs, comps);
		pdf_colorspace_to_rgb(cs, comps, stroke ? g->stroke_rgb : g->fill_rgb);
	} else {
		float *rgb = stroke ? g->stroke_rgb : g->fill_rgb;

		rgb[0] = rgb[1] = rgb[2] = 0;
	}
}

static void set_color(struct rctx *c, struct pdf_obj **ops, int nops, bool stroke, struct pdf_obj *resources)
{
	struct gstate *g = cur(c);
	struct pdf_colorspace *cs = stroke ? g->stroke_cs : g->fill_cs;
	float *rgb = stroke ? g->stroke_rgb : g->fill_rgb;

	if (c->ignore_color) {
		return;
	}
	if (cs == NULL) {
		return;
	}
	if (cs->type == PDF_CS_PATTERN) {
		if (nops >= 1 && ops[nops - 1]->type == PDF_NAME) {
			struct pdf_obj *pats = pdf_get(c->doc, resources, "Pattern");
			struct pdf_obj *pat = pdf_get(c->doc, pats, pdf_name_str(ops[nops - 1]));

			if (stroke) {
				g->stroke_pat = pat;
			} else {
				g->fill_pat = pat;
			}
			/* uncoloured patterns take the components before the name */
			if (cs->base != NULL && nops - 1 >= cs->base->ncomps) {
				float comps[32];

				for (int i = 0; i < cs->base->ncomps && i < 32; i++) {
					comps[i] = pdf_num(ops[i]);
				}
				pdf_colorspace_to_rgb(cs->base, comps, rgb);
			}
		}
		return;
	}
	float comps[32];
	int n = 0;

	for (int i = 0; i < nops && n < 32; i++) {
		if (pdf_is_num(ops[i])) {
			comps[n++] = pdf_num(ops[i]);
		}
	}
	if (n < cs->ncomps) {
		/* fewer operands than components: device space by count */
		if (n == 1) {
			rgb[0] = rgb[1] = rgb[2] = comps[0];
		} else if (n == 3) {
			memcpy(rgb, comps, sizeof(float) * 3);
		}
		return;
	}
	pdf_colorspace_to_rgb(cs, comps, rgb);
}

static void set_device_color(struct rctx *c, enum pdf_cs_type type, struct pdf_obj **ops, int nops, bool stroke)
{
	if (c->ignore_color) {
		return;
	}
	struct pdf_colorspace *cs = pdf_colorspace_device(type);

	if (cs == NULL) {
		return;
	}
	set_cs(c, cs, stroke);
	set_color(c, ops, nops, stroke, NULL);
}

/* ---------------------------------------------------------------------- */
/* ExtGState and soft masks                                               */

static int blend_from_name(const char *n)
{
	if (strcmp(n, "Multiply") == 0) {
		return 1;
	}
	if (strcmp(n, "Screen") == 0) {
		return 2;
	}
	if (strcmp(n, "Darken") == 0) {
		return 3;
	}
	if (strcmp(n, "Lighten") == 0) {
		return 4;
	}
	if (strcmp(n, "Difference") == 0) {
		return 5;
	}
	if (strcmp(n, "Exclusion") == 0) {
		return 6;
	}
	return 0;
}

static void do_form(struct rctx *c, struct pdf_obj *xo, struct pdf_obj *parent_res);

static void set_soft_mask(struct rctx *c, struct pdf_obj *sm)
{
	struct gstate *g = cur(c);

	mask_unref(g->smask);
	g->smask = NULL;
	if (sm == NULL || sm->type != PDF_DICT) {
		return;
	}
	struct pdf_obj *form = pdf_get(c->doc, sm, "G");

	if (form == NULL || form->type != PDF_STREAM || c->depth >= MAX_FORM_DEPTH - 2) {
		return;
	}
	bool luminosity = pdf_is_name(pdf_get(c->doc, sm, "S"), "Luminosity");
	size_t npix = (size_t)c->w * (size_t)c->h;
	uint8_t *tmp = pdf_malloc(npix * 3);

	if (tmp == NULL) {
		return;
	}
	/* backdrop: black for luminosity (or BC), transparent (black) for alpha */
	uint8_t bc[3] = {0, 0, 0};

	if (luminosity) {
		struct pdf_obj *bco = pdf_get(c->doc, sm, "BC");
		struct pdf_obj *grp = pdf_get(c->doc, form, "Group");
		struct pdf_colorspace *gcs = grp ? pdf_colorspace_load(c->doc, pdf_get(c->doc, grp, "CS"), NULL) : NULL;

		if (bco != NULL && gcs != NULL && (int)pdf_array_len(bco) >= gcs->ncomps) {
			float comps[32], col[3];

			for (int i = 0; i < gcs->ncomps && i < 32; i++) {
				comps[i] = pdf_num(pdf_array_get(c->doc, bco, (size_t)i));
			}
			pdf_colorspace_to_rgb(gcs, comps, col);
			bc[0] = (uint8_t)(col[0] * 255 + 0.5f);
			bc[1] = (uint8_t)(col[1] * 255 + 0.5f);
			bc[2] = (uint8_t)(col[2] * 255 + 0.5f);
		}
		pdf_colorspace_free(gcs);
	}
	for (size_t i = 0; i < npix; i++) {
		tmp[i * 3] = bc[0];
		tmp[i * 3 + 1] = bc[1];
		tmp[i * 3 + 2] = bc[2];
	}
	/* render the group form into tmp with a fresh state (same CTM) */
	uint8_t *saved_rgb = c->rgb;
	struct dpath saved_path = c->path;
	int saved_pending = c->pending_clip;
	float saved_tm[6], saved_tlm[6];

	memcpy(saved_tm, c->tm, sizeof(saved_tm));
	memcpy(saved_tlm, c->tlm, sizeof(saved_tlm));
	memset(&c->path, 0, sizeof(c->path));
	c->rgb = tmp;
	gs_push(c);
	struct gstate *ng = cur(c);

	mask_unref(ng->smask);
	ng->smask = NULL;
	mask_unref(ng->clip_mask);
	ng->clip_mask = NULL;
	ng->clip.x0 = 0;
	ng->clip.y0 = 0;
	ng->clip.x1 = c->w;
	ng->clip.y1 = c->h;
	ng->fill_alpha = ng->stroke_alpha = 1.0f;
	ng->blend = 0;
	struct mask *saved_group = c->group_mask;
	float saved_group_alpha = c->group_alpha;

	c->group_mask = NULL;
	c->group_alpha = 1.0f;
	/* within the luminosity group the BBox area outside the form keeps the backdrop */
	do_form(c, form, NULL);
	c->group_mask = saved_group;
	c->group_alpha = saved_group_alpha;
	gs_pop(c);
	c->rgb = saved_rgb;
	dpath_free(&c->path);
	c->path = saved_path;
	c->pending_clip = saved_pending;
	memcpy(c->tm, saved_tm, sizeof(saved_tm));
	memcpy(c->tlm, saved_tlm, sizeof(saved_tlm));

	struct mask *m = mask_new(c);

	if (m != NULL) {
		struct pdf_obj *tr = pdf_get(c->doc, sm, "TR");
		struct pdf_function *trf = (tr != NULL && tr->type != PDF_NAME) ? pdf_function_load(c->doc, tr) : NULL;
		uint8_t trlut[256];

		for (int i = 0; i < 256; i++) {
			trlut[i] = (uint8_t)i;
			if (trf != NULL) {
				float in = i / 255.0f, out[PDF_FUNC_MAX_OUT];
				int nout = 0;

				if (pdf_function_eval(trf, &in, 1, out, &nout) == 0 && nout >= 1) {
					int v = (int)(out[0] * 255 + 0.5f);

					trlut[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
				}
			}
		}
		pdf_function_free(trf);
		for (size_t i = 0; i < npix; i++) {
			int lum = (tmp[i * 3] * 77 + tmp[i * 3 + 1] * 151 + tmp[i * 3 + 2] * 28) >> 8;

			m->data[i] = trlut[lum > 255 ? 255 : lum];
		}
		g = cur(c);
		g->smask = m;
	}
	pdf_free(tmp);
}

static struct rfont *get_rfont(struct rctx *c, struct pdf_obj *fd);

static void apply_extgstate(struct rctx *c, struct pdf_obj *gsd)
{
	struct gstate *g = cur(c);
	struct pdf_doc *doc = c->doc;

	if (gsd == NULL || gsd->type != PDF_DICT) {
		return;
	}
	for (size_t i = 0; i < gsd->dict.n; i++) {
		const char *k = gsd->dict.items[i].key;
		struct pdf_obj *v = pdf_resolve(doc, gsd->dict.items[i].val);

		if (v == NULL) {
			continue;
		}
		if (strcmp(k, "LW") == 0) {
			g->line_width = pdf_num(v);
		} else if (strcmp(k, "LC") == 0) {
			g->line_cap = (int)pdf_int(v);
		} else if (strcmp(k, "LJ") == 0) {
			g->line_join = (int)pdf_int(v);
		} else if (strcmp(k, "CA") == 0 && pdf_is_num(v)) {
			g->stroke_alpha = pdf_num(v);
		} else if (strcmp(k, "ca") == 0 && pdf_is_num(v)) {
			g->fill_alpha = pdf_num(v);
		} else if (strcmp(k, "BM") == 0) {
			if (v->type == PDF_NAME) {
				g->blend = blend_from_name(pdf_name_str(v));
			} else if (v->type == PDF_ARRAY && v->arr.n > 0) {
				g->blend = blend_from_name(pdf_name_str(pdf_array_get(doc, v, 0)));
			}
		} else if (strcmp(k, "SMask") == 0) {
			if (v->type == PDF_NAME) {
				mask_unref(g->smask);
				g->smask = NULL;
			} else {
				set_soft_mask(c, v);
				g = cur(c);
			}
		} else if (strcmp(k, "D") == 0 && v->type == PDF_ARRAY && v->arr.n >= 1) {
			struct pdf_obj *arr = pdf_array_get(doc, v, 0);

			g->ndash = 0;
			for (size_t j = 0; j < pdf_array_len(arr) && j < MAX_DASH; j++) {
				g->dash[g->ndash++] = pdf_num(pdf_array_get(doc, arr, j));
			}
			g->dash_phase = v->arr.n > 1 ? pdf_num(pdf_array_get(doc, v, 1)) : 0;
		} else if (strcmp(k, "Font") == 0 && v->type == PDF_ARRAY && v->arr.n == 2) {
			struct pdf_obj *fd = pdf_array_get(doc, v, 0);

			if (fd != NULL && fd->type == PDF_DICT) {
				g->font = get_rfont(c, fd);
				g->font_size = pdf_num(pdf_array_get(doc, v, 1));
			}
		}
	}
}

/* ---------------------------------------------------------------------- */
/* fonts                                                                  */

static void gl_move(void *ud, float x, float y)
{
	rast_move_to(ud, x, y);
}

static void gl_line(void *ud, float x, float y)
{
	rast_line_to(ud, x, y);
}

static void gl_quad(void *ud, float cx, float cy, float x, float y)
{
	rast_quad_to(ud, cx, cy, x, y);
}

static void gl_cubic(void *ud, float x1, float y1, float x2, float y2, float x3, float y3)
{
	rast_curve_to(ud, x1, y1, x2, y2, x3, y3);
}

static void gl_close(void *ud)
{
	rast_close(ud);
}

static const struct ttf_outline_cb ttf_cb = {gl_move, gl_line, gl_quad, gl_close};
static const struct cff_outline_cb cff_cb = {gl_move, gl_line, gl_cubic, gl_close};

static void pick_builtin(struct rfont *rf, const char *base, int flags)
{
	const char *n = base;
	const char *plus = strchr(n, '+');

	if (plus != NULL && plus - n == 6) {
		n = plus + 1;
	}
	bool bold = strstr(n, "Bold") || strstr(n, "bold") || strstr(n, "Black") || strstr(n, "Heavy") ||
		    (flags & (1 << 18));
	bool italic = strstr(n, "Italic") || strstr(n, "Oblique") || (flags & (1 << 6));
	bool serif = strstr(n, "Times") || strstr(n, "Serif") || strstr(n, "Georgia") || strstr(n, "Book") ||
		     strstr(n, "Garamond") || strstr(n, "Roman") || strstr(n, "Minion") || strstr(n, "Cambria") ||
		     strstr(n, "Palatino") || strstr(n, "Century");
	bool fixed = strstr(n, "Courier") || strstr(n, "Mono") || strstr(n, "Consolas") || (flags & 1);
	bool symbol = strstr(n, "Symbol") || strstr(n, "Dingbat") || strstr(n, "Wingding");
	const unsigned char *data;
	unsigned int len;

	if (symbol) {
		data = zpdf_font_symbol;
		len = zpdf_font_symbol_len;
	} else if (fixed) {
		data = bold ? zpdf_font_mono_bold : zpdf_font_mono;
		len = bold ? zpdf_font_mono_bold_len : zpdf_font_mono_len;
	} else if (serif) {
		if (bold && italic) {
			data = zpdf_font_serif_bolditalic;
			len = zpdf_font_serif_bolditalic_len;
		} else if (bold) {
			data = zpdf_font_serif_bold;
			len = zpdf_font_serif_bold_len;
		} else if (italic) {
			data = zpdf_font_serif_italic;
			len = zpdf_font_serif_italic_len;
		} else {
			data = zpdf_font_serif;
			len = zpdf_font_serif_len;
		}
	} else {
		if (bold && italic) {
			data = zpdf_font_sans_bolditalic;
			len = zpdf_font_sans_bolditalic_len;
		} else if (bold) {
			data = zpdf_font_sans_bold;
			len = zpdf_font_sans_bold_len;
		} else if (italic) {
			data = zpdf_font_sans_italic;
			len = zpdf_font_sans_italic_len;
		} else {
			data = zpdf_font_sans;
			len = zpdf_font_sans_len;
		}
	}
	if (ttf_open(&rf->builtin_ttf, data, len) == 0) {
		rf->src = SRC_TTF;
		rf->builtin = true;
		rf->ttf = &rf->builtin_ttf;
		memcpy(rf->fm, identity, sizeof(rf->fm));
		rf->fm[0] = rf->fm[3] = 1.0f / rf->ttf->upem;
	}
}

static void load_names(struct rctx *c, struct rfont *rf, struct pdf_obj *dict)
{
	struct pdf_obj *enc = pdf_get(c->doc, dict, "Encoding");

	if (enc == NULL || enc->type != PDF_DICT) {
		return;
	}
	struct pdf_obj *diff = pdf_get(c->doc, enc, "Differences");

	if (diff == NULL || diff->type != PDF_ARRAY) {
		return;
	}
	int code = 0;

	for (size_t i = 0; i < diff->arr.n; i++) {
		struct pdf_obj *d = pdf_array_get(c->doc, diff, i);

		if (pdf_is_num(d)) {
			code = (int)pdf_int(d);
		} else if (d != NULL && d->type == PDF_NAME && code >= 0 && code < 256) {
			pdf_free(rf->names[code]);
			rf->names[code] = pdf_strndup(pdf_name_str(d), strlen(pdf_name_str(d)));
			rf->has_names = true;
			code++;
		}
	}
}

static struct rfont *get_rfont(struct rctx *c, struct pdf_obj *fd)
{
	for (int i = 0; i < c->nfonts; i++) {
		if (c->fonts[i]->dict == fd) {
			return c->fonts[i];
		}
	}
	struct rfont *rf = pdf_calloc(sizeof(*rf));

	if (rf == NULL) {
		return NULL;
	}
	for (int i = 0; i < 256; i++) {
		rf->gid_cache[i] = -1;
	}
	rf->dict = fd;
	rf->pf = pdf_doc_font_cache_get(c->doc, fd);
	if (rf->pf == NULL) {
		rf->pf = pdf_font_load(c->doc, fd);
		if (rf->pf != NULL) {
			pdf_doc_font_cache_put(c->doc, fd, rf->pf);
		}
	}
	if (rf->pf == NULL) {
		pdf_free(rf);
		return NULL;
	}
	struct pdf_doc *doc = c->doc;
	const char *subtype = pdf_name_str(pdf_get(doc, fd, "Subtype"));
	struct pdf_obj *desc_font = fd;

	if (rf->pf->type0) {
		struct pdf_obj *df = pdf_get(doc, fd, "DescendantFonts");

		desc_font = pdf_array_get(doc, df, 0);
		if (desc_font == NULL) {
			desc_font = fd;
		}
		struct pdf_obj *c2g = pdf_get(doc, desc_font, "CIDToGIDMap");

		if (c2g != NULL && c2g->type == PDF_STREAM) {
			if (pdf_stream_decode(doc, c2g, &rf->cid2gid, &rf->cid2gid_len) != 0) {
				rf->cid2gid = NULL;
			}
		}
	} else {
		load_names(c, rf, fd);
	}
	struct pdf_obj *descr = pdf_get(doc, desc_font, "FontDescriptor");
	int flags = (int)pdf_int(pdf_get(doc, descr, "Flags"));

	rf->symbolic = (flags & 4) && !(flags & 32);
	memcpy(rf->fm, identity, sizeof(rf->fm));
	rf->fm[0] = rf->fm[3] = 0.001f;

	if (strcmp(subtype, "Type3") == 0) {
		rf->src = SRC_TYPE3;
		rf->charprocs = pdf_get(doc, fd, "CharProcs");
		rf->t3res = pdf_get(doc, fd, "Resources");
		struct pdf_obj *fm = pdf_get(doc, fd, "FontMatrix");

		if (pdf_array_len(fm) >= 6) {
			for (int i = 0; i < 6; i++) {
				rf->fm[i] = pdf_num(pdf_array_get(doc, fm, (size_t)i));
			}
		}
		goto done;
	}
	struct pdf_obj *ff = pdf_get(doc, descr, "FontFile2");
	bool is_cff = false;

	if (ff == NULL) {
		ff = pdf_get(doc, descr, "FontFile3");
		is_cff = ff != NULL;
	}
	if (ff != NULL && ff->type == PDF_STREAM) {
		/* the decoded program is shared through the document cache */
		struct font_program *fp = pdf_doc_aux_get(doc, ff);

		if (fp == NULL) {
			fp = pdf_calloc(sizeof(*fp));
			if (fp != NULL && pdf_stream_decode(doc, ff, &fp->data, &fp->len) == 0 && fp->len > 4) {
				bool otto = memcmp(fp->data, "OTTO", 4) == 0;
				bool cff_magic = fp->data[0] == 1 && fp->data[1] == 0;

				if ((is_cff || cff_magic || otto) && (otto || cff_magic)) {
					fp->cff = cff_open(fp->data, fp->len);
					if (fp->cff != NULL) {
						fp->kind = SRC_CFF;
					}
				}
				if (fp->kind == SRC_NONE && ttf_open(&fp->ttf, fp->data, fp->len) == 0 && fp->ttf.has_glyf) {
					fp->kind = SRC_TTF;
				}
				if (fp->kind == SRC_NONE && !otto) {
					fp->cff = cff_open(fp->data, fp->len);
					if (fp->cff != NULL) {
						fp->kind = SRC_CFF;
					}
				}
			}
			if (fp != NULL && fp->kind == SRC_NONE) {
				font_program_free(fp);
				fp = NULL;
			}
			if (fp != NULL && pdf_doc_aux_put(doc, ff, fp, fp->len + sizeof(*fp), font_program_free) < 0) {
				font_program_free(fp);
				fp = NULL;
			}
		}
		if (fp != NULL) {
			rf->prog = fp;
			rf->prog_key = ff;
			rf->doc = doc;
			rf->src = fp->kind;
			if (fp->kind == SRC_CFF) {
				rf->cff = fp->cff;
				cff_font_matrix(rf->cff, rf->fm);
			} else {
				rf->ttf = &fp->ttf;
				rf->fm[0] = rf->fm[3] = 1.0f / rf->ttf->upem;
			}
		}
	}
	if (rf->src == SRC_NONE) {
		/* Type1 (FontFile) programs and missing files: built-in font */
		pick_builtin(rf, pdf_name_str(pdf_get(doc, fd, "BaseFont")), flags);
	}
done:
	if (c->nfonts == c->fonts_cap) {
		int ncap = c->fonts_cap ? c->fonts_cap * 2 : 16;
		struct rfont **nf = pdf_realloc(c->fonts, (size_t)ncap * sizeof(*nf));

		if (nf == NULL) {
			return rf; /* leaks on this rare path, but stays usable */
		}
		c->fonts = nf;
		c->fonts_cap = ncap;
	}
	c->fonts[c->nfonts++] = rf;
	return rf;
}

static void rfont_free(struct rfont *rf)
{
	if (rf == NULL) {
		return;
	}
	if (rf->prog != NULL) {
		pdf_doc_aux_unref(rf->doc, rf->prog_key);
	}
	pdf_free(rf->cid2gid);
	for (int i = 0; i < 256; i++) {
		pdf_free(rf->names[i]);
	}
	pdf_free(rf);
}

/* glyph name for a code of a simple font: /Differences, else from the base encoding */
static const char *glyph_name_for_unicode(uint32_t uni, char *buf, size_t n)
{
	/* reverse of the glyph list for the common cases */
	if (uni >= 'A' && uni <= 'Z') {
		buf[0] = (char)uni;
		buf[1] = 0;
		return buf;
	}
	if (uni >= 'a' && uni <= 'z') {
		buf[0] = (char)uni;
		buf[1] = 0;
		return buf;
	}
	static const char *const digits[] = {"zero", "one", "two", "three", "four", "five", "six",
					     "seven", "eight", "nine"};
	if (uni >= '0' && uni <= '9') {
		return digits[uni - '0'];
	}
	static const struct {
		uint16_t u;
		const char *n;
	} t[] = {{' ', "space"}, {'!', "exclam"}, {'"', "quotedbl"}, {'#', "numbersign"}, {'$', "dollar"},
		 {'%', "percent"}, {'&', "ampersand"}, {'\'', "quotesingle"}, {'(', "parenleft"},
		 {')', "parenright"}, {'*', "asterisk"}, {'+', "plus"}, {',', "comma"}, {'-', "hyphen"},
		 {'.', "period"}, {'/', "slash"}, {':', "colon"}, {';', "semicolon"}, {'<', "less"},
		 {'=', "equal"}, {'>', "greater"}, {'?', "question"}, {'@', "at"}, {'[', "bracketleft"},
		 {'\\', "backslash"}, {']', "bracketright"}, {'^', "asciicircum"}, {'_', "underscore"},
		 {'`', "grave"}, {'{', "braceleft"}, {'|', "bar"}, {'}', "braceright"}, {'~', "asciitilde"},
		 {0x2019, "quoteright"}, {0x2018, "quoteleft"}, {0x201C, "quotedblleft"},
		 {0x201D, "quotedblright"}, {0x2013, "endash"}, {0x2014, "emdash"}, {0x2022, "bullet"},
		 {0x2026, "ellipsis"}, {0xFB01, "fi"}, {0xFB02, "fl"}, {0xA9, "copyright"},
		 {0xAE, "registered"}, {0x2122, "trademark"}, {0xB0, "degree"}, {0xE9, "eacute"},
		 {0xE8, "egrave"}, {0xE0, "agrave"}, {0xE1, "aacute"}, {0xFC, "udieresis"}, {0xF6, "odieresis"},
		 {0xE4, "adieresis"}, {0xDF, "germandbls"}, {0xE7, "ccedilla"}, {0xF1, "ntilde"},
		 {0xA7, "section"}, {0xB7, "periodcentered"}, {0xAB, "guillemotleft"}, {0xBB, "guillemotright"},
		 {0x2212, "minus"}, {0xD7, "multiply"}, {0xB5, "mu"}, {0x2030, "perthousand"}};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (t[i].u == uni) {
			return t[i].n;
		}
	}
	snprintf(buf, n, "uni%04X", (unsigned)uni);
	return buf;
}

static uint16_t glyph_index(struct rfont *rf, uint32_t code, uint32_t cid)
{
	struct pdf_font *pf = rf->pf;

	if (!pf->type0 && code < 256 && rf->gid_cache[code] >= 0) {
		return (uint16_t)rf->gid_cache[code];
	}
	uint16_t gid = 0;

	if (pf->type0) {
		uint32_t g = cid;

		if (rf->cid2gid != NULL) {
			g = ((size_t)cid * 2 + 1 < rf->cid2gid_len)
				    ? ((uint32_t)rf->cid2gid[cid * 2] << 8) | rf->cid2gid[cid * 2 + 1]
				    : 0;
		}
		if (rf->builtin) {
			char utf8[16];
			int n = pdf_font_to_utf8(pf, code, cid, utf8);
			uint32_t uni = 0;

			if (n > 0) {
				pdf_utf8_get(utf8, (size_t)n, &uni);
			}
			gid = uni ? ttf_cmap_lookup(rf->ttf, 31, uni) : 0;
		} else if (rf->src == SRC_CFF) {
			gid = cff_is_cid(rf->cff) ? cff_gid_for_cid(rf->cff, cid) : (uint16_t)g;
		} else {
			gid = (uint16_t)g;
		}
		return gid;
	}
	/* simple fonts */
	const char *name = (code < 256) ? rf->names[code] : NULL;
	uint32_t uni = code < 256 ? pf->simple_enc[code] : 0;

	if (rf->builtin) {
		if (name != NULL) {
			uint32_t u2 = pdf_glyph_to_unicode(name);

			if (u2) {
				uni = u2;
			}
		}
		if (uni == 0) {
			uni = code;
		}
		gid = ttf_cmap_lookup(rf->ttf, 31, uni);
	} else if (rf->src == SRC_TTF) {
		const struct ttf_font *t = rf->ttf;
		bool any_cmap = ttf_has_cmap(t, 31) || ttf_has_cmap(t, 30) || ttf_has_cmap(t, 10);

		if (name != NULL && t->post_off) {
			gid = ttf_post_lookup(t, name);
		}
		if (gid == 0 && rf->symbolic && ttf_has_cmap(t, 30)) {
			gid = ttf_cmap_lookup(t, 30, 0xF000 | (code & 0xFF));
			if (gid == 0) {
				gid = ttf_cmap_lookup(t, 30, code);
			}
			if (gid == 0) {
				gid = ttf_cmap_lookup(t, 30, 0xF100 | (code & 0xFF));
			}
		}
		if (gid == 0 && ttf_has_cmap(t, 31)) {
			uint32_t u = uni;

			if (name != NULL) {
				uint32_t u2 = pdf_glyph_to_unicode(name);

				if (u2) {
					u = u2;
				}
			}
			if (u) {
				gid = ttf_cmap_lookup(t, 31, u);
			}
			if (gid == 0) {
				gid = ttf_cmap_lookup(t, 31, 0xF000 | (code & 0xFF));
			}
			if (gid == 0) {
				gid = ttf_cmap_lookup(t, 31, code);
			}
		}
		if (gid == 0 && ttf_has_cmap(t, 10)) {
			gid = ttf_cmap_lookup(t, 10, code);
		}
		if (gid == 0 && ttf_has_cmap(t, 30) && !rf->symbolic) {
			gid = ttf_cmap_lookup(t, 30, 0xF000 | (code & 0xFF));
		}
		if (gid == 0 && (!any_cmap || (name == NULL && !ttf_has_cmap(t, 31)))) {
			gid = (uint16_t)code;
		}
		if (gid == 0 && name != NULL && name[0] == 'g' && name[1] >= '0' && name[1] <= '9') {
			gid = (uint16_t)atoi(name + 1);
		}
		if (gid == 0 && name != NULL && strncmp(name, "glyph", 5) == 0) {
			gid = (uint16_t)atoi(name + 5);
		}
	} else if (rf->src == SRC_CFF) {
		char buf[32];

		if (name != NULL) {
			gid = cff_gid_for_name(rf->cff, name);
			if (gid == 0 && name[0] == 'g' && name[1] >= '0' && name[1] <= '9') {
				gid = (uint16_t)atoi(name + 1);
			}
		}
		if (gid == 0 && !rf->symbolic && uni) {
			gid = cff_gid_for_name(rf->cff, glyph_name_for_unicode(uni, buf, sizeof(buf)));
		}
		if (gid == 0) {
			gid = cff_gid_for_code(rf->cff, (uint8_t)code);
		}
		if (gid == 0 && uni) {
			gid = cff_gid_for_name(rf->cff, glyph_name_for_unicode(uni, buf, sizeof(buf)));
		}
		if (gid == 0 && cff_is_cid(rf->cff)) {
			gid = cff_gid_for_cid(rf->cff, code);
		}
	}
	if (code < 256) {
		rf->gid_cache[code] = gid;
	}
	return gid;
}

static void draw_type3_glyph(struct rctx *c, struct rfont *rf, uint32_t code, const float trm[6],
			     struct pdf_obj *resources)
{
	const char *name = code < 256 ? rf->names[code] : NULL;

	if (name == NULL || rf->charprocs == NULL || c->t3_depth >= 4) {
		return;
	}
	struct pdf_obj *proc = pdf_get(c->doc, rf->charprocs, name);

	if (proc == NULL || proc->type != PDF_STREAM) {
		return;
	}
	uint8_t *data;
	size_t len;

	if (pdf_stream_decode(c->doc, proc, &data, &len) < 0) {
		return;
	}
	struct dpath saved_path = c->path;
	float saved_tm[6], saved_tlm[6];
	int saved_pending = c->pending_clip;

	memcpy(saved_tm, c->tm, sizeof(saved_tm));
	memcpy(saved_tlm, c->tlm, sizeof(saved_tlm));
	memset(&c->path, 0, sizeof(c->path));
	gs_push(c);
	mat_mul(rf->fm, trm, cur(c)->ctm);
	/* glyphs described with d1 use the text fill colour; d0 glyphs set their own */
	c->t3_depth++;
	run_content(c, data, len, rf->t3res ? rf->t3res : resources);
	c->t3_depth--;
	gs_pop(c);
	dpath_free(&c->path);
	c->path = saved_path;
	c->pending_clip = saved_pending;
	memcpy(c->tm, saved_tm, sizeof(saved_tm));
	memcpy(c->tlm, saved_tlm, sizeof(saved_tlm));
	pdf_free(data);
}

struct mask_capture {
	uint8_t *cov;
	int x0, y0, w, h;
};

static void span_capture(void *ud, int y, int x0, const uint8_t *cov, int n)
{
	struct mask_capture *m = ud;
	int row = y - m->y0;

	if (row < 0 || row >= m->h) {
		return;
	}
	for (int i = 0; i < n; i++) {
		int x = x0 + i - m->x0;

		if (x >= 0 && x < m->w) {
			m->cov[(size_t)row * m->w + x] = cov[i];
		}
	}
}

static uint32_t glyph_hash(const void *font, uint32_t gid, int32_t ma, int32_t mb, int32_t mc, int32_t md,
			   uint8_t subx)
{
	uint32_t h = (uint32_t)(uintptr_t)font * 2654435761u;

	h ^= gid * 2246822519u;
	h ^= (uint32_t)ma * 3266489917u;
	h ^= (uint32_t)mb * 668265263u;
	h ^= (uint32_t)mc * 374761393u;
	h ^= (uint32_t)md * 2654435761u;
	h ^= subx * 0x9E3779B1u;
	return h % GLYPH_CACHE_SLOTS;
}

/*
 * Draw one glyph through the cache. The outline is rasterized with an
 * integer origin and the fractional x offset quantized to quarters, so
 * the same letter at the same size reuses its coverage bitmap.
 * Returns false when the glyph must take the slow path.
 */
static bool draw_glyph_cached(struct rctx *c, struct rfont *rf, uint16_t gid, const float m[6])
{
	struct gstate *g = cur(c);
	struct pdf_colorspace *cs = g->fill_cs;

	if ((cs != NULL && cs->type == PDF_CS_PATTERN) || (cs != NULL && cs->type == PDF_CS_SEPARATION && cs->is_none)) {
		return false;
	}
	float units = rf->src == SRC_TTF ? (float)rf->ttf->upem : 1000.0f;
	float size_dev = sqrtf(m[1] * m[1] + m[3] * m[3]) * units;

	if (size_dev > GLYPH_CACHE_MAX_PX || size_dev < 0.5f) {
		return false;
	}
	if (c->glyphs == NULL) {
		c->glyphs = pdf_calloc(sizeof(*c->glyphs) * GLYPH_CACHE_SLOTS);
		if (c->glyphs == NULL) {
			return false;
		}
	}
	float ox = floorf(m[4]), oy = floorf(m[5]);
	uint8_t subx = (uint8_t)((m[4] - ox) * 4.0f);
	int32_t ma = (int32_t)(m[0] * 256.0f), mb = (int32_t)(m[1] * 256.0f);
	int32_t mc = (int32_t)(m[2] * 256.0f), md = (int32_t)(m[3] * 256.0f);
	const void *key = rf->src == SRC_TTF ? (const void *)rf->ttf : (const void *)rf->cff;
	uint32_t slot = glyph_hash(key, gid, ma, mb, mc, md, subx);
	struct glyph_entry *e = &c->glyphs[slot];

	if (!(e->used && e->font == key && e->gid == gid && e->ma == ma && e->mb == mb && e->mc == mc &&
	      e->md == md && e->subx == subx)) {
		float lm[6] = {m[0], m[1], m[2], m[3], (float)subx / 4.0f, 0.0f};

		rast_path_reset(&c->rp);
		if (rf->src == SRC_TTF) {
			ttf_glyph_outline(rf->ttf, gid, lm, &ttf_cb, &c->rp);
		} else {
			cff_glyph_outline(rf->cff, gid, lm, &cff_cb, &c->rp);
		}
		rast_close(&c->rp);
		if (e->used) {
			c->glyph_bytes -= (size_t)e->w * e->h;
			pdf_free(e->cov);
			e->cov = NULL;
		}
		e->used = true;
		e->font = key;
		e->gid = gid;
		e->ma = ma;
		e->mb = mb;
		e->mc = mc;
		e->md = md;
		e->subx = subx;
		e->empty = rast_path_empty(&c->rp);
		e->cov = NULL;
		e->w = e->h = 0;
		c->glyph_misses++;
		if (!e->empty) {
			int x0 = (int)floorf(c->rp.minx), y0 = (int)floorf(c->rp.miny);
			int x1 = (int)ceilf(c->rp.maxx) + 1, y1 = (int)ceilf(c->rp.maxy) + 1;
			int w = x1 - x0, h = y1 - y0;

			if (w <= 0 || h <= 0 || w > 2 * GLYPH_CACHE_MAX_PX || h > 2 * GLYPH_CACHE_MAX_PX ||
			    c->glyph_bytes + (size_t)w * h > CONFIG_ZPDF_GLYPH_CACHE_BYTES) {
				e->used = false;
				return false;
			}
			e->cov = pdf_calloc((size_t)w * h);
			if (e->cov == NULL) {
				e->used = false;
				return false;
			}
			e->x0 = (int16_t)x0;
			e->y0 = (int16_t)y0;
			e->w = (uint16_t)w;
			e->h = (uint16_t)h;
			c->glyph_bytes += (size_t)w * h;
			struct mask_capture cap = {.cov = e->cov, .x0 = x0, .y0 = y0, .w = w, .h = h};
			struct rast_clip all = {x0, y0, x1, y1};

			rast_fill_scratch(&c->rp, false, &all, span_capture, &cap, &c->scratch);
		}
	} else {
		c->glyph_hits++;
	}
	if (e->empty) {
		return true;
	}
	struct paint p;

	paint_init(&p, c, g->fill_rgb, g->fill_alpha, false);
	int bx = (int)ox + e->x0, by = (int)oy + e->y0;
	int cx0 = bx < g->clip.x0 ? g->clip.x0 : bx;
	int cx1 = bx + e->w > g->clip.x1 ? g->clip.x1 : bx + e->w;

	if (cx1 <= cx0) {
		return true;
	}
	for (int row = 0; row < e->h; row++) {
		int y = by + row;

		if (y < g->clip.y0 || y >= g->clip.y1) {
			continue;
		}
		span_paint(&p, y, cx0, e->cov + (size_t)row * e->w + (cx0 - bx), cx1 - cx0);
	}
	return true;
}

static void show_text(struct rctx *c, const uint8_t *s, size_t n, struct pdf_obj *resources)
{
	struct gstate *g = cur(c);
	struct rfont *rf = g->font;

	if (rf == NULL || rf->pf == NULL) {
		return;
	}
	struct pdf_font *pf = rf->pf;
	size_t i = 0;
	int mode = g->tmode;

	while (i < n) {
		uint32_t code, cid;
		int nb = pdf_font_next_code(pf, s + i, n - i, &code, &cid);

		if (nb <= 0) {
			nb = 1;
		}
		float w0 = pdf_font_width(pf, code, cid);

		if (rf->src == SRC_TYPE3) {
			/* widths of type 3 fonts are in glyph space: pdf_font scaled them already */
		} else if (rf->builtin && pf->widths == NULL && pf->nwranges == 0) {
			uint16_t gid = glyph_index(rf, code, cid);

			w0 = ttf_advance(rf->ttf, gid) / (float)rf->ttf->upem;
		}
		bool is_space = (nb == 1 && code == 32);
		float tx = (w0 * g->font_size + g->tc + (is_space ? g->tw : 0.0f)) * g->th;
		float ty = 0.0f;

		if (pf->vertical) {
			ty = -(w0 * g->font_size + g->tc + (is_space ? g->tw : 0.0f));
			tx = 0.0f;
		}
		if (mode != 3 && mode != 7) {
			float ts[6] = {g->font_size * g->th, 0, 0, g->font_size, 0, g->trise};
			float trm[6], m[6];

			mat_mul(ts, c->tm, trm);
			mat_mul(trm, g->ctm, trm);
			if (rf->src == SRC_TYPE3) {
				draw_type3_glyph(c, rf, code, trm, resources);
			} else if (rf->src != SRC_NONE) {
				uint16_t gid = glyph_index(rf, code, cid);

				if (gid != 0 || (rf->src == SRC_CFF)) {
					mat_mul(rf->fm, trm, m);
					if (mode == 0 && draw_glyph_cached(c, rf, gid, m)) {
						goto advance;
					}
					rast_path_reset(&c->rp);
					if (rf->src == SRC_TTF) {
						ttf_glyph_outline(rf->ttf, gid, m, &ttf_cb, &c->rp);
					} else {
						cff_glyph_outline(rf->cff, gid, m, &cff_cb, &c->rp);
					}
					rast_close(&c->rp);
					if (!rast_path_empty(&c->rp)) {
						bool do_fill = (mode == 0 || mode == 2 || mode == 4 || mode == 6);
						bool do_stroke = (mode == 1 || mode == 2 || mode == 5 || mode == 6);

						if (do_fill) {
							fill_rast(c, &c->rp, false, false, resources);
						}
						if (do_stroke) {
							/* approximate: thin outline in the stroke colour */
							struct gstate *gg = cur(c);
							float width = gg->line_width * mat_scale(gg->ctm);

							if (width < MIN_LINE_WIDTH) {
								width = MIN_LINE_WIDTH;
							}
							/* reuse the filled shape when the stroke is thin */
							if (!do_fill) {
								fill_rast(c, &c->rp, false, true, resources);
							}
						}
					}
				}
			}
		}
advance:;
		float adv[6] = {1, 0, 0, 1, tx, ty};

		mat_mul(adv, c->tm, c->tm);
		i += (size_t)nb;
		g = cur(c);
	}
}

static void adjust_tj(struct rctx *c, float amount)
{
	struct gstate *g = cur(c);
	float tx = -amount / 1000.0f * g->font_size * g->th;
	float adv[6] = {1, 0, 0, 1, tx, 0};

	if (g->font != NULL && g->font->pf != NULL && g->font->pf->vertical) {
		adv[4] = 0;
		adv[5] = -amount / 1000.0f * g->font_size;
	}
	mat_mul(adv, c->tm, c->tm);
}

static void next_line(struct rctx *c, float tx, float ty)
{
	float t[6] = {1, 0, 0, 1, tx, ty};

	mat_mul(t, c->tlm, c->tlm);
	memcpy(c->tm, c->tlm, sizeof(c->tm));
}

/* ---------------------------------------------------------------------- */
/* images                                                                 */

struct image_source {
	const struct pdf_image *img;
	float inv[6]; /* device -> unit square */
	int ssx, ssy; /* supersampling when minifying */
	uint8_t fill[3];
	bool bilinear;
};

static inline void sample_image(const struct pdf_image *img, float u, float v, uint8_t rgb[3], int *a,
				bool bilinear)
{
	float fx = u * img->w, fy = (1.0f - v) * img->h;
	int sx = (int)fx, sy = (int)fy;

	if (sx < 0) {
		sx = 0;
	}
	if (sy < 0) {
		sy = 0;
	}
	if (sx >= img->w) {
		sx = img->w - 1;
	}
	if (sy >= img->h) {
		sy = img->h - 1;
	}
	size_t idx = (size_t)sy * (size_t)img->w + (size_t)sx;

	if (bilinear && img->rgb != NULL && img->w > 1 && img->h > 1) {
		float gx = fx - 0.5f, gy = fy - 0.5f;
		int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
		float tx = gx - x0, ty = gy - y0;
		int x1 = x0 + 1, y1 = y0 + 1;

		x0 = x0 < 0 ? 0 : (x0 >= img->w ? img->w - 1 : x0);
		x1 = x1 < 0 ? 0 : (x1 >= img->w ? img->w - 1 : x1);
		y0 = y0 < 0 ? 0 : (y0 >= img->h ? img->h - 1 : y0);
		y1 = y1 < 0 ? 0 : (y1 >= img->h ? img->h - 1 : y1);
		const uint8_t *p00 = img->rgb + ((size_t)y0 * img->w + x0) * 3;
		const uint8_t *p10 = img->rgb + ((size_t)y0 * img->w + x1) * 3;
		const uint8_t *p01 = img->rgb + ((size_t)y1 * img->w + x0) * 3;
		const uint8_t *p11 = img->rgb + ((size_t)y1 * img->w + x1) * 3;

		for (int k = 0; k < 3; k++) {
			float top = p00[k] + (p10[k] - p00[k]) * tx;
			float bot = p01[k] + (p11[k] - p01[k]) * tx;

			rgb[k] = (uint8_t)(top + (bot - top) * ty + 0.5f);
		}
		if (img->alpha != NULL) {
			float a00 = img->alpha[(size_t)y0 * img->w + x0], a10 = img->alpha[(size_t)y0 * img->w + x1];
			float a01 = img->alpha[(size_t)y1 * img->w + x0], a11 = img->alpha[(size_t)y1 * img->w + x1];
			float top = a00 + (a10 - a00) * tx, bot = a01 + (a11 - a01) * tx;

			*a = (int)(top + (bot - top) * ty + 0.5f);
		} else {
			*a = 255;
		}
		return;
	}
	if (img->rgb != NULL) {
		memcpy(rgb, img->rgb + idx * 3, 3);
	}
	*a = img->alpha ? img->alpha[idx] : 255;
}

static bool image_source(void *sud, int x, int y, uint8_t rgb[3], int *a)
{
	struct image_source *s = sud;
	const struct pdf_image *img = s->img;
	int acc[4] = {0, 0, 0, 0};
	int count = 0;

	for (int j = 0; j < s->ssy; j++) {
		for (int i = 0; i < s->ssx; i++) {
			float px = (float)x + ((float)i + 0.5f) / s->ssx;
			float py = (float)y + ((float)j + 0.5f) / s->ssy;
			float u, v;

			mat_apply(s->inv, px, py, &u, &v);
			if (u < 0 || u >= 1 || v < 0 || v >= 1) {
				continue;
			}
			uint8_t c[3] = {0, 0, 0};
			int aa;

			sample_image(img, u, v, c, &aa, s->bilinear);
			if (img->is_mask) {
				acc[0] += s->fill[0] * aa;
				acc[1] += s->fill[1] * aa;
				acc[2] += s->fill[2] * aa;
			} else {
				acc[0] += c[0] * aa;
				acc[1] += c[1] * aa;
				acc[2] += c[2] * aa;
			}
			acc[3] += aa;
			count++;
		}
	}
	int total = s->ssx * s->ssy;

	if (count == 0 || acc[3] == 0) {
		return false;
	}
	rgb[0] = (uint8_t)(acc[0] / acc[3]);
	rgb[1] = (uint8_t)(acc[1] / acc[3]);
	rgb[2] = (uint8_t)(acc[2] / acc[3]);
	/* coverage: alpha averaged over all samples (edge samples outside count as 0) */
	*a = acc[3] / total;
	return true;
}

static void draw_image(struct rctx *c, const struct pdf_image *img)
{
	struct gstate *g = cur(c);
	const float *m = g->ctm;
	struct image_source s;

	if (!mat_invert(m, s.inv)) {
		return;
	}
	s.img = img;
	/* device size of the image */
	float dw = sqrtf(m[0] * m[0] + m[1] * m[1]), dh = sqrtf(m[2] * m[2] + m[3] * m[3]);

	s.ssx = (int)ceilf((float)img->w / (dw > 1 ? dw : 1));
	s.ssy = (int)ceilf((float)img->h / (dh > 1 ? dh : 1));
	if (s.ssx < 1) {
		s.ssx = 1;
	}
	if (s.ssy < 1) {
		s.ssy = 1;
	}
	if (s.ssx > 6) {
		s.ssx = 6;
	}
	if (s.ssy > 6) {
		s.ssy = 6;
	}
	/* edges of the unit square still need antialiasing: supersample at least 2x2 */
	if (s.ssx < 2) {
		s.ssx = 2;
	}
	if (s.ssy < 2) {
		s.ssy = 2;
	}
	s.bilinear = !img->is_mask && (dw > img->w * 1.2f || dh > img->h * 1.2f);
	s.fill[0] = (uint8_t)(g->fill_rgb[0] * 255 + 0.5f);
	s.fill[1] = (uint8_t)(g->fill_rgb[1] * 255 + 0.5f);
	s.fill[2] = (uint8_t)(g->fill_rgb[2] * 255 + 0.5f);

	/* bounding box of the transformed unit square */
	float xs[4], ys[4];

	mat_apply(m, 0, 0, &xs[0], &ys[0]);
	mat_apply(m, 1, 0, &xs[1], &ys[1]);
	mat_apply(m, 1, 1, &xs[2], &ys[2]);
	mat_apply(m, 0, 1, &xs[3], &ys[3]);
	float mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];

	for (int i = 1; i < 4; i++) {
		mnx = xs[i] < mnx ? xs[i] : mnx;
		mxx = xs[i] > mxx ? xs[i] : mxx;
		mny = ys[i] < mny ? ys[i] : mny;
		mxy = ys[i] > mxy ? ys[i] : mxy;
	}
	struct rast_clip clip = g->clip;

	if ((int)floorf(mnx) > clip.x0) {
		clip.x0 = (int)floorf(mnx);
	}
	if ((int)ceilf(mxx) < clip.x1) {
		clip.x1 = (int)ceilf(mxx);
	}
	if ((int)floorf(mny) > clip.y0) {
		clip.y0 = (int)floorf(mny);
	}
	if ((int)ceilf(mxy) < clip.y1) {
		clip.y1 = (int)ceilf(mxy);
	}
	if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0) {
		return;
	}
	if (img->is_mask && g->fill_cs != NULL && g->fill_cs->type == PDF_CS_PATTERN && g->fill_pat != NULL) {
		/* stencil through a pattern: use the mask as the region */
		struct mask *region = mask_new(c);

		if (region != NULL) {
			struct paint mp = {.c = c, .dst_mask = region->data, .alpha = 255, .source = image_source, .sud = &s};
			uint8_t *cov = pdf_malloc((size_t)(clip.x1 - clip.x0));

			if (cov != NULL) {
				memset(cov, 255, (size_t)(clip.x1 - clip.x0));
				for (int y = clip.y0; y < clip.y1; y++) {
					span_paint(&mp, y, clip.x0, cov, clip.x1 - clip.x0);
				}
				pdf_free(cov);
			}
			paint_pattern(c, g->fill_pat, region, false, NULL);
			mask_unref(region);
		}
		return;
	}
	struct paint p;
	float white[3] = {1, 1, 1};

	paint_init(&p, c, white, g->fill_alpha, false);
	p.source = image_source;
	p.sud = &s;
	uint8_t *cov = pdf_malloc((size_t)(clip.x1 - clip.x0));

	if (cov == NULL) {
		return;
	}
	memset(cov, 255, (size_t)(clip.x1 - clip.x0));
	for (int y = clip.y0; y < clip.y1; y++) {
		span_paint(&p, y, clip.x0, cov, clip.x1 - clip.x0);
	}
	pdf_free(cov);
}

static void image_device_size(struct rctx *c, int *dw, int *dh)
{
	const float *m = cur(c)->ctm;

	*dw = (int)ceilf(sqrtf(m[0] * m[0] + m[1] * m[1]));
	*dh = (int)ceilf(sqrtf(m[2] * m[2] + m[3] * m[3]));
	if (*dw < 1) {
		*dw = 1;
	}
	if (*dh < 1) {
		*dh = 1;
	}
}

static struct pdf_image *cached_image(struct rctx *c, struct pdf_obj *stream, struct pdf_obj *resources)
{
	int dw, dh;

	image_device_size(c, &dw, &dh);
	for (int i = 0; i < 8; i++) {
		if (c->imgs[i].stream == stream && c->imgs[i].img != NULL) {
			return c->imgs[i].img;
		}
	}
	struct pdf_image *img = pdf_image_load_hint(c->doc, stream, resources, dw, dh);

	if (img == NULL) {
		return NULL;
	}
	int slot = -1;

	for (int i = 0; i < 8; i++) {
		if (c->imgs[i].img == NULL) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = 0;
		pdf_image_free(c->imgs[0].img);
		memmove(&c->imgs[0], &c->imgs[1], 7 * sizeof(c->imgs[0]));
		slot = 7;
	}
	c->imgs[slot].stream = stream;
	c->imgs[slot].img = img;
	return img;
}

/* ---------------------------------------------------------------------- */
/* form XObjects, patterns, annotations                                   */

static void clip_to_bbox(struct rctx *c, struct pdf_obj *bbox, const float m[6])
{
	if (pdf_array_len(bbox) < 4) {
		return;
	}
	float bx0 = pdf_num(pdf_array_get(c->doc, bbox, 0)), by0 = pdf_num(pdf_array_get(c->doc, bbox, 1));
	float bx1 = pdf_num(pdf_array_get(c->doc, bbox, 2)), by1 = pdf_num(pdf_array_get(c->doc, bbox, 3));
	float xs[4], ys[4];

	mat_apply(m, bx0, by0, &xs[0], &ys[0]);
	mat_apply(m, bx1, by0, &xs[1], &ys[1]);
	mat_apply(m, bx1, by1, &xs[2], &ys[2]);
	mat_apply(m, bx0, by1, &xs[3], &ys[3]);
	bool axis = (fabsf(m[1]) < 1e-6f && fabsf(m[2]) < 1e-6f) || (fabsf(m[0]) < 1e-6f && fabsf(m[3]) < 1e-6f);

	if (axis) {
		float mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];

		for (int i = 1; i < 4; i++) {
			mnx = xs[i] < mnx ? xs[i] : mnx;
			mxx = xs[i] > mxx ? xs[i] : mxx;
			mny = ys[i] < mny ? ys[i] : mny;
			mxy = ys[i] > mxy ? ys[i] : mxy;
		}
		clip_intersect_rect(cur(c), mnx, mny, mxx, mxy);
		return;
	}
	rast_path_reset(&c->rp);
	rast_move_to(&c->rp, xs[0], ys[0]);
	for (int i = 1; i < 4; i++) {
		rast_line_to(&c->rp, xs[i], ys[i]);
	}
	rast_close(&c->rp);
	clip_with_rast(c, &c->rp, false);
}

static void do_form(struct rctx *c, struct pdf_obj *xo, struct pdf_obj *parent_res)
{
	if (c->depth >= MAX_FORM_DEPTH) {
		return;
	}
	for (int i = 0; i < c->depth; i++) {
		if (c->form_path[i] == xo) {
			return;
		}
	}
	uint8_t *data;
	size_t len;

	if (pdf_stream_decode(c->doc, xo, &data, &len) < 0) {
		return;
	}
	struct pdf_obj *res = pdf_get(c->doc, xo, "Resources");
	struct pdf_obj *mtx = pdf_get(c->doc, xo, "Matrix");
	struct pdf_obj *bbox = pdf_get(c->doc, xo, "BBox");

	if (res == NULL) {
		res = parent_res;
	}
	struct dpath saved_path = c->path;
	int saved_pending = c->pending_clip;
	float saved_tm[6], saved_tlm[6], saved_pb[6];

	memcpy(saved_tm, c->tm, sizeof(saved_tm));
	memcpy(saved_tlm, c->tlm, sizeof(saved_tlm));
	memcpy(saved_pb, c->pattern_base, sizeof(saved_pb));
	memset(&c->path, 0, sizeof(c->path));
	c->pending_clip = 0;
	gs_push(c);
	int saved_gsp = c->gsp;

	if (pdf_array_len(mtx) >= 6) {
		float m[6];

		for (int i = 0; i < 6; i++) {
			m[i] = pdf_num(pdf_array_get(c->doc, mtx, (size_t)i));
		}
		mat_mul(m, cur(c)->ctm, cur(c)->ctm);
	}
	memcpy(c->pattern_base, cur(c)->ctm, sizeof(c->pattern_base));
	clip_to_bbox(c, bbox, cur(c)->ctm);
	/*
	 * Transparency groups are composited as a whole with the soft mask and
	 * alpha in effect at Do; inside, states may reset them. Pin them here
	 * and apply them to every object of the group (an approximation of
	 * group compositing).
	 */
	struct mask *saved_group = c->group_mask;
	float saved_group_alpha = c->group_alpha;
	bool is_group = pdf_get(c->doc, xo, "Group") != NULL;

	if (is_group) {
		struct gstate *fg = cur(c);

		if (fg->smask != NULL) {
			if (c->group_mask == NULL) {
				c->group_mask = mask_ref(fg->smask);
			} else {
				struct mask *comb = mask_new(c);

				if (comb != NULL) {
					size_t n = (size_t)c->w * (size_t)c->h;

					for (size_t i = 0; i < n; i++) {
						comb->data[i] = (uint8_t)((c->group_mask->data[i] * fg->smask->data[i] + 127) / 255);
					}
					c->group_mask = comb;
				} else {
					mask_ref(c->group_mask);
				}
			}
			mask_unref(fg->smask);
			fg->smask = NULL;
		} else {
			mask_ref(c->group_mask);
		}
		c->group_alpha = saved_group_alpha * fg->fill_alpha;
		fg->fill_alpha = 1.0f;
		fg->stroke_alpha = 1.0f;
	}
	c->form_path[c->depth++] = xo;
	run_content(c, data, len, res);
	c->depth--;
	while (c->gsp > saved_gsp) {
		gs_pop(c);
	}
	gs_pop(c);
	if (is_group) {
		mask_unref(c->group_mask);
		c->group_mask = saved_group;
		c->group_alpha = saved_group_alpha;
	}
	dpath_free(&c->path);
	c->path = saved_path;
	c->pending_clip = saved_pending;
	memcpy(c->tm, saved_tm, sizeof(saved_tm));
	memcpy(c->tlm, saved_tlm, sizeof(saved_tlm));
	memcpy(c->pattern_base, saved_pb, sizeof(saved_pb));
	pdf_free(data);
}

static void paint_pattern(struct rctx *c, struct pdf_obj *pat, struct mask *region, bool stroke,
			  struct pdf_obj *resources)
{
	struct pdf_doc *doc = c->doc;
	int ptype = (int)pdf_int(pdf_get(doc, pat, "PatternType"));
	float pm[6];
	struct pdf_obj *mtx = pdf_get(doc, pat, "Matrix");

	memcpy(pm, identity, sizeof(pm));
	if (pdf_array_len(mtx) >= 6) {
		for (int i = 0; i < 6; i++) {
			pm[i] = pdf_num(pdf_array_get(doc, mtx, (size_t)i));
		}
	}
	float m[6];

	mat_mul(pm, c->pattern_base, m);
	struct gstate *g = cur(c);
	float alpha = stroke ? g->stroke_alpha : g->fill_alpha;

	if (ptype == 2) {
		struct pdf_obj *sh = pdf_get(doc, pat, "Shading");

		if (sh != NULL) {
			paint_shading(c, sh, m, region, alpha);
		}
		return;
	}
	if (ptype != 1 || pat->type != PDF_STREAM || c->depth >= MAX_FORM_DEPTH - 1) {
		return;
	}
	/* tiling pattern: run the cell content for every tile covering the region */
	struct pdf_obj *bbox = pdf_get(doc, pat, "BBox");
	float xstep = pdf_num(pdf_get(doc, pat, "XStep")), ystep = pdf_num(pdf_get(doc, pat, "YStep"));
	int paint_type = (int)pdf_int(pdf_get(doc, pat, "PaintType"));

	if (pdf_array_len(bbox) < 4) {
		return;
	}
	float bx0 = pdf_num(pdf_array_get(doc, bbox, 0)), by0 = pdf_num(pdf_array_get(doc, bbox, 1));
	float bx1 = pdf_num(pdf_array_get(doc, bbox, 2)), by1 = pdf_num(pdf_array_get(doc, bbox, 3));

	if (xstep == 0 || !(xstep == xstep)) {
		xstep = bx1 - bx0;
	}
	if (ystep == 0 || !(ystep == ystep)) {
		ystep = by1 - by0;
	}
	if (fabsf(xstep) < 1e-6f || fabsf(ystep) < 1e-6f) {
		return;
	}
	/* region bbox in device space -> pattern space to find the tile range */
	int rx0 = c->w, ry0 = c->h, rx1 = 0, ry1 = 0;

	for (int y = g->clip.y0; y < g->clip.y1; y++) {
		for (int x = g->clip.x0; x < g->clip.x1; x++) {
			if (region->data[(size_t)y * c->w + x]) {
				rx0 = x < rx0 ? x : rx0;
				rx1 = x > rx1 ? x : rx1;
				ry0 = y < ry0 ? y : ry0;
				ry1 = y > ry1 ? y : ry1;
			}
		}
	}
	if (rx1 < rx0) {
		return;
	}
	float inv[6];

	if (!mat_invert(m, inv)) {
		return;
	}
	float cx[4], cy[4];

	mat_apply(inv, (float)rx0, (float)ry0, &cx[0], &cy[0]);
	mat_apply(inv, (float)rx1 + 1, (float)ry0, &cx[1], &cy[1]);
	mat_apply(inv, (float)rx1 + 1, (float)ry1 + 1, &cx[2], &cy[2]);
	mat_apply(inv, (float)rx0, (float)ry1 + 1, &cx[3], &cy[3]);
	float pmnx = cx[0], pmxx = cx[0], pmny = cy[0], pmxy = cy[0];

	for (int i = 1; i < 4; i++) {
		pmnx = cx[i] < pmnx ? cx[i] : pmnx;
		pmxx = cx[i] > pmxx ? cx[i] : pmxx;
		pmny = cy[i] < pmny ? cy[i] : pmny;
		pmxy = cy[i] > pmxy ? cy[i] : pmxy;
	}
	int i0 = (int)floorf((pmnx - bx1) / fabsf(xstep)), i1 = (int)ceilf((pmxx - bx0) / fabsf(xstep));
	int j0 = (int)floorf((pmny - by1) / fabsf(ystep)), j1 = (int)ceilf((pmxy - by0) / fabsf(ystep));
	long ntiles = (long)(i1 - i0 + 1) * (long)(j1 - j0 + 1);

	if (ntiles <= 0) {
		return;
	}
	uint8_t *data;
	size_t len;

	if (pdf_stream_decode(doc, pat, &data, &len) < 0) {
		return;
	}
	struct pdf_obj *pres = pdf_get(doc, pat, "Resources");

	if (pres == NULL) {
		pres = resources;
	}
	/* state for the tiles: clip = clip ∩ region */
	struct dpath saved_path = c->path;
	int saved_pending = c->pending_clip;
	float saved_tm[6], saved_tlm[6], saved_pb[6];
	bool saved_ignore = c->ignore_color;

	memcpy(saved_tm, c->tm, sizeof(saved_tm));
	memcpy(saved_tlm, c->tlm, sizeof(saved_tlm));
	memcpy(saved_pb, c->pattern_base, sizeof(saved_pb));
	memset(&c->path, 0, sizeof(c->path));
	gs_push(c);
	struct gstate *tg = cur(c);
	struct mask *combined = mask_new(c);

	if (combined != NULL) {
		size_t n = (size_t)c->w * (size_t)c->h;

		for (size_t k = 0; k < n; k++) {
			combined->data[k] = tg->clip_mask ? (uint8_t)((region->data[k] * tg->clip_mask->data[k] + 127) / 255)
							  : region->data[k];
		}
		mask_unref(tg->clip_mask);
		tg->clip_mask = combined;
	}
	tg->clip.x0 = rx0 > tg->clip.x0 ? rx0 : tg->clip.x0;
	tg->clip.y0 = ry0 > tg->clip.y0 ? ry0 : tg->clip.y0;
	tg->clip.x1 = rx1 + 1 < tg->clip.x1 ? rx1 + 1 : tg->clip.x1;
	tg->clip.y1 = ry1 + 1 < tg->clip.y1 ? ry1 + 1 : tg->clip.y1;
	tg->fill_pat = NULL;
	tg->stroke_pat = NULL;
	if (paint_type == 2) {
		/* uncoloured: both colours come from the current fill/stroke colour */
		float *src = stroke ? tg->stroke_rgb : tg->fill_rgb;
		struct pdf_colorspace *rgbcs = pdf_colorspace_device(PDF_CS_RGB);

		memcpy(tg->fill_rgb, src, sizeof(tg->fill_rgb));
		memcpy(tg->stroke_rgb, src, sizeof(tg->stroke_rgb));
		if (rgbcs != NULL) {
			rgbcs->refs = 2;
			cs_unref(tg->fill_cs);
			cs_unref(tg->stroke_cs);
			tg->fill_cs = rgbcs;
			tg->stroke_cs = rgbcs;
		}
		c->ignore_color = true;
	} else {
		struct pdf_colorspace *gray = pdf_colorspace_device(PDF_CS_GRAY);

		if (gray != NULL) {
			gray->refs = 2;
			cs_unref(tg->fill_cs);
			cs_unref(tg->stroke_cs);
			tg->fill_cs = gray;
			tg->stroke_cs = gray;
		}
		tg->fill_rgb[0] = tg->fill_rgb[1] = tg->fill_rgb[2] = 0;
		tg->stroke_rgb[0] = tg->stroke_rgb[1] = tg->stroke_rgb[2] = 0;
	}
	int saved_gsp = c->gsp;
	long count = 0;

	c->depth++;
	for (int j = j0; j <= j1 && count < 4096; j++) {
		for (int i = i0; i <= i1 && count < 4096; i++, count++) {
			float t[6] = {1, 0, 0, 1, bx0 + (float)i * fabsf(xstep) - bx0, by0 + (float)j * fabsf(ystep) - by0};

			t[4] = (float)i * fabsf(xstep);
			t[5] = (float)j * fabsf(ystep);
			gs_push(c);
			mat_mul(t, m, cur(c)->ctm);
			memcpy(c->pattern_base, cur(c)->ctm, sizeof(c->pattern_base));
			/* skip tiles entirely outside the region */
			float tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;

			mat_apply(cur(c)->ctm, bx0, by0, &tx0, &ty0);
			mat_apply(cur(c)->ctm, bx1, by0, &tx1, &ty1);
			mat_apply(cur(c)->ctm, bx1, by1, &tx2, &ty2);
			mat_apply(cur(c)->ctm, bx0, by1, &tx3, &ty3);
			float mnx = fminf(fminf(tx0, tx1), fminf(tx2, tx3)), mxx = fmaxf(fmaxf(tx0, tx1), fmaxf(tx2, tx3));
			float mny = fminf(fminf(ty0, ty1), fminf(ty2, ty3)), mxy = fmaxf(fmaxf(ty0, ty1), fmaxf(ty2, ty3));

			if (mxx < rx0 || mnx > rx1 + 1 || mxy < ry0 || mny > ry1 + 1) {
				gs_pop(c);
				continue;
			}
			clip_to_bbox(c, bbox, cur(c)->ctm);
			dpath_reset(&c->path);
			c->pending_clip = 0;
			run_content(c, data, len, pres);
			while (c->gsp > saved_gsp + 1) {
				gs_pop(c);
			}
			gs_pop(c);
		}
	}
	c->depth--;
	while (c->gsp > saved_gsp) {
		gs_pop(c);
	}
	gs_pop(c);
	c->ignore_color = saved_ignore;
	dpath_free(&c->path);
	c->path = saved_path;
	c->pending_clip = saved_pending;
	memcpy(c->tm, saved_tm, sizeof(saved_tm));
	memcpy(c->tlm, saved_tlm, sizeof(saved_tlm));
	memcpy(c->pattern_base, saved_pb, sizeof(saved_pb));
	pdf_free(data);
}

static void draw_annotations(struct rctx *c, struct pdf_obj *page)
{
	struct pdf_doc *doc = c->doc;
	struct pdf_obj *annots = pdf_get(doc, page, "Annots");

	for (size_t i = 0; i < pdf_array_len(annots); i++) {
		struct pdf_obj *a = pdf_array_get(doc, annots, i);

		if (a == NULL || a->type != PDF_DICT) {
			continue;
		}
		struct pdf_obj *st = pdf_get(doc, a, "Subtype");

		if (pdf_is_name(st, "Link") || pdf_is_name(st, "Popup")) {
			continue;
		}
		int flags = (int)pdf_int(pdf_get(doc, a, "F"));

		if (flags & 2) {
			continue; /* hidden */
		}
		if (flags & 32) {
			continue; /* NoView */
		}
		struct pdf_obj *ap = pdf_get(doc, a, "AP");
		struct pdf_obj *n = pdf_get(doc, ap, "N");

		if (n != NULL && n->type == PDF_DICT) {
			struct pdf_obj *as = pdf_get(doc, a, "AS");

			if (as != NULL && as->type == PDF_NAME) {
				n = pdf_get(doc, n, pdf_name_str(as));
			} else if (n->dict.n == 1) {
				n = pdf_resolve(doc, n->dict.items[0].val);
			} else {
				n = NULL;
			}
		}
		if (n == NULL || n->type != PDF_STREAM) {
			continue;
		}
		struct pdf_obj *rect = pdf_get(doc, a, "Rect");

		if (pdf_array_len(rect) < 4) {
			continue;
		}
		float rx0 = pdf_num(pdf_array_get(doc, rect, 0)), ry0 = pdf_num(pdf_array_get(doc, rect, 1));
		float rx1 = pdf_num(pdf_array_get(doc, rect, 2)), ry1 = pdf_num(pdf_array_get(doc, rect, 3));

		if (rx1 < rx0) {
			float t = rx0;

			rx0 = rx1;
			rx1 = t;
		}
		if (ry1 < ry0) {
			float t = ry0;

			ry0 = ry1;
			ry1 = t;
		}
		/* map the transformed BBox onto Rect (algorithm 8.1) */
		struct pdf_obj *bbox = pdf_get(doc, n, "BBox");
		struct pdf_obj *mtx = pdf_get(doc, n, "Matrix");
		float m[6];

		memcpy(m, identity, sizeof(m));
		if (pdf_array_len(mtx) >= 6) {
			for (int k = 0; k < 6; k++) {
				m[k] = pdf_num(pdf_array_get(doc, mtx, (size_t)k));
			}
		}
		float A[6];

		memcpy(A, identity, sizeof(A));
		if (pdf_array_len(bbox) >= 4) {
			float bx0 = pdf_num(pdf_array_get(doc, bbox, 0)), by0 = pdf_num(pdf_array_get(doc, bbox, 1));
			float bx1 = pdf_num(pdf_array_get(doc, bbox, 2)), by1 = pdf_num(pdf_array_get(doc, bbox, 3));
			float xs[4], ys[4];

			mat_apply(m, bx0, by0, &xs[0], &ys[0]);
			mat_apply(m, bx1, by0, &xs[1], &ys[1]);
			mat_apply(m, bx1, by1, &xs[2], &ys[2]);
			mat_apply(m, bx0, by1, &xs[3], &ys[3]);
			float tx0 = fminf(fminf(xs[0], xs[1]), fminf(xs[2], xs[3]));
			float tx1 = fmaxf(fmaxf(xs[0], xs[1]), fmaxf(xs[2], xs[3]));
			float ty0 = fminf(fminf(ys[0], ys[1]), fminf(ys[2], ys[3]));
			float ty1 = fmaxf(fmaxf(ys[0], ys[1]), fmaxf(ys[2], ys[3]));
			float sx = (tx1 - tx0) > 1e-6f ? (rx1 - rx0) / (tx1 - tx0) : 1;
			float sy = (ty1 - ty0) > 1e-6f ? (ry1 - ry0) / (ty1 - ty0) : 1;

			A[0] = sx;
			A[3] = sy;
			A[4] = rx0 - tx0 * sx;
			A[5] = ry0 - ty0 * sy;
		}
		gs_push(c);
		mat_mul(A, c->base_ctm, cur(c)->ctm);
		do_form(c, n, NULL);
		gs_pop(c);
	}
}

/* ---------------------------------------------------------------------- */
/* inline images                                                          */

static void inline_image(struct rctx *c, struct pdf_lex *lx, struct pdf_obj *resources)
{
	struct pdf_obj *dict = pdf_obj_new(PDF_DICT);

	if (dict == NULL) {
		return;
	}
	for (;;) {
		struct pdf_obj *k = pdf_parse_obj(lx, false, 0);

		if (k == NULL) {
			pdf_obj_free(dict);
			return;
		}
		if (pdf_is_op(k, "ID")) {
			pdf_obj_free(k);
			break;
		}
		if (k->type != PDF_NAME) {
			pdf_obj_free(k);
			continue;
		}
		struct pdf_obj *v = pdf_parse_obj(lx, false, 0);

		if (v == NULL) {
			pdf_obj_free(k);
			pdf_obj_free(dict);
			return;
		}
		pdf_dict_put(dict, pdf_name_str(k), v);
		pdf_obj_free(k);
	}
	if (lx->pos < lx->len) {
		lx->pos++; /* single white space after ID */
	}
	size_t start = lx->pos;
	size_t datalen = 0;
	struct pdf_obj *filter = pdf_dict_get_raw(dict, "F");

	if (filter == NULL) {
		filter = pdf_dict_get_raw(dict, "Filter");
	}
	if (filter == NULL || (filter->type == PDF_ARRAY && filter->arr.n == 0)) {
		/* unencoded: size from the parameters */
		int w = (int)pdf_int(pdf_dict_get_raw(dict, "W") ? pdf_dict_get_raw(dict, "W") : pdf_dict_get_raw(dict, "Width"));
		int h = (int)pdf_int(pdf_dict_get_raw(dict, "H") ? pdf_dict_get_raw(dict, "H") : pdf_dict_get_raw(dict, "Height"));
		int bpc = (int)pdf_int(pdf_dict_get_raw(dict, "BPC") ? pdf_dict_get_raw(dict, "BPC") : pdf_dict_get_raw(dict, "BitsPerComponent"));
		struct pdf_obj *im = pdf_dict_get_raw(dict, "IM") ? pdf_dict_get_raw(dict, "IM") : pdf_dict_get_raw(dict, "ImageMask");
		int nc = 1;

		if (im != NULL && im->type == PDF_BOOL && im->b) {
			bpc = 1;
		} else {
			struct pdf_obj *cso = pdf_dict_get_raw(dict, "CS") ? pdf_dict_get_raw(dict, "CS") : pdf_dict_get_raw(dict, "ColorSpace");
			struct pdf_colorspace *cs = cso ? pdf_colorspace_load(c->doc, cso, resources) : NULL;

			if (cs != NULL) {
				nc = cs->ncomps;
				pdf_colorspace_free(cs);
			}
		}
		if (bpc <= 0) {
			bpc = 8;
		}
		datalen = (((size_t)w * (size_t)nc * (size_t)bpc + 7) / 8) * (size_t)h;
		if (start + datalen > lx->len) {
			datalen = lx->len - start;
		}
		lx->pos = start + datalen;
	} else {
		size_t p = start;

		while (p + 1 < lx->len) {
			if (lx->buf[p] == 'E' && lx->buf[p + 1] == 'I' && (p == 0 || pdf_is_white(lx->buf[p - 1])) &&
			    (p + 2 >= lx->len || pdf_is_white(lx->buf[p + 2]) || pdf_is_delim(lx->buf[p + 2]))) {
				break;
			}
			p++;
		}
		datalen = (p > start) ? p - start : 0;
		if (datalen > 0 && pdf_is_white(lx->buf[start + datalen - 1])) {
			datalen--;
		}
		lx->pos = p;
	}
	/* consume EI */
	pdf_lex_skip_ws(lx);
	if (lx->pos + 2 <= lx->len && lx->buf[lx->pos] == 'E' && lx->buf[lx->pos + 1] == 'I') {
		lx->pos += 2;
	}
	struct pdf_obj *stream = pdf_obj_new(PDF_STREAM);

	if (stream == NULL) {
		pdf_obj_free(dict);
		return;
	}
	stream->stream.dict = dict;
	stream->stream.data = lx->buf + start;
	stream->stream.len = datalen;
	int dw, dh;

	image_device_size(c, &dw, &dh);
	struct pdf_image *img = pdf_image_load_hint(c->doc, stream, resources, dw, dh);

	if (img != NULL) {
		draw_image(c, img);
		pdf_image_free(img);
	}
	pdf_obj_free(stream);
}

/* ---------------------------------------------------------------------- */
/* content stream interpreter                                             */

static float num_at(struct pdf_obj **ops, int n, int i)
{
	return (i >= 0 && i < n) ? pdf_num(ops[i]) : 0.0f;
}

static void run_content(struct rctx *c, const uint8_t *data, size_t len, struct pdf_obj *resources)
{
	struct pdf_lex lx;
	struct pdf_obj *ops[OPERANDS];
	int nops = 0;
	int compat = 0;

	pdf_lex_init(&lx, data, len, 0);
	for (;;) {
		struct pdf_obj *o = pdf_parse_obj(&lx, false, 0);

		if (o == NULL) {
			break;
		}
		if (++c->ops > MAX_OPS) {
			pdf_obj_free(o);
			break;
		}
#if !defined(__ZEPHYR__) && defined(ZPDF_DEBUG)
		{
			const char *lim = getenv("ZPDF_OPS");

			if (lim != NULL && c->ops > atol(lim)) {
				pdf_obj_free(o);
				break;
			}
			if (getenv("ZPDF_TRACE") != NULL && o->type == PDF_OP) {
				fprintf(stderr, "%ld: %s (%d operands) depth %d\n", c->ops, o->op, nops, c->depth);
			}
		}
#endif
		if (o->type != PDF_OP) {
			if (nops == OPERANDS) {
				pdf_obj_free(ops[0]);
				memmove(ops, ops + 1, (OPERANDS - 1) * sizeof(ops[0]));
				nops--;
			}
			ops[nops++] = o;
			continue;
		}
		const char *op = o->op;
		struct gstate *g = cur(c);

		switch (op[0]) {
		case 'q':
			if (op[1] == 0) {
				gs_push(c);
			}
			break;
		case 'Q':
			if (op[1] == 0) {
				gs_pop(c);
			}
			break;
		case 'c':
			if (op[1] == 'm' && nops >= 6) {
				float m[6];

				for (int i = 0; i < 6; i++) {
					m[i] = num_at(ops, nops, nops - 6 + i);
				}
				mat_mul(m, g->ctm, g->ctm);
			} else if (op[1] == 0 && nops >= 6) {
				path_curveto(c, num_at(ops, nops, nops - 6), num_at(ops, nops, nops - 5),
					     num_at(ops, nops, nops - 4), num_at(ops, nops, nops - 3),
					     num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
			} else if (op[1] == 's' && op[2] == 0 && nops >= 1) {
				set_cs(c, pdf_colorspace_load(c->doc, ops[nops - 1], resources), false);
			}
			break;
		case 'C':
			if (op[1] == 'S' && nops >= 1) {
				set_cs(c, pdf_colorspace_load(c->doc, ops[nops - 1], resources), true);
			}
			break;
		case 'm':
			if (op[1] == 0 && nops >= 2) {
				path_moveto(c, num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
			}
			break;
		case 'l':
			if (op[1] == 0 && nops >= 2) {
				path_lineto(c, num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
			}
			break;
		case 'v':
			if (op[1] == 0 && nops >= 4) {
				float x0, y0;

				path_current_user(c, &x0, &y0);
				path_curveto(c, x0, y0, num_at(ops, nops, nops - 4), num_at(ops, nops, nops - 3),
					     num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
			}
			break;
		case 'y':
			if (op[1] == 0 && nops >= 4) {
				path_curveto(c, num_at(ops, nops, nops - 4), num_at(ops, nops, nops - 3),
					     num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1),
					     num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
			}
			break;
		case 'h':
			if (op[1] == 0) {
				path_close(c);
			}
			break;
		case 'r':
			if (op[1] == 'e' && nops >= 4) {
				float x = num_at(ops, nops, nops - 4), y = num_at(ops, nops, nops - 3);
				float w = num_at(ops, nops, nops - 2), h = num_at(ops, nops, nops - 1);

				path_moveto(c, x, y);
				path_lineto(c, x + w, y);
				path_lineto(c, x + w, y + h);
				path_lineto(c, x, y + h);
				path_close(c);
			} else if (op[1] == 'g' && nops >= 3) {
				set_device_color(c, PDF_CS_RGB, ops + nops - 3, 3, false);
			} else if (op[1] == 'i') {
				/* rendering intent */
			}
			break;
		case 'R':
			if (op[1] == 'G' && nops >= 3) {
				set_device_color(c, PDF_CS_RGB, ops + nops - 3, 3, true);
			}
			break;
		case 'g':
			if (op[1] == 0 && nops >= 1) {
				set_device_color(c, PDF_CS_GRAY, ops + nops - 1, 1, false);
			} else if (op[1] == 's' && nops >= 1 && ops[nops - 1]->type == PDF_NAME) {
				struct pdf_obj *egs = pdf_get(c->doc, resources, "ExtGState");

				apply_extgstate(c, pdf_get(c->doc, egs, pdf_name_str(ops[nops - 1])));
			}
			break;
		case 'G':
			if (op[1] == 0 && nops >= 1) {
				set_device_color(c, PDF_CS_GRAY, ops + nops - 1, 1, true);
			}
			break;
		case 'k':
			if (op[1] == 0 && nops >= 4) {
				set_device_color(c, PDF_CS_CMYK, ops + nops - 4, 4, false);
			}
			break;
		case 'K':
			if (op[1] == 0 && nops >= 4) {
				set_device_color(c, PDF_CS_CMYK, ops + nops - 4, 4, true);
			}
			break;
		case 's':
			if (op[1] == 0) {
				path_close(c);
				stroke_path(c, resources);
				end_path(c);
			} else if (op[1] == 'c' && (op[2] == 0 || op[2] == 'n')) {
				set_color(c, ops, nops, false, resources);
			} else if (op[1] == 'h' && op[2] == 0 && nops >= 1 && ops[nops - 1]->type == PDF_NAME) {
				struct pdf_obj *shd = pdf_get(c->doc, resources, "Shading");
				struct pdf_obj *sh = pdf_get(c->doc, shd, pdf_name_str(ops[nops - 1]));

				if (sh != NULL) {
					paint_shading(c, sh, g->ctm, NULL, g->fill_alpha);
				}
			}
			break;
		case 'S':
			if (op[1] == 0) {
				stroke_path(c, resources);
				end_path(c);
			} else if (op[1] == 'C' && (op[2] == 0 || op[2] == 'N')) {
				set_color(c, ops, nops, true, resources);
			}
			break;
		case 'f':
		case 'F':
			if (op[1] == 0 || (op[1] == '*' && op[2] == 0)) {
				fill_path(c, op[1] == '*', resources);
				end_path(c);
			}
			break;
		case 'B':
			if (op[1] == 0 || (op[1] == '*' && op[2] == 0)) {
				fill_path(c, op[1] == '*', resources);
				stroke_path(c, resources);
				end_path(c);
			} else if (op[1] == 'T') {
				memcpy(c->tm, identity, sizeof(c->tm));
				memcpy(c->tlm, identity, sizeof(c->tlm));
			} else if (op[1] == 'I') {
				inline_image(c, &lx, resources);
			} else if (op[1] == 'X') {
				compat++;
			}
			break;
		case 'b':
			if (op[1] == 0 || (op[1] == '*' && op[2] == 0)) {
				path_close(c);
				fill_path(c, op[1] == '*', resources);
				stroke_path(c, resources);
				end_path(c);
			}
			break;
		case 'n':
			if (op[1] == 0) {
				end_path(c);
			}
			break;
		case 'W':
			if (op[1] == 0) {
				c->pending_clip = 1;
			} else if (op[1] == '*') {
				c->pending_clip = 2;
			}
			break;
		case 'w':
			if (op[1] == 0 && nops >= 1) {
				g->line_width = num_at(ops, nops, nops - 1);
			}
			break;
		case 'J':
			if (op[1] == 0 && nops >= 1) {
				g->line_cap = (int)pdf_int(ops[nops - 1]);
			}
			break;
		case 'j':
			if (op[1] == 0 && nops >= 1) {
				g->line_join = (int)pdf_int(ops[nops - 1]);
			}
			break;
		case 'd':
			if (op[1] == 0 && nops >= 2 && ops[nops - 2]->type == PDF_ARRAY) {
				struct pdf_obj *arr = ops[nops - 2];

				g->ndash = 0;
				for (size_t i = 0; i < arr->arr.n && i < MAX_DASH; i++) {
					g->dash[g->ndash++] = pdf_num(arr->arr.items[i]);
				}
				g->dash_phase = pdf_num(ops[nops - 1]);
			}
			/* d0 / d1: type 3 glyph metrics, ignored */
			break;
		case 'E':
			if (op[1] == 'T') {
				/* nothing to do; text clipping modes are not accumulated */
			} else if (op[1] == 'X') {
				if (compat > 0) {
					compat--;
				}
			}
			break;
		case 'T':
			switch (op[1]) {
			case 'f':
				if (nops >= 2 && ops[nops - 2]->type == PDF_NAME) {
					struct pdf_obj *fonts = pdf_get(c->doc, resources, "Font");
					struct pdf_obj *fd = pdf_get(c->doc, fonts, pdf_name_str(ops[nops - 2]));

					g->font = (fd != NULL && fd->type == PDF_DICT) ? get_rfont(c, fd) : NULL;
					if (g->font == NULL) {
						/* missing font resource: fall back to a built-in sans */
						static struct pdf_obj *fallback_dict;

						if (fallback_dict == NULL) {
							fallback_dict = pdf_obj_new(PDF_DICT);
							if (fallback_dict != NULL) {
								pdf_dict_put(fallback_dict, "BaseFont", pdf_obj_new_name("Helvetica", 9));
								pdf_dict_put(fallback_dict, "Subtype", pdf_obj_new_name("Type1", 5));
							}
						}
						if (fallback_dict != NULL) {
							g->font = get_rfont(c, fallback_dict);
						}
					}
					g->font_size = num_at(ops, nops, nops - 1);
				}
				break;
			case 'd':
				if (nops >= 2) {
					next_line(c, num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
				}
				break;
			case 'D':
				if (nops >= 2) {
					g->tl = -num_at(ops, nops, nops - 1);
					next_line(c, num_at(ops, nops, nops - 2), num_at(ops, nops, nops - 1));
				}
				break;
			case 'm':
				if (nops >= 6) {
					for (int i = 0; i < 6; i++) {
						c->tlm[i] = num_at(ops, nops, nops - 6 + i);
					}
					memcpy(c->tm, c->tlm, sizeof(c->tm));
				}
				break;
			case '*':
				next_line(c, 0, -g->tl);
				break;
			case 'L':
				g->tl = num_at(ops, nops, nops - 1);
				break;
			case 'c':
				g->tc = num_at(ops, nops, nops - 1);
				break;
			case 'w':
				g->tw = num_at(ops, nops, nops - 1);
				break;
			case 'z':
				g->th = num_at(ops, nops, nops - 1) / 100.0f;
				if (g->th == 0) {
					g->th = 1;
				}
				break;
			case 's':
				g->trise = num_at(ops, nops, nops - 1);
				break;
			case 'r':
				g->tmode = (int)pdf_int(nops ? ops[nops - 1] : NULL);
				break;
			case 'j':
				if (nops >= 1 && ops[nops - 1]->type == PDF_STRING) {
					show_text(c, ops[nops - 1]->str.data, ops[nops - 1]->str.len, resources);
				}
				break;
			case 'J':
				if (nops >= 1 && ops[nops - 1]->type == PDF_ARRAY) {
					struct pdf_obj *arr = ops[nops - 1];

					for (size_t i = 0; i < arr->arr.n; i++) {
						struct pdf_obj *e = arr->arr.items[i];

						if (e->type == PDF_STRING) {
							show_text(c, e->str.data, e->str.len, resources);
						} else if (pdf_is_num(e)) {
							adjust_tj(c, pdf_num(e));
						}
					}
				}
				break;
			default:
				break;
			}
			break;
		case '\'':
			if (op[1] == 0 && nops >= 1 && ops[nops - 1]->type == PDF_STRING) {
				next_line(c, 0, -g->tl);
				show_text(c, ops[nops - 1]->str.data, ops[nops - 1]->str.len, resources);
			}
			break;
		case '"':
			if (op[1] == 0 && nops >= 3 && ops[nops - 1]->type == PDF_STRING) {
				g->tw = num_at(ops, nops, nops - 3);
				g->tc = num_at(ops, nops, nops - 2);
				next_line(c, 0, -g->tl);
				show_text(c, ops[nops - 1]->str.data, ops[nops - 1]->str.len, resources);
			}
			break;
		case 'D':
			if (op[1] == 'o' && nops >= 1 && ops[nops - 1]->type == PDF_NAME) {
				struct pdf_obj *xobjs = pdf_get(c->doc, resources, "XObject");
				struct pdf_obj *xo = pdf_get(c->doc, xobjs, pdf_name_str(ops[nops - 1]));

				if (xo != NULL && xo->type == PDF_STREAM) {
					struct pdf_obj *st = pdf_get(c->doc, xo, "Subtype");

					if (pdf_is_name(st, "Image")) {
						struct pdf_image *img = cached_image(c, xo, resources);

						if (img != NULL) {
							draw_image(c, img);
						}
					} else if (pdf_is_name(st, "Form") || pdf_get(c->doc, xo, "BBox") != NULL) {
						do_form(c, xo, resources);
					}
				}
			}
			break;
		default:
			break;
		}
		pdf_obj_free(o);
		for (int i = 0; i < nops; i++) {
			pdf_obj_free(ops[i]);
		}
		nops = 0;
	}
	for (int i = 0; i < nops; i++) {
		pdf_obj_free(ops[i]);
	}
}

/* ---------------------------------------------------------------------- */
/* page setup                                                             */

static void page_box(struct pdf_doc *doc, struct pdf_obj *page, float box[4], int *rotate)
{
	struct pdf_obj *mb = pdf_page_attr(doc, page, "MediaBox");
	struct pdf_obj *cb = pdf_page_attr(doc, page, "CropBox");

	box[0] = 0;
	box[1] = 0;
	box[2] = 612;
	box[3] = 792;
	if (pdf_array_len(mb) >= 4) {
		for (int i = 0; i < 4; i++) {
			box[i] = pdf_num(pdf_array_get(doc, mb, (size_t)i));
		}
	}
	if (pdf_array_len(cb) >= 4) {
		float c[4];

		for (int i = 0; i < 4; i++) {
			c[i] = pdf_num(pdf_array_get(doc, cb, (size_t)i));
		}
		/* intersect with the media box */
		float x0 = fmaxf(fminf(box[0], box[2]), fminf(c[0], c[2]));
		float x1 = fminf(fmaxf(box[0], box[2]), fmaxf(c[0], c[2]));
		float y0 = fmaxf(fminf(box[1], box[3]), fminf(c[1], c[3]));
		float y1 = fminf(fmaxf(box[1], box[3]), fmaxf(c[1], c[3]));

		if (x1 > x0 && y1 > y0) {
			box[0] = x0;
			box[1] = y0;
			box[2] = x1;
			box[3] = y1;
		}
	}
	if (box[2] < box[0]) {
		float t = box[0];

		box[0] = box[2];
		box[2] = t;
	}
	if (box[3] < box[1]) {
		float t = box[1];

		box[1] = box[3];
		box[3] = t;
	}
	if (box[2] - box[0] < 1 || box[3] - box[1] < 1) {
		box[0] = 0;
		box[1] = 0;
		box[2] = 612;
		box[3] = 792;
	}
	int r = (int)pdf_int(pdf_page_attr(doc, page, "Rotate"));

	r = ((r % 360) + 360) % 360;
	*rotate = (r / 90) * 90;
}

int pdf_page_size(struct pdf_doc *doc, size_t index, float *w, float *h)
{
	struct pdf_obj *page = pdf_page(doc, index);
	float box[4];
	int rot;

	if (page == NULL) {
		return -1;
	}
	page_box(doc, page, box, &rot);
	float pw = box[2] - box[0], ph = box[3] - box[1];

	if (rot == 90 || rot == 270) {
		*w = ph;
		*h = pw;
	} else {
		*w = pw;
		*h = ph;
	}
	return 0;
}

int pdf_render_page(struct pdf_doc *doc, size_t index, float scale, struct pdf_bitmap *out)
{
	return pdf_render_region(doc, index, scale, 0, 0, 0, 0, out);
}

int pdf_render_region(struct pdf_doc *doc, size_t index, float scale, int rx, int ry, int rw, int rh,
		      struct pdf_bitmap *out)
{
	struct pdf_obj *page = pdf_page(doc, index);

	memset(out, 0, sizeof(*out));
	if (page == NULL || scale <= 0) {
		return -1;
	}
	if (pdf_is_encrypted(doc)) {
		return -13;
	}
	float box[4];
	int rot;

	page_box(doc, page, box, &rot);
	float pw = (box[2] - box[0]) * scale, ph = (box[3] - box[1]) * scale;
	int W = (int)ceilf(pw - 0.01f), H = (int)ceilf(ph - 0.01f);

	if (rot == 90 || rot == 270) {
		int t = W;

		W = H;
		H = t;
	}
	/* region: default is the whole page */
	if (rw <= 0 || rh <= 0) {
		rx = 0;
		ry = 0;
		rw = W;
		rh = H;
	}
	if (rx < 0) {
		rw += rx;
		rx = 0;
	}
	if (ry < 0) {
		rh += ry;
		ry = 0;
	}
	if (rx + rw > W) {
		rw = W - rx;
	}
	if (ry + rh > H) {
		rh = H - ry;
	}
	int full_w = W, full_h = H;

	W = rw;
	H = rh;
	(void)full_w;
	(void)full_h;
	if (W < 1 || H < 1 || (long)W * H > 64L * 1024 * 1024) {
		return -1;
	}
	struct rctx *c = pdf_calloc(sizeof(*c));

	if (c == NULL) {
		return -12;
	}
	c->doc = doc;
	c->w = W;
	c->h = H;
	c->group_alpha = 1.0f;
	c->rgb = pdf_malloc((size_t)W * (size_t)H * 3);
	if (c->rgb == NULL) {
		pdf_free(c);
		return -12;
	}
	memset(c->rgb, 255, (size_t)W * (size_t)H * 3);
	rast_path_init(&c->rp);

	/* page space -> device: flip y, then rotate */
	float flip[6] = {scale, 0, 0, -scale, -box[0] * scale, box[3] * scale};
	float rotm[6];

	switch (rot) {
	case 90:
		rotm[0] = 0; rotm[1] = 1; rotm[2] = -1; rotm[3] = 0; rotm[4] = ph; rotm[5] = 0;
		break;
	case 180:
		rotm[0] = -1; rotm[1] = 0; rotm[2] = 0; rotm[3] = -1; rotm[4] = pw; rotm[5] = ph;
		break;
	case 270:
		rotm[0] = 0; rotm[1] = -1; rotm[2] = 1; rotm[3] = 0; rotm[4] = 0; rotm[5] = pw;
		break;
	default:
		memcpy(rotm, identity, sizeof(rotm));
		break;
	}
	mat_mul(flip, rotm, c->base_ctm);
	/* shift the region to the origin of the bitmap */
	c->base_ctm[4] -= (float)rx;
	c->base_ctm[5] -= (float)ry;
	memcpy(c->pattern_base, c->base_ctm, sizeof(c->pattern_base));

	struct gstate *g = &c->gs[0];

	memset(g, 0, sizeof(*g));
	memcpy(g->ctm, c->base_ctm, sizeof(g->ctm));
	g->clip.x0 = 0;
	g->clip.y0 = 0;
	g->clip.x1 = W;
	g->clip.y1 = H;
	g->fill_alpha = g->stroke_alpha = 1.0f;
	g->line_width = 1.0f;
	g->th = 1.0f;
	g->fill_cs = pdf_colorspace_device(PDF_CS_GRAY);
	g->stroke_cs = pdf_colorspace_device(PDF_CS_GRAY);
	if (g->fill_cs) {
		g->fill_cs->refs = 1;
	}
	if (g->stroke_cs) {
		g->stroke_cs->refs = 1;
	}
	memcpy(c->tm, identity, sizeof(c->tm));
	memcpy(c->tlm, identity, sizeof(c->tlm));

	uint8_t *content;
	size_t clen;

	if (pdf_page_content(doc, page, &content, &clen) == 0) {
		struct pdf_obj *res = pdf_page_attr(doc, page, "Resources");

		run_content(c, content, clen, res);
		pdf_free(content);
	}
	while (c->gsp > 0) {
		gs_pop(c);
	}
	draw_annotations(c, page);
	while (c->gsp > 0) {
		gs_pop(c);
	}
	gs_release(&c->gs[0]);

	out->w = W;
	out->h = H;
	out->stride = W * 3;
	out->rgb = c->rgb;

	for (int i = 0; i < c->nfonts; i++) {
		rfont_free(c->fonts[i]);
	}
	pdf_free(c->fonts);
	for (int i = 0; i < 8; i++) {
		pdf_image_free(c->imgs[i].img);
	}
	if (c->glyphs != NULL) {
		for (int i = 0; i < GLYPH_CACHE_SLOTS; i++) {
			pdf_free(c->glyphs[i].cov);
		}
		pdf_free(c->glyphs);
	}
	PDF_INF("glyph cache: %ld hits, %ld misses", c->glyph_hits, c->glyph_misses);
	rast_path_free(&c->rp);
	rast_scratch_free(&c->scratch);
	dpath_free(&c->path);
	pdf_free(c);
	return 0;
}

void pdf_bitmap_free(struct pdf_bitmap *bm)
{
	if (bm != NULL) {
		pdf_free(bm->rgb);
		bm->rgb = NULL;
	}
}
