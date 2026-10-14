/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Anti-aliased scanline rasterizer for device space polygons. Curves are
 * flattened by the caller; the fill rule is nonzero or even-odd. Coverage
 * is produced row by row through a span callback, so the painter decides
 * how to blend.
 */
#ifndef ZPDF_PDF_RASTER_H
#define ZPDF_PDF_RASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rast_edge {
	float x0, y0, x1, y1; /* y0 < y1 */
	int8_t dir;           /* +1 when the original edge pointed down (increasing y) */
};

struct rast_path {
	struct rast_edge *e;
	size_t n, cap;
	float minx, miny, maxx, maxy;
	/* subpath bookkeeping for the move/line/close helpers */
	float cur_x, cur_y, start_x, start_y;
	bool open;
	bool oom;
};

struct rast_clip {
	int x0, y0, x1, y1; /* x1, y1 exclusive */
};

/* Coverage row callback: n bytes of coverage (0..255) starting at pixel x0 of row y. */
typedef void (*rast_span_fn)(void *ud, int y, int x0, const uint8_t *cov, int n);

void rast_path_init(struct rast_path *p);
void rast_path_reset(struct rast_path *p);
void rast_path_free(struct rast_path *p);

/* Polygon construction (device coordinates, y down). */
void rast_move_to(struct rast_path *p, float x, float y);
void rast_line_to(struct rast_path *p, float x, float y);
void rast_close(struct rast_path *p);
/* Cubic and quadratic Béziers flattened from the current point. */
void rast_curve_to(struct rast_path *p, float x1, float y1, float x2, float y2, float x3, float y3);
void rast_quad_to(struct rast_path *p, float cx, float cy, float x, float y);
/* Add a closed polygon with its orientation normalised (used by the stroker). */
void rast_add_polygon(struct rast_path *p, const float *xy, int npts);

static inline bool rast_path_empty(const struct rast_path *p)
{
	return p->n == 0;
}

/* Reusable working memory for rast_fill (avoids allocations per shape). */
struct rast_scratch {
	struct rast_edge *edges;
	int *active;
	void *crossings;
	size_t edge_cap;
	uint16_t *acc;
	uint8_t *row;
	size_t row_cap;
};

void rast_scratch_free(struct rast_scratch *s);

/*
 * Rasterize the polygons of @p within @clip. The callback receives every
 * row that has coverage. @scratch may be NULL (temporary buffers are then
 * allocated). Returns 0, or -ENOMEM when working memory is unavailable.
 */
int rast_fill(const struct rast_path *p, bool evenodd, const struct rast_clip *clip,
	      rast_span_fn fn, void *ud);
int rast_fill_scratch(const struct rast_path *p, bool evenodd, const struct rast_clip *clip,
		      rast_span_fn fn, void *ud, struct rast_scratch *scratch);

/*
 * Build the outline of a stroked polyline into @out as polygons for a
 * nonzero fill. @xy holds npts points; width is in device pixels. Round
 * joins are added for widths above 1.5 px; caps are butt (0), round (1)
 * or square (2).
 */
void rast_stroke_polyline(struct rast_path *out, const float *xy, int npts, bool closed,
			  float width, int cap, int join);

#endif
