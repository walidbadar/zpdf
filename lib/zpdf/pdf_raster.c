/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Scanline rasterizer: each pixel row is sampled with RAST_SS sub-scanlines;
 * on each, the crossings of the edges are sorted and spans are derived from
 * the winding numbers. Horizontal coverage of the span ends is exact, so
 * vertical edges are smooth and near-horizontal ones have RAST_SS levels.
 */
#include "pdf_raster.h"
#include "pdf_mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define RAST_SS 5
#define FLATNESS 0.25f

void rast_path_init(struct rast_path *p)
{
	memset(p, 0, sizeof(*p));
	p->minx = p->miny = 1e30f;
	p->maxx = p->maxy = -1e30f;
}

void rast_path_reset(struct rast_path *p)
{
	p->n = 0;
	p->open = false;
	p->oom = false;
	p->minx = p->miny = 1e30f;
	p->maxx = p->maxy = -1e30f;
}

void rast_path_free(struct rast_path *p)
{
	pdf_free(p->e);
	p->e = NULL;
	p->n = p->cap = 0;
}

static void add_edge(struct rast_path *p, float x0, float y0, float x1, float y1)
{
	if (y0 == y1 || p->oom) {
		return;
	}
	if (!(x0 == x0) || !(y0 == y0) || !(x1 == x1) || !(y1 == y1)) {
		return; /* NaN */
	}
	if (p->n == p->cap) {
		size_t ncap = p->cap ? p->cap * 2 : 64;
		struct rast_edge *ne = pdf_realloc(p->e, ncap * sizeof(*ne));

		if (ne == NULL) {
			p->oom = true;
			return;
		}
		p->e = ne;
		p->cap = ncap;
	}
	struct rast_edge *e = &p->e[p->n++];

	if (y0 < y1) {
		e->x0 = x0;
		e->y0 = y0;
		e->x1 = x1;
		e->y1 = y1;
		e->dir = 1;
	} else {
		e->x0 = x1;
		e->y0 = y1;
		e->x1 = x0;
		e->y1 = y0;
		e->dir = -1;
	}
	if (x0 < p->minx) {
		p->minx = x0;
	}
	if (x1 < p->minx) {
		p->minx = x1;
	}
	if (x0 > p->maxx) {
		p->maxx = x0;
	}
	if (x1 > p->maxx) {
		p->maxx = x1;
	}
	if (e->y0 < p->miny) {
		p->miny = e->y0;
	}
	if (e->y1 > p->maxy) {
		p->maxy = e->y1;
	}
}

void rast_close(struct rast_path *p)
{
	if (p->open) {
		add_edge(p, p->cur_x, p->cur_y, p->start_x, p->start_y);
		p->cur_x = p->start_x;
		p->cur_y = p->start_y;
		p->open = false;
	}
}

void rast_move_to(struct rast_path *p, float x, float y)
{
	rast_close(p);
	p->cur_x = p->start_x = x;
	p->cur_y = p->start_y = y;
	p->open = true;
}

void rast_line_to(struct rast_path *p, float x, float y)
{
	if (!p->open) {
		rast_move_to(p, x, y);
		return;
	}
	add_edge(p, p->cur_x, p->cur_y, x, y);
	p->cur_x = x;
	p->cur_y = y;
}

void rast_curve_to(struct rast_path *p, float x1, float y1, float x2, float y2, float x3, float y3)
{
	float x0 = p->cur_x, y0 = p->cur_y;

	if (!p->open) {
		rast_move_to(p, x0, y0);
	}
	/* number of segments from the control polygon length */
	float d = fabsf(x1 - x0) + fabsf(y1 - y0) + fabsf(x2 - x1) + fabsf(y2 - y1) + fabsf(x3 - x2) +
		  fabsf(y3 - y2);
	int n = (int)ceilf(sqrtf(d / FLATNESS * 0.5f));

	if (n < 1) {
		n = 1;
	}
	if (n > 100) {
		n = 100;
	}
	for (int i = 1; i <= n; i++) {
		float t = (float)i / n, mt = 1.0f - t;
		float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, dd = t * t * t;

		rast_line_to(p, a * x0 + b * x1 + c * x2 + dd * x3, a * y0 + b * y1 + c * y2 + dd * y3);
	}
}

void rast_quad_to(struct rast_path *p, float cx, float cy, float x, float y)
{
	float x0 = p->cur_x, y0 = p->cur_y;

	rast_curve_to(p, x0 + 2.0f / 3.0f * (cx - x0), y0 + 2.0f / 3.0f * (cy - y0),
		      x + 2.0f / 3.0f * (cx - x), y + 2.0f / 3.0f * (cy - y), x, y);
}

void rast_add_polygon(struct rast_path *p, const float *xy, int npts)
{
	if (npts < 3) {
		return;
	}
	float area = 0.0f;

	for (int i = 0; i < npts; i++) {
		int j = (i + 1) % npts;

		area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
	}
	rast_close(p);
	if (area >= 0.0f) {
		rast_move_to(p, xy[0], xy[1]);
		for (int i = 1; i < npts; i++) {
			rast_line_to(p, xy[i * 2], xy[i * 2 + 1]);
		}
	} else {
		rast_move_to(p, xy[(npts - 1) * 2], xy[(npts - 1) * 2 + 1]);
		for (int i = npts - 2; i >= 0; i--) {
			rast_line_to(p, xy[i * 2], xy[i * 2 + 1]);
		}
	}
	rast_close(p);
}

/* ---------------------------------------------------------------------- */

struct crossing {
	float x;
	int8_t dir;
};

static int cmp_edge_y(const void *a, const void *b)
{
	const struct rast_edge *ea = a, *eb = b;

	return (ea->y0 < eb->y0) ? -1 : (ea->y0 > eb->y0);
}

static void sort_crossings(struct crossing *c, int n)
{
	/* insertion sort: n is small per scanline */
	for (int i = 1; i < n; i++) {
		struct crossing t = c[i];
		int j = i - 1;

		while (j >= 0 && c[j].x > t.x) {
			c[j + 1] = c[j];
			j--;
		}
		c[j + 1] = t;
	}
}

static inline void accumulate(uint16_t *acc, int w, float xa, float xb, uint16_t weight)
{
	/* add coverage weight to the pixels covered by [xa, xb) with fractional ends */
	if (xb <= 0.0f || xa >= (float)w) {
		return;
	}
	if (xa < 0.0f) {
		xa = 0.0f;
	}
	if (xb > (float)w) {
		xb = (float)w;
	}
	if (xb <= xa) {
		return;
	}
	int ia = (int)xa, ib = (int)xb;

	if (ia == ib) {
		acc[ia] += (uint16_t)(weight * (xb - xa));
		return;
	}
	acc[ia] += (uint16_t)(weight * ((float)(ia + 1) - xa));
	for (int x = ia + 1; x < ib; x++) {
		acc[x] += weight;
	}
	if (ib < w) {
		acc[ib] += (uint16_t)(weight * (xb - (float)ib));
	}
}

void rast_scratch_free(struct rast_scratch *s)
{
	pdf_free(s->edges);
	pdf_free(s->active);
	pdf_free(s->crossings);
	pdf_free(s->acc);
	pdf_free(s->row);
	memset(s, 0, sizeof(*s));
}

static int scratch_reserve(struct rast_scratch *s, size_t nedges, size_t width)
{
	if (nedges > s->edge_cap) {
		size_t ncap = s->edge_cap ? s->edge_cap : 256;

		while (ncap < nedges) {
			ncap *= 2;
		}
		struct rast_edge *e = pdf_realloc(s->edges, ncap * sizeof(*e));
		int *a = e ? pdf_realloc(s->active, ncap * sizeof(int)) : NULL;
		void *c = a ? pdf_realloc(s->crossings, ncap * sizeof(struct crossing)) : NULL;

		if (e) {
			s->edges = e;
		}
		if (a) {
			s->active = a;
		}
		if (c) {
			s->crossings = c;
		}
		if (!e || !a || !c) {
			return -ENOMEM;
		}
		s->edge_cap = ncap;
	}
	if (width > s->row_cap) {
		size_t ncap = s->row_cap ? s->row_cap : 512;

		while (ncap < width) {
			ncap *= 2;
		}
		uint16_t *acc = pdf_realloc(s->acc, ncap * sizeof(uint16_t));
		uint8_t *row = acc ? pdf_realloc(s->row, ncap) : NULL;

		if (acc) {
			s->acc = acc;
		}
		if (row) {
			s->row = row;
		}
		if (!acc || !row) {
			return -ENOMEM;
		}
		s->row_cap = ncap;
	}
	return 0;
}

int rast_fill(const struct rast_path *path, bool evenodd, const struct rast_clip *clip,
	      rast_span_fn fn, void *ud)
{
	struct rast_scratch tmp = {0};
	int rc = rast_fill_scratch(path, evenodd, clip, fn, ud, &tmp);

	rast_scratch_free(&tmp);
	return rc;
}

int rast_fill_scratch(const struct rast_path *path, bool evenodd, const struct rast_clip *clip,
		      rast_span_fn fn, void *ud, struct rast_scratch *scratch)
{
	if (path->n == 0 || path->oom) {
		return 0;
	}
	int cx0 = clip->x0, cy0 = clip->y0, cx1 = clip->x1, cy1 = clip->y1;

	if ((int)floorf(path->minx) > cx0) {
		cx0 = (int)floorf(path->minx);
	}
	if ((int)ceilf(path->maxx) + 1 < cx1) {
		cx1 = (int)ceilf(path->maxx) + 1;
	}
	if ((int)floorf(path->miny) > cy0) {
		cy0 = (int)floorf(path->miny);
	}
	if ((int)ceilf(path->maxy) + 1 < cy1) {
		cy1 = (int)ceilf(path->maxy) + 1;
	}
	if (cx0 >= cx1 || cy0 >= cy1) {
		return 0;
	}
	int w = cx1 - cx0;
	size_t n = path->n;

	if (scratch_reserve(scratch, n, (size_t)w) < 0) {
		return -ENOMEM;
	}
	struct rast_edge *edges = scratch->edges;
	uint16_t *acc = scratch->acc;
	uint8_t *row = scratch->row;
	struct crossing *cr = scratch->crossings;
	int *active = scratch->active;

	memcpy(edges, path->e, n * sizeof(*edges));
	qsort(edges, n, sizeof(*edges), cmp_edge_y);

	const uint16_t weight = (uint16_t)(255 / RAST_SS) + 1; /* 52 for 5 samples: max 260 */
	size_t next = 0;
	int nactive = 0;

	for (int y = cy0; y < cy1; y++) {
		float ytop = (float)y, ybot = (float)(y + 1);

		/* drop finished edges, add newly started ones */
		int k = 0;

		for (int i = 0; i < nactive; i++) {
			if (edges[active[i]].y1 > ytop) {
				active[k++] = active[i];
			}
		}
		nactive = k;
		while (next < n && edges[next].y0 < ybot) {
			if (edges[next].y1 > ytop) {
				active[nactive++] = (int)next;
			}
			next++;
		}
		if (nactive == 0) {
			if (next >= n) {
				break;
			}
			continue;
		}
		memset(acc, 0, (size_t)w * sizeof(uint16_t));
		int minx = w, maxx = -1;

		for (int s = 0; s < RAST_SS; s++) {
			float sy = ytop + ((float)s + 0.5f) / RAST_SS;
			int nc = 0;

			for (int i = 0; i < nactive; i++) {
				const struct rast_edge *e = &edges[active[i]];

				if (sy >= e->y0 && sy < e->y1) {
					float t = (sy - e->y0) / (e->y1 - e->y0);

					cr[nc].x = e->x0 + t * (e->x1 - e->x0) - (float)cx0;
					cr[nc].dir = e->dir;
					nc++;
				}
			}
			if (nc < 2) {
				continue;
			}
			sort_crossings(cr, nc);
			int wind = 0;

			for (int i = 0; i + 1 < nc; i++) {
				wind += evenodd ? 1 : cr[i].dir;
				bool inside = evenodd ? (wind & 1) : (wind != 0);

				if (inside) {
					float xa = cr[i].x, xb = cr[i + 1].x;

					accumulate(acc, w, xa, xb, weight);
					int ia = (int)floorf(xa), ib = (int)ceilf(xb);

					if (ia < minx) {
						minx = ia < 0 ? 0 : ia;
					}
					if (ib > maxx) {
						maxx = ib >= w ? w - 1 : ib;
					}
				}
			}
		}
		if (maxx < minx) {
			continue;
		}
		int cnt = maxx - minx + 1;

		for (int x = 0; x < cnt; x++) {
			uint16_t v = acc[minx + x];

			row[x] = (uint8_t)(v > 255 ? 255 : v);
		}
		fn(ud, y, cx0 + minx, row, cnt);
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* stroking                                                               */

static void add_round(struct rast_path *out, float cx, float cy, float r)
{
	float pts[2 * 16];
	int n = r < 2.0f ? 8 : (r < 6.0f ? 12 : 16);

	for (int i = 0; i < n; i++) {
		float a = 6.2831853f * (float)i / n;

		pts[i * 2] = cx + r * cosf(a);
		pts[i * 2 + 1] = cy + r * sinf(a);
	}
	rast_add_polygon(out, pts, n);
}

void rast_stroke_polyline(struct rast_path *out, const float *xy, int npts, bool closed,
			  float width, int cap, int join)
{
	float hw = width * 0.5f;

	if (npts == 1 || (npts == 2 && xy[0] == xy[2] && xy[1] == xy[3])) {
		/* a dot: only drawn with round or square caps */
		if (cap == 1) {
			add_round(out, xy[0], xy[1], hw);
		} else if (cap == 2) {
			float q[8] = {xy[0] - hw, xy[1] - hw, xy[0] + hw, xy[1] - hw,
				      xy[0] + hw, xy[1] + hw, xy[0] - hw, xy[1] + hw};

			rast_add_polygon(out, q, 4);
		}
		return;
	}
	int nseg = closed ? npts : npts - 1;

	for (int i = 0; i < nseg; i++) {
		int j = (i + 1) % npts;
		float x0 = xy[i * 2], y0 = xy[i * 2 + 1], x1 = xy[j * 2], y1 = xy[j * 2 + 1];
		float dx = x1 - x0, dy = y1 - y0;
		float len = sqrtf(dx * dx + dy * dy);

		if (len < 1e-6f) {
			continue;
		}
		dx /= len;
		dy /= len;
		float ex = 0.0f, ey = 0.0f;

		if (!closed && cap == 2) {
			ex = dx * hw;
			ey = dy * hw;
		}
		float sx0 = x0, sy0 = y0, sx1 = x1, sy1 = y1;

		if (!closed && i == 0) {
			sx0 -= ex;
			sy0 -= ey;
		}
		if (!closed && i == nseg - 1) {
			sx1 += ex;
			sy1 += ey;
		}
		float nx = -dy * hw, ny = dx * hw;
		float q[8] = {sx0 + nx, sy0 + ny, sx1 + nx, sy1 + ny, sx1 - nx, sy1 - ny, sx0 - nx, sy0 - ny};

		rast_add_polygon(out, q, 4);

		/* joins: round for widths that show them; miter approximated by round */
		bool has_join = closed || i < nseg - 1;

		if (has_join && width > 1.5f) {
			if (join == 2) {
				/* bevel: triangle between the two offset corners is covered by the round
				 * approximation well enough at these sizes */
			}
			add_round(out, x1, y1, hw);
		}
	}
	if (!closed && cap == 1 && width > 1.5f) {
		add_round(out, xy[0], xy[1], hw);
		add_round(out, xy[(npts - 1) * 2], xy[(npts - 1) * 2 + 1], hw);
	}
}
