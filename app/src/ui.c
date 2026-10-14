/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Reader in the style of Acrobat: a dark title bar, the page on a grey
 * workspace with a shadow, and a floating toolbar for page navigation and
 * zoom that hides itself while reading. The page is rendered in tiles
 * around the visible area, so zooming to 300 % costs memory for the
 * viewport, not for the whole page. A text view shows the extracted text.
 */
#include "ui.h"
#include "storage.h"
#include "worker.h"
#include <zpdf/pdf_doc.h>
#include <zpdf/pdf_text.h>
#include <zpdf/pdf_render.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

/*
 * Object flag helpers: the dedicated setters (lv_obj_set_hidden, ...) only
 * exist in newer LVGL releases; the flag calls exist in every LVGL 9.
 */
static inline void obj_set_hidden(lv_obj_t *o, bool hidden)
{
	if (hidden) {
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
	}
}

static inline bool obj_is_hidden(lv_obj_t *o)
{
	return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static inline void obj_set_scrollable(lv_obj_t *o, bool en)
{
	if (en) {
		lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	} else {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	}
}

static inline void obj_set_clickable(lv_obj_t *o, bool en)
{
	if (en) {
		lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	} else {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	}
}


#define TOPBAR_H 32
#define TOOLBAR_H 38
#define PAGE_PAD 10
#define TOOLBAR_HIDE_MS 3000
#define MAX_PAGES_CACHED 512

/* Acrobat-like palette */
#define COL_BAR lv_color_hex(0x323639)
#define COL_WORKSPACE lv_color_hex(0x525659)
#define COL_TOOLBAR lv_color_hex(0x2b2d2f)
#define COL_ACCENT lv_color_hex(0x1473e6)

static struct {
	char *path[CONFIG_ZPDF_APP_MAX_FILES];
	char *name[CONFIG_ZPDF_APP_MAX_FILES];
	int n;
} files;

static struct {
	struct pdf_doc *doc;
	uint8_t *buf;
	size_t len;
	size_t page;
	size_t npages;
	int file_index;
	bool page_mode;
	int zoom_pct; /* 100 = fit to width */
	bool fit_page;
	float page_w[MAX_PAGES_CACHED], page_h[MAX_PAGES_CACHED]; /* points */
	/* current page geometry at the current zoom */
	float scale;
	int px_w, px_h;
	/* rendered tile */
	int tile_x, tile_y, tile_w, tile_h;
	bool have_tile;
	bool waiting;
	int want_zoom; /* zoom of the pending request */
} cur;

static lv_obj_t *scr_files, *scr_reader;
static lv_obj_t *lbl_status, *list;
static lv_obj_t *topbar, *lbl_title, *btn_mode, *lbl_mode;
static lv_obj_t *page_view, *canvas, *page_img, *body, *lbl_text, *lbl_loading;
static lv_obj_t *toolbar, *lbl_page, *lbl_zoom, *btn_prev, *btn_next, *btn_zin, *btn_zout, *btn_fit;
static lv_timer_t *hide_timer;
static lv_image_dsc_t page_dsc;
static uint16_t *page_px;
static int font_index = 1;
static const lv_font_t *const fonts[] = {&lv_font_montserrat_12, &lv_font_montserrat_14,
					 &lv_font_montserrat_16, &lv_font_montserrat_18};
static const int zoom_levels[] = {50, 75, 100, 125, 150, 200, 300};

static atomic_t req_next, req_prev;

/* ---------------------------------------------------------------------- */
/* text for the built in font                                             */

/*
 * The Montserrat fonts built into LVGL cover ASCII, the degree sign and the
 * bullet. Everything else is approximated so the page stays readable.
 */
static int map_codepoint(uint32_t cp, char *out)
{
	static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

	if (cp == '\t') {
		out[0] = ' ';
		return 1;
	}
	if (cp == '\n') {
		out[0] = '\n';
		return 1;
	}
	if (cp < 0x20 || cp == 0x7F || cp == 0xAD || cp == 0x200B || cp == 0xFEFF) {
		return 0;
	}
	if (cp < 0x7F) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp == 0xB0 || cp == 0x2022) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		if (cp == 0x2022) {
			out[0] = (char)0xE2;
			out[1] = (char)0x80;
			out[2] = (char)0xA2;
			return 3;
		}
		return 2;
	}
	if (cp >= 0xC0 && cp <= 0xFF) {
		if (cp == 0xC6) {
			memcpy(out, "AE", 2);
			return 2;
		}
		if (cp == 0xE6) {
			memcpy(out, "ae", 2);
			return 2;
		}
		if (cp == 0xDF) {
			memcpy(out, "ss", 2);
			return 2;
		}
		out[0] = latin1[cp - 0xC0];
		return 1;
	}
	/* Latin Extended-A: strip the accent by the row of the letter */
	if (cp >= 0x100 && cp <= 0x17F) {
		static const char ext_a[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLl"
					    "NnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

		out[0] = ext_a[cp - 0x100];
		return 1;
	}
	const char *rep = NULL;

	switch (cp) {
	case 0xA0:
	case 0x2002:
	case 0x2003:
	case 0x2009:
		rep = " ";
		break;
	case 0xA9:
		rep = "(c)";
		break;
	case 0xAE:
		rep = "(R)";
		break;
	case 0x2122:
		rep = "TM";
		break;
	case 0xB7:
	case 0x2219:
	case 0x25CF:
	case 0x25A0:
		rep = "*";
		break;
	case 0xB5:
		rep = "u";
		break;
	case 0xD7:
		rep = "x";
		break;
	case 0xF7:
		rep = "/";
		break;
	case 0xB1:
		rep = "+/-";
		break;
	case 0xB2:
		rep = "2";
		break;
	case 0xB3:
		rep = "3";
		break;
	case 0xB9:
		rep = "1";
		break;
	case 0xBC:
		rep = "1/4";
		break;
	case 0xBD:
		rep = "1/2";
		break;
	case 0xBE:
		rep = "3/4";
		break;
	case 0xA3:
		rep = "GBP";
		break;
	case 0x20AC:
		rep = "EUR";
		break;
	case 0x2018:
	case 0x2019:
	case 0x201A:
	case 0x2032:
		rep = "'";
		break;
	case 0x201C:
	case 0x201D:
	case 0x201E:
	case 0x2033:
		rep = "\"";
		break;
	case 0x2010:
	case 0x2011:
	case 0x2012:
	case 0x2013:
	case 0x2212:
		rep = "-";
		break;
	case 0x2014:
	case 0x2015:
		rep = "--";
		break;
	case 0x2026:
		rep = "...";
		break;
	case 0x2192:
		rep = "->";
		break;
	case 0x2190:
		rep = "<-";
		break;
	case 0x2194:
		rep = "<->";
		break;
	case 0x2264:
		rep = "<=";
		break;
	case 0x2265:
		rep = ">=";
		break;
	case 0x2260:
		rep = "!=";
		break;
	case 0x2248:
		rep = "~";
		break;
	case 0x2030:
		rep = "%o";
		break;
	case 0x2713:
	case 0x2714:
		rep = "v";
		break;
	case 0xFB00:
		rep = "ff";
		break;
	case 0xFB01:
		rep = "fi";
		break;
	case 0xFB02:
		rep = "fl";
		break;
	case 0xFB03:
		rep = "ffi";
		break;
	case 0xFB04:
		rep = "ffl";
		break;
	case 0x3A9:
	case 0x2126:
		rep = "Ohm";
		break;
	case 0x3BC:
		rep = "u";
		break;
	case 0x3C0:
		rep = "pi";
		break;
	case 0x394:
	case 0x2206:
		rep = "d";
		break;
	default:
		rep = "?";
		break;
	}
	size_t l = strlen(rep);

	memcpy(out, rep, l);
	return (int)l;
}

static int utf8_get(const char *s, size_t n, uint32_t *cp)
{
	uint8_t c = (uint8_t)s[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if ((c & 0xE0) == 0xC0 && n >= 2) {
		*cp = ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
		return 2;
	}
	if ((c & 0xF0) == 0xE0 && n >= 3) {
		*cp = ((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
		return 3;
	}
	if ((c & 0xF8) == 0xF0 && n >= 4) {
		*cp = ((c & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) |
		      (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
		return 4;
	}
	*cp = 0xFFFD;
	return 1;
}

static bool is_lower(char c)
{
	return c >= 'a' && c <= 'z';
}

static bool is_alnum(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/*
 * Lines of a paragraph are joined again so the text wraps to the screen:
 * a line that ends in a letter, digit or comma continues on the next line
 * when that one starts with a lower case letter. A trailing hyphen before
 * such a continuation is a hyphenation and is dropped.
 */
static void reflow(char *s)
{
	size_t n = strlen(s), w = 0;

	for (size_t i = 0; i < n; i++) {
		if (s[i] != '\n' || i == 0 || i + 1 >= n) {
			s[w++] = s[i];
			continue;
		}
		char prev = s[w - 1];
		char next = s[i + 1];

		if ((is_alnum(prev) || prev == ',') && is_lower(next)) {
			s[w++] = ' ';
		} else if (prev == '-' && w >= 2 && is_alnum(s[w - 2]) && is_lower(next)) {
			w--; /* hyphenation */
		} else {
			s[w++] = '\n';
		}
	}
	s[w] = 0;
}

/* Returns a k_malloc'd copy with only glyphs the built in font has. */
static char *sanitize(const char *text)
{
	size_t n = strlen(text);
	char *out = k_malloc(n * 3 + 1);
	size_t o = 0;

	if (out == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < n;) {
		uint32_t cp;
		int l = utf8_get(text + i, n - i, &cp);

		i += (size_t)l;
		o += (size_t)map_codepoint(cp, out + o);
	}
	out[o] = 0;
	reflow(out);
	return out;
}

/* ---------------------------------------------------------------------- */
/* loading indicator and toolbar visibility                               */

static void loading_show(const char *msg)
{
	lv_label_set_text(lbl_loading, msg);
	obj_set_hidden(lbl_loading, false);
}

static void loading_hide(void)
{
	obj_set_hidden(lbl_loading, true);
}

static void toolbar_hide_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	obj_set_hidden(toolbar, true);
}

/* Show the toolbar and restart its hide timer. */
static void toolbar_show(void)
{
	obj_set_hidden(toolbar, false);
	if (hide_timer == NULL) {
		hide_timer = lv_timer_create(toolbar_hide_cb, TOOLBAR_HIDE_MS, NULL);
	}
	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

static void toolbar_toggle(void)
{
	if (obj_is_hidden(toolbar)) {
		toolbar_show();
	} else {
		obj_set_hidden(toolbar, true);
	}
}

/* ---------------------------------------------------------------------- */
/* page geometry                                                          */

static int view_w(void)
{
	return lv_obj_get_width(page_view);
}

static int view_h(void)
{
	return lv_obj_get_height(page_view);
}

/* Scale for the current zoom: 100 % fits the page width to the view. */
static void update_geometry(void)
{
	float pw = cur.page < MAX_PAGES_CACHED ? cur.page_w[cur.page] : 612.0f;
	float ph = cur.page < MAX_PAGES_CACHED ? cur.page_h[cur.page] : 792.0f;
	float fit_w = (float)(view_w() - 2 * PAGE_PAD) / pw;

	if (cur.fit_page) {
		float fit_h = (float)(view_h() - 2 * PAGE_PAD) / ph;

		cur.scale = fit_h < fit_w ? fit_h : fit_w;
	} else {
		cur.scale = fit_w * (float)cur.zoom_pct / 100.0f;
	}
	cur.px_w = (int)(pw * cur.scale + 0.5f);
	cur.px_h = (int)(ph * cur.scale + 0.5f);
	if (cur.px_w < 1) {
		cur.px_w = 1;
	}
	if (cur.px_h < 1) {
		cur.px_h = 1;
	}
	/* the canvas is the page; a narrow page is centred by the padding of the view */
	lv_obj_set_size(canvas, cur.px_w, cur.px_h);
	int cw = cur.px_w + 2 * PAGE_PAD, left = cw < view_w() ? (view_w() - cur.px_w) / 2 : PAGE_PAD;
	int ch = cur.px_h + 2 * PAGE_PAD, top = ch < view_h() ? (view_h() - cur.px_h) / 2 : PAGE_PAD;

	lv_obj_set_style_pad_left(page_view, left, 0);
	lv_obj_set_style_pad_right(page_view, PAGE_PAD, 0);
	lv_obj_set_style_pad_top(page_view, top, 0);
	lv_obj_set_style_pad_bottom(page_view, PAGE_PAD, 0);
	lv_obj_set_pos(canvas, 0, 0);
}

static void release_tile(void)
{
	if (page_px != NULL) {
		lv_image_set_src(page_img, NULL);
		storage_free((uint8_t *)page_px);
		page_px = NULL;
	}
	cur.have_tile = false;
	obj_set_hidden(page_img, true);
}

/* Visible rectangle of the page in page pixels. */
static void visible_rect(int *x, int *y, int *w, int *h)
{
	int sx = lv_obj_get_scroll_x(page_view) - lv_obj_get_style_pad_left(page_view, 0);
	int sy = lv_obj_get_scroll_y(page_view) - lv_obj_get_style_pad_top(page_view, 0);

	*x = sx < 0 ? 0 : sx;
	*y = sy < 0 ? 0 : sy;
	*w = view_w();
	*h = view_h();
}

/* Ask the worker for the tile around the visible area (twice its size). */
static void request_tile(void)
{
	char info[48];
	int vx, vy, vw, vh;

	if (cur.doc == NULL) {
		return;
	}
	visible_rect(&vx, &vy, &vw, &vh);
	int tw = vw * 2, th = vh * 2;
	int tx = vx - vw / 2, ty = vy - vh / 2;

	if (tw > cur.px_w) {
		tw = cur.px_w;
	}
	if (th > cur.px_h) {
		th = cur.px_h;
	}
	if (tx < 0) {
		tx = 0;
	}
	if (ty < 0) {
		ty = 0;
	}
	if (tx + tw > cur.px_w) {
		tx = cur.px_w - tw;
	}
	if (ty + th > cur.px_h) {
		ty = cur.px_h - th;
	}
	if (tw < 1 || th < 1 || tx < 0 || ty < 0) {
		return;
	}
	cur.waiting = true;
	cur.want_zoom = cur.fit_page ? -1 : cur.zoom_pct;
	/* at high zoom the previous tile makes room for the new one */
	if (cur.zoom_pct >= 200 && page_px != NULL) {
		release_tile();
	}
	snprintf(info, sizeof(info), "Rendering page %u ...", (unsigned)cur.page + 1);
	loading_show(info);
	worker_request_render(cur.doc, cur.page, cur.scale, cur.want_zoom, tx, ty, tw, th);
}

/* Is the visible area still inside the rendered tile (with a margin)? */
static bool tile_covers_view(void)
{
	int vx, vy, vw, vh;

	if (!cur.have_tile) {
		return false;
	}
	visible_rect(&vx, &vy, &vw, &vh);
	int margin_x = vw / 4, margin_y = vh / 4;
	int need_x0 = vx - margin_x, need_y0 = vy - margin_y;
	int need_x1 = vx + vw + margin_x, need_y1 = vy + vh + margin_y;

	if (need_x0 < 0) {
		need_x0 = 0;
	}
	if (need_y0 < 0) {
		need_y0 = 0;
	}
	if (need_x1 > cur.px_w) {
		need_x1 = cur.px_w;
	}
	if (need_y1 > cur.px_h) {
		need_y1 = cur.px_h;
	}
	return need_x0 >= cur.tile_x && need_y0 >= cur.tile_y && need_x1 <= cur.tile_x + cur.tile_w &&
	       need_y1 <= cur.tile_y + cur.tile_h;
}

static void update_nav(void)
{
	char info[32];

	snprintf(info, sizeof(info), "%u / %u", (unsigned)cur.page + 1, (unsigned)cur.npages);
	lv_label_set_text(lbl_page, info);
	if (cur.page_mode) {
		if (cur.fit_page) {
			lv_label_set_text(lbl_zoom, "Fit");
		} else {
			lv_label_set_text_fmt(lbl_zoom, "%d%%", cur.zoom_pct);
		}
	} else {
		lv_label_set_text_fmt(lbl_zoom, "A%d", (int)lv_font_get_line_height(fonts[font_index]));
	}
	if (cur.page == 0) {
		lv_obj_add_state(btn_prev, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(btn_prev, LV_STATE_DISABLED);
	}
	if (cur.page + 1 >= cur.npages) {
		lv_obj_add_state(btn_next, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(btn_next, LV_STATE_DISABLED);
	}
	lv_label_set_text(lbl_mode, cur.page_mode ? "Aa" : LV_SYMBOL_IMAGE);
	obj_set_hidden(btn_fit, !cur.page_mode);
	obj_set_hidden(page_view, !cur.page_mode);
	obj_set_hidden(body, cur.page_mode);
}

static void request_page(void)
{
	char info[40];

	if (cur.doc == NULL) {
		return;
	}
	if (cur.page_mode) {
		update_geometry();
		request_tile();
		return;
	}
	cur.waiting = true;
	snprintf(info, sizeof(info), "Page %u ...", (unsigned)cur.page + 1);
	loading_show(info);
	worker_request_text(cur.doc, cur.page);
}

static void show_page(size_t index)
{
	if (cur.doc == NULL || index >= cur.npages) {
		return;
	}
	cur.page = index;
	release_tile();
	update_nav();
	update_geometry();
	lv_obj_scroll_to(page_view, 0, 0, LV_ANIM_OFF);
	request_page();
	toolbar_show();
}

/* ---------------------------------------------------------------------- */
/* results from the worker                                                */

static void deliver(struct worker_result *r)
{
	bool wanted = cur.doc != NULL && r->page == cur.page && ((r->kind == WORKER_RENDER) == cur.page_mode);

	if (wanted && r->kind == WORKER_RENDER) {
		wanted = r->zoom_pct == (cur.fit_page ? -1 : cur.zoom_pct);
	}
	if (!wanted) {
		storage_free((uint8_t *)r->px);
		pdf_text_free(r->text);
		return;
	}
	cur.waiting = false;
	loading_hide();
	if (r->kind == WORKER_TEXT) {
		if (r->error < 0) {
			lv_label_set_text(lbl_text, r->error == -13
							    ? "This document is encrypted; its text cannot be shown."
							    : "This page could not be read.");
		} else if (r->text == NULL || r->text[0] == 0) {
			lv_label_set_text(lbl_text, "(no text on this page)");
		} else {
			char *clean = sanitize(r->text);

			lv_label_set_text(lbl_text, clean ? clean : r->text);
			k_free(clean);
		}
		pdf_text_free(r->text);
		lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
		return;
	}
	if (r->error < 0 || r->px == NULL) {
		storage_free((uint8_t *)r->px);
		if (r->error != -13 && (cur.zoom_pct != 100 || cur.fit_page)) {
			LOG_WRN("render failed (%d) at %d%% zoom, back to 100%%", r->error, cur.zoom_pct);
			cur.zoom_pct = 100;
			cur.fit_page = false;
			update_nav();
			request_page();
			return;
		}
		lv_label_set_text(lbl_text, r->error == -13 ? "This document is encrypted; it cannot be rendered."
							    : "This page could not be rendered.");
		cur.page_mode = false;
		update_nav();
		return;
	}
	release_tile();
	page_px = r->px;
	memset(&page_dsc, 0, sizeof(page_dsc));
	page_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	page_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	page_dsc.header.w = (uint32_t)r->w;
	page_dsc.header.h = (uint32_t)r->h;
	page_dsc.header.stride = (uint32_t)r->w * 2;
	page_dsc.data_size = (uint32_t)r->w * (uint32_t)r->h * 2;
	page_dsc.data = (const uint8_t *)page_px;
	lv_image_set_src(page_img, &page_dsc);
	lv_obj_set_size(page_img, r->w, r->h);
	lv_obj_set_pos(page_img, r->x, r->y);
	obj_set_hidden(page_img, false);
	cur.tile_x = r->x;
	cur.tile_y = r->y;
	cur.tile_w = r->w;
	cur.tile_h = r->h;
	cur.have_tile = true;
	LOG_INF("page %u tile %dx%d at %d,%d (%d%%) in %u ms", (unsigned)r->page + 1, r->w, r->h, r->x, r->y,
		r->zoom_pct, r->ms);
	/* the view may have moved on while rendering */
	if (!tile_covers_view()) {
		request_tile();
	}
}

/* ---------------------------------------------------------------------- */
/* document                                                               */

static void close_document(void)
{
	worker_drain();
	release_tile();
	if (cur.doc != NULL) {
		pdf_close(cur.doc);
		cur.doc = NULL;
	}
	if (cur.buf != NULL) {
		storage_free(cur.buf);
		cur.buf = NULL;
	}
	cur.npages = 0;
	cur.page = 0;
	cur.have_tile = false;
}

static void open_document(int file_index)
{
	char err[48];

	close_document();
	cur.file_index = file_index;
	cur.page_mode = true;
	cur.zoom_pct = 100;
	cur.fit_page = false;
	lv_label_set_text(lbl_title, files.name[file_index]);
	lv_label_set_text(lbl_page, "");
	lv_label_set_text(lbl_text, "");
	lv_screen_load(scr_reader);
	loading_show("Opening ...");
	lv_refr_now(NULL);

	int64_t t0 = k_uptime_get();
	int rc = storage_read(files.path[file_index], &cur.buf, &cur.len);

	if (rc != 0) {
		loading_hide();
		lv_label_set_text_fmt(lbl_text, "Cannot read the file (%d).%s", rc,
				      rc == -EFBIG ? " It is larger than the configured limit." : "");
		cur.page_mode = false;
		update_nav();
		return;
	}
	cur.doc = pdf_open(cur.buf, cur.len, err, sizeof(err));
	if (cur.doc == NULL) {
		loading_hide();
		lv_label_set_text_fmt(lbl_text, "Cannot open this PDF: %s.", err);
		storage_free(cur.buf);
		cur.buf = NULL;
		cur.page_mode = false;
		update_nav();
		return;
	}
	cur.npages = pdf_page_count(cur.doc);
	/* page sizes are read now, while the worker is idle */
	for (size_t i = 0; i < cur.npages && i < MAX_PAGES_CACHED; i++) {
		if (pdf_page_size(cur.doc, i, &cur.page_w[i], &cur.page_h[i]) < 0 || cur.page_w[i] < 1) {
			cur.page_w[i] = 612;
			cur.page_h[i] = 792;
		}
	}
	LOG_INF("opened %s: %u pages, %u bytes, %lld ms", files.name[file_index], (unsigned)cur.npages,
		(unsigned)cur.len, k_uptime_get() - t0);

	char *title = pdf_info_string(cur.doc, "Title");

	if (title != NULL && title[0] != 0) {
		char *clean = sanitize(title);

		if (clean != NULL && clean[0] != 0) {
			lv_label_set_text(lbl_title, clean);
		}
		k_free(clean);
	}
	pdf_text_free(title);
	show_page(0);
}

static void next_page(void)
{
	if (cur.doc != NULL && cur.page + 1 < cur.npages) {
		show_page(cur.page + 1);
	}
}

static void prev_page(void)
{
	if (cur.doc != NULL && cur.page > 0) {
		show_page(cur.page - 1);
	}
}

/* Change zoom keeping the point at the centre of the view in place. */
static void set_zoom(int pct, bool fit)
{
	int vx, vy, vw, vh;

	visible_rect(&vx, &vy, &vw, &vh);
	float cx = (float)(vx + vw / 2) / (float)cur.px_w;
	float cy = (float)(vy + vh / 2) / (float)cur.px_h;

	cur.zoom_pct = pct;
	cur.fit_page = fit;
	release_tile();
	update_nav();
	update_geometry();
	int nx = (int)(cx * cur.px_w) - vw / 2 + lv_obj_get_style_pad_left(page_view, 0);
	int ny = (int)(cy * cur.px_h) - vh / 2 + lv_obj_get_style_pad_top(page_view, 0);

	lv_obj_scroll_to(page_view, nx < 0 ? 0 : nx, ny < 0 ? 0 : ny, LV_ANIM_OFF);
	request_tile();
	toolbar_show();
}

static void zoom_step(int dir)
{
	if (cur.doc == NULL) {
		return;
	}
	if (!cur.page_mode) {
		/* text view: the buttons change the text size */
		int n = (int)ARRAY_SIZE(fonts);

		font_index = (font_index + dir + n) % n;
		lv_obj_set_style_text_font(lbl_text, fonts[font_index], 0);
		update_nav();
		toolbar_show();
		return;
	}
	int n = (int)ARRAY_SIZE(zoom_levels);
	int i = 0;

	if (cur.fit_page) {
		/* fit page is below 100 %: step from there */
		i = dir > 0 ? 2 : 0;
		set_zoom(zoom_levels[i], false);
		return;
	}
	for (; i < n; i++) {
		if (zoom_levels[i] >= cur.zoom_pct) {
			break;
		}
	}
	int j = i + dir;

	if (j < 0 || j >= n || zoom_levels[j] > CONFIG_ZPDF_APP_MAX_ZOOM_PCT) {
		toolbar_show();
		return;
	}
	set_zoom(zoom_levels[j], false);
}

/* ---------------------------------------------------------------------- */
/* events                                                                 */

static void on_prev(lv_event_t *e)
{
	ARG_UNUSED(e);
	prev_page();
}

static void on_next(lv_event_t *e)
{
	ARG_UNUSED(e);
	next_page();
}

static void on_back(lv_event_t *e)
{
	ARG_UNUSED(e);
	close_document();
	lv_screen_load(scr_files);
}

static void on_zoom_in(lv_event_t *e)
{
	ARG_UNUSED(e);
	zoom_step(1);
}

static void on_zoom_out(lv_event_t *e)
{
	ARG_UNUSED(e);
	zoom_step(-1);
}

static void on_fit(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (cur.doc == NULL || !cur.page_mode) {
		return;
	}
	/* toggle between fit page and fit width */
	if (cur.fit_page) {
		set_zoom(100, false);
	} else {
		set_zoom(cur.zoom_pct, true);
	}
}

static void on_mode(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (cur.doc == NULL) {
		return;
	}
	cur.page_mode = !cur.page_mode;
	release_tile();
	update_nav();
	request_page();
	toolbar_show();
}

static void on_page_click(lv_event_t *e)
{
	ARG_UNUSED(e);
	toolbar_toggle();
}

static void on_scroll_end(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (cur.page_mode && !cur.waiting && !tile_covers_view()) {
		request_tile();
	}
}

static void on_gesture(lv_event_t *e)
{
	lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

	ARG_UNUSED(e);
	if (dir == LV_DIR_LEFT) {
		next_page();
	} else if (dir == LV_DIR_RIGHT) {
		prev_page();
	}
}

/* ---------------------------------------------------------------------- */
/* widgets                                                                */

static lv_obj_t *make_tool(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int width)
{
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_t *lbl = lv_label_create(btn);

	lv_label_set_text(lbl, text);
	lv_obj_center(lbl);
	lv_obj_set_size(btn, width, TOOLBAR_H - 8);
	lv_obj_set_style_radius(btn, 6, 0);
	lv_obj_set_style_bg_color(btn, COL_TOOLBAR, 0);
	lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_set_style_text_color(btn, lv_color_white(), 0);
	lv_obj_set_style_text_color(btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DISABLED);
	lv_obj_set_style_bg_color(btn, COL_TOOLBAR, LV_STATE_DISABLED);
	lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	return btn;
}

static lv_obj_t *make_separator(lv_obj_t *parent)
{
	lv_obj_t *sep = lv_obj_create(parent);

	lv_obj_set_size(sep, 1, TOOLBAR_H - 14);
	lv_obj_set_style_bg_color(sep, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
	lv_obj_set_style_border_width(sep, 0, 0);
	lv_obj_set_style_radius(sep, 0, 0);
	return sep;
}

static lv_obj_t *make_readout(lv_obj_t *parent, int width)
{
	lv_obj_t *lbl = lv_label_create(parent);

	lv_obj_set_width(lbl, width);
	lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
	return lbl;
}

static void build_reader(void)
{
	scr_reader = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_reader, COL_WORKSPACE, 0);

	int32_t w = lv_display_get_horizontal_resolution(NULL);
	int32_t h = lv_display_get_vertical_resolution(NULL);

	/* title bar */
	topbar = lv_obj_create(scr_reader);
	lv_obj_set_size(topbar, w, TOPBAR_H);
	lv_obj_set_pos(topbar, 0, 0);
	lv_obj_set_style_radius(topbar, 0, 0);
	lv_obj_set_style_border_width(topbar, 0, 0);
	lv_obj_set_style_pad_all(topbar, 2, 0);
	lv_obj_set_style_pad_column(topbar, 6, 0);
	lv_obj_set_style_bg_color(topbar, COL_BAR, 0);
	lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	obj_set_scrollable(topbar, false);

	lv_obj_t *back = make_tool(topbar, LV_SYMBOL_LEFT, on_back, 40);

	lv_obj_set_style_bg_color(back, COL_BAR, 0);
	lbl_title = lv_label_create(topbar);
	lv_obj_set_size(lbl_title, w - 40 - 44 - 4 * 6, lv_font_get_line_height(&lv_font_montserrat_14) + 2);
	lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
	lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
	btn_mode = make_tool(topbar, "Aa", on_mode, 44);
	lv_obj_set_style_bg_color(btn_mode, COL_BAR, 0);
	lbl_mode = lv_obj_get_child(btn_mode, 0);

	/* workspace with the page */
	page_view = lv_obj_create(scr_reader);
	lv_obj_set_size(page_view, w, h - TOPBAR_H);
	lv_obj_set_pos(page_view, 0, TOPBAR_H);
	lv_obj_set_style_radius(page_view, 0, 0);
	lv_obj_set_style_border_width(page_view, 0, 0);
	lv_obj_set_style_pad_all(page_view, PAGE_PAD, 0);
	lv_obj_set_style_bg_color(page_view, COL_WORKSPACE, 0);
	lv_obj_set_scroll_dir(page_view, LV_DIR_ALL);
	lv_obj_set_scrollbar_mode(page_view, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_add_event_cb(page_view, on_scroll_end, LV_EVENT_SCROLL_END, NULL);
	lv_obj_add_event_cb(page_view, on_page_click, LV_EVENT_CLICKED, NULL);

	canvas = lv_obj_create(page_view);
	lv_obj_set_style_radius(canvas, 0, 0);
	lv_obj_set_style_border_width(canvas, 0, 0);
	lv_obj_set_style_pad_all(canvas, 0, 0);
	lv_obj_set_style_bg_color(canvas, lv_color_hex(0xe8e8e8), 0);
	lv_obj_set_style_shadow_width(canvas, 16, 0);
	lv_obj_set_style_shadow_opa(canvas, LV_OPA_50, 0);
	lv_obj_set_style_shadow_color(canvas, lv_color_black(), 0);
	obj_set_scrollable(canvas, false);
	lv_obj_add_event_cb(canvas, on_page_click, LV_EVENT_CLICKED, NULL);

	page_img = lv_image_create(canvas);
	lv_obj_set_pos(page_img, 0, 0);
	lv_obj_add_event_cb(page_img, on_page_click, LV_EVENT_CLICKED, NULL);
	obj_set_clickable(page_img, true);

	/* text view */
	body = lv_obj_create(scr_reader);
	lv_obj_set_size(body, w, h - TOPBAR_H);
	lv_obj_set_pos(body, 0, TOPBAR_H);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_hor(body, 12, 0);
	lv_obj_set_style_pad_ver(body, 8, 0);
	lv_obj_set_style_bg_color(body, lv_color_white(), 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(body, on_gesture, LV_EVENT_GESTURE, NULL);
	lv_obj_add_event_cb(body, on_page_click, LV_EVENT_CLICKED, NULL);

	lbl_text = lv_label_create(body);
	lv_obj_set_width(lbl_text, lv_pct(100));
	lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_color(lbl_text, lv_color_black(), 0);
	lv_obj_set_style_text_line_space(lbl_text, 3, 0);
	lv_obj_set_style_text_font(lbl_text, fonts[font_index], 0);
	lv_obj_add_event_cb(lbl_text, on_gesture, LV_EVENT_GESTURE, NULL);

	/* floating toolbar */
	toolbar = lv_obj_create(scr_reader);
	lv_obj_set_size(toolbar, LV_SIZE_CONTENT, TOOLBAR_H);
	lv_obj_align(toolbar, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_style_radius(toolbar, 10, 0);
	lv_obj_set_style_border_width(toolbar, 0, 0);
	lv_obj_set_style_pad_ver(toolbar, 4, 0);
	lv_obj_set_style_pad_hor(toolbar, 8, 0);
	lv_obj_set_style_pad_column(toolbar, 4, 0);
	lv_obj_set_style_bg_color(toolbar, COL_TOOLBAR, 0);
	lv_obj_set_style_bg_opa(toolbar, LV_OPA_90, 0);
	lv_obj_set_style_shadow_width(toolbar, 12, 0);
	lv_obj_set_style_shadow_opa(toolbar, LV_OPA_40, 0);
	lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	obj_set_scrollable(toolbar, false);

	btn_prev = make_tool(toolbar, LV_SYMBOL_LEFT, on_prev, 36);
	lbl_page = make_readout(toolbar, 64);
	btn_next = make_tool(toolbar, LV_SYMBOL_RIGHT, on_next, 36);
	make_separator(toolbar);
	btn_zout = make_tool(toolbar, LV_SYMBOL_MINUS, on_zoom_out, 36);
	lbl_zoom = make_readout(toolbar, 52);
	btn_zin = make_tool(toolbar, LV_SYMBOL_PLUS, on_zoom_in, 36);
	make_separator(toolbar);
	btn_fit = make_tool(toolbar, LV_SYMBOL_IMAGE, on_fit, 36);

	/* loading indicator on the top layer, shared by both screens */
	lbl_loading = lv_label_create(lv_layer_top());
	lv_obj_set_style_bg_opa(lbl_loading, LV_OPA_90, 0);
	lv_obj_set_style_bg_color(lbl_loading, COL_BAR, 0);
	lv_obj_set_style_text_color(lbl_loading, lv_color_white(), 0);
	lv_obj_set_style_pad_all(lbl_loading, 10, 0);
	lv_obj_set_style_radius(lbl_loading, 8, 0);
	lv_obj_align(lbl_loading, LV_ALIGN_TOP_MID, 0, TOPBAR_H + 8);
	obj_set_hidden(lbl_loading, true);
}

/* ---------------------------------------------------------------------- */
/* document list                                                          */

static void on_file(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (idx >= 0 && idx < files.n) {
		open_document(idx);
	}
}

static void list_add(const char *path, const char *name, void *user_data)
{
	ARG_UNUSED(user_data);
	if (files.n >= CONFIG_ZPDF_APP_MAX_FILES) {
		return;
	}
	size_t pl = strlen(path), nl = strlen(name);
	char *p = k_malloc(pl + 1), *n = k_malloc(nl + 1);

	if (p == NULL || n == NULL) {
		k_free(p);
		k_free(n);
		return;
	}
	memcpy(p, path, pl + 1);
	memcpy(n, name, nl + 1);
	files.path[files.n] = p;
	files.name[files.n] = n;

	lv_obj_t *btn = lv_button_create(list);
	lv_obj_t *lbl = lv_label_create(btn);

	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_set_style_radius(btn, 0, 0);
	lv_obj_set_style_pad_ver(btn, 10, 0);
	lv_obj_set_style_pad_hor(btn, 12, 0);
	lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
	lv_obj_set_style_bg_color(btn, lv_color_hex(0xe3eefb), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(btn, 1, 0);
	lv_obj_set_style_border_color(btn, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
	lv_label_set_text_fmt(lbl, LV_SYMBOL_FILE "  %s", n);
	lv_obj_set_size(lbl, lv_pct(100), lv_font_get_line_height(&lv_font_montserrat_16) + 2);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
	lv_obj_add_event_cb(btn, on_file, LV_EVENT_CLICKED, (void *)(intptr_t)files.n);
	files.n++;
}

static void refresh_list(void)
{
	for (int i = 0; i < files.n; i++) {
		k_free(files.path[i]);
		k_free(files.name[i]);
	}
	files.n = 0;
	lv_obj_clean(list);

	int rc = storage_list(list_add, NULL);

	if (rc < 0) {
		lv_label_set_text_fmt(lbl_status, "%s", storage_describe());
	} else if (rc == 0) {
		lv_label_set_text_fmt(lbl_status, "%s - no PDF files", storage_describe());
	} else {
		lv_label_set_text_fmt(lbl_status, "%s - %d document%s", storage_describe(), rc, rc == 1 ? "" : "s");
	}
}

static void on_refresh(lv_event_t *e)
{
	ARG_UNUSED(e);
	loading_show("Reading storage ...");
	storage_init(); /* a card inserted after boot gets mounted now */
	refresh_list();
	loading_hide();
}

static void build_files(void)
{
	scr_files = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_files, lv_color_white(), 0);

	int32_t w = lv_display_get_horizontal_resolution(NULL);
	int32_t h = lv_display_get_vertical_resolution(NULL);

	lv_obj_t *hdr = lv_obj_create(scr_files);

	lv_obj_set_size(hdr, w, TOPBAR_H);
	lv_obj_set_pos(hdr, 0, 0);
	lv_obj_set_style_radius(hdr, 0, 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_pad_all(hdr, 2, 0);
	lv_obj_set_style_pad_hor(hdr, 10, 0);
	lv_obj_set_style_pad_column(hdr, 10, 0);
	lv_obj_set_style_bg_color(hdr, COL_BAR, 0);
	lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	obj_set_scrollable(hdr, false);

	lv_obj_t *title = lv_label_create(hdr);

	lv_label_set_text(title, "Files");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(title, lv_color_white(), 0);

	lbl_status = lv_label_create(hdr);
	lv_obj_set_size(lbl_status, w - 60 - 40 - 40, lv_font_get_line_height(&lv_font_montserrat_12) + 2);
	lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_color(lbl_status, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);

	lv_obj_t *refresh = make_tool(hdr, LV_SYMBOL_REFRESH, on_refresh, 40);

	lv_obj_set_style_bg_color(refresh, COL_BAR, 0);

	list = lv_obj_create(scr_files);
	lv_obj_set_size(list, w, h - TOPBAR_H);
	lv_obj_set_pos(list, 0, TOPBAR_H);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_row(list, 0, 0);
	lv_obj_set_style_bg_color(list, lv_color_white(), 0);
	lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
}

/* ---------------------------------------------------------------------- */

void ui_init(void)
{
	build_files();
	build_reader();
	lv_screen_load(scr_files);
	refresh_list();
	if (storage_auto_open() && files.n > 0) {
		open_document(0);
	}
}

void ui_request_next_page(void)
{
	atomic_set(&req_next, 1);
}

void ui_set_zoom(int pct)
{
	if (cur.doc != NULL && cur.page_mode) {
		set_zoom(pct > 0 ? pct : cur.zoom_pct, pct <= 0);
	}
}

void ui_set_text_view(bool text)
{
	if (cur.doc != NULL && cur.page_mode == text) {
		on_mode(NULL);
	}
}

void ui_request_prev_page(void)
{
	atomic_set(&req_prev, 1);
}

void ui_poll(void)
{
	struct worker_result r;

	while (worker_take_result(&r)) {
		deliver(&r);
	}
	if (atomic_cas(&req_next, 1, 0) && lv_screen_active() == scr_reader) {
		next_page();
	}
	if (atomic_cas(&req_prev, 1, 0) && lv_screen_active() == scr_reader) {
		prev_page();
	}
}
