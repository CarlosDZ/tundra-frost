#include "win.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "kbd.h"
#include "line.h"
#include "links.h"
#include "mouse.h"
#include "paste.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "proto.h"
#include "render.h"
#include "select.h"
#include "shm.h"
#include "winint.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FONT_SIZE 24
#define BLINK_MS 500
#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define WHEEL_LINES 3
#define CURSOR_SIZE 24
#define CLICK_MS 400
#define PASTE_MAX (16u << 20)

struct win w;

#ifdef FROST_TEST
static void test_log(const char *what, long n) {
	const char *path = getenv("FROST_TEST_LOG");
	FILE *f = path ? fopen(path, "a") : NULL;
	if (f) {
		fprintf(f, "%s %ld\n", what, n);
		fclose(f);
	}
}
#else
#define test_log(what, n) ((void)0)
#endif

static void update_hover(void);

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
	(void)data, (void)version;
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		w.comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		w.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		w.wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	else if (strcmp(iface, wl_seat_interface.name) == 0 && !w.seat)
		w.seat = wl_registry_bind(reg, name, &wl_seat_interface,
		                          version < 8 ? version : 8);
	else if (strcmp(iface,
	                zwp_primary_selection_device_manager_v1_interface.name) ==
	         0)
		w.psm = wl_registry_bind(
		    reg, name, &zwp_primary_selection_device_manager_v1_interface, 1);
	else if (strcmp(iface, wl_data_device_manager_interface.name) == 0)
		w.ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
		                         version < 3 ? version : 3);
	else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0)
		w.deco_mgr = wl_registry_bind(reg, name,
		                              &zxdg_decoration_manager_v1_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *reg,
                            uint32_t name) {
	(void)data, (void)reg, (void)name;
}

static const struct wl_registry_listener registry_listener = {registry_global,
                                                              registry_remove};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t width,
                          int32_t height, struct wl_array *states) {
	(void)data, (void)t;
	w.next_w = width, w.next_h = height;
	w.next_focus = 0;
	uint32_t *s;
	wl_array_for_each(s, states) if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
	    w.next_focus = 1;
}

static void top_close(void *data, struct xdg_toplevel *t) {
	(void)data, (void)t;
	for (int i = 0; i < w.npanes; i++)
		if (!w.panes[i].closing && !w.panes[i].dead) {
			msg_send(w.panes[i].sock, MSG_CLOSE, NULL, 0, -1);
			w.panes[i].closing = 1;
		}
	for (int i = w.npanes - 1; i >= 0 && !w.ended; i--)
		if (w.panes[i].dead)
			pane_drop(i, w.panes[i].failed);
}

static void top_bounds(void *data, struct xdg_toplevel *t, int32_t width,
                       int32_t height) {
	(void)data, (void)t, (void)width, (void)height;
}

static void top_caps(void *data, struct xdg_toplevel *t,
                     struct wl_array *caps) {
	(void)data, (void)t, (void)caps;
}

static const struct xdg_toplevel_listener top_listener = {
    top_configure, top_close, top_bounds, top_caps};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);
	w.configured = 1;
}

static const struct xdg_surface_listener xs_listener = {xs_configure};

static void buffer_release(void *data, struct wl_buffer *b) {
	(void)b;
	((struct buffer *)data)->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {buffer_release};

static void free_buffers(void) {
	for (int i = 0; i < 2; i++) {
		struct buffer *b = &w.buf[i];
		if (b->wl)
			wl_buffer_destroy(b->wl);
		tdraw_canvas_free(b->cv);
		*b = (struct buffer){0};
	}
	w.shown = NULL;
	if (w.pool)
		munmap(w.pool, w.pool_size);
	w.pool = NULL;
}

static int make_buffers(int width, int height) {
	free_buffers();
	size_t one = (size_t)width * (size_t)height * 4;
	w.pool_size = 2 * one;
	int fd = memfd_create("frost-window", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)w.pool_size) < 0) {
		perror("memfd error");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	w.pool = mmap(NULL, w.pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (w.pool == MAP_FAILED) {
		perror("mmap error");
		close(fd);
		w.pool = NULL;
		return -1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(w.shm, fd, (int)w.pool_size);
	for (int i = 0; i < 2; i++) {
		struct buffer *b = &w.buf[i];
		uint32_t *px = (uint32_t *)((uint8_t *)w.pool + (size_t)i * one);
		b->wl = wl_shm_pool_create_buffer(pool, (int)((size_t)i * one), width,
		                                  height, width * 4,
		                                  WL_SHM_FORMAT_ARGB8888);
		wl_buffer_add_listener(b->wl, &buffer_listener, b);
		b->cv = tdraw_canvas_new(px, width, height, width * 4);
	}
	wl_shm_pool_destroy(pool);
	close(fd);
	w.bw = width, w.bh = height;
	for (int i = 0; i < 2; i++)
		if (!w.buf[i].cv)
			return -1;
	return 0;
}

static int view_of(const struct pane *t, struct view *v) {
	if (!t->map)
		return -1;
	const struct shm_head *h = (const struct shm_head *)t->map;
	if (h->magic != SHM_MAGIC || h->version != SHM_VERSION || !h->cols ||
	    !h->rows || t->map_size < shm_layout(h->cols, h->rows).size)
		return -1;
	struct shm_layout l = shm_layout(h->cols, h->rows);
	*v = (struct view){h->cols,
	                   h->rows,
	                   (const struct cell *)(t->map + l.cells),
	                   (const uint16_t *)(t->map + l.map),
	                   h->x,
	                   h->y,
	                   h->pending,
	                   h->cursor_visible,
	                   h->cursor_shape,
	                   h->cursor_blink,
	                   h->reverse,
	                   h->palette,
	                   h->palette_set,
	                   t->map + l.lines,
	                   0,
	                   0,
	                   0,
	                   0};
	if (t == &T && w.sel_on)
		select_span(v, w.sel_anchor, w.sel_head, w.sel_mode, &v->sel_from,
		            &v->sel_to);
	if (t == &T && w.link_kind)
		v->link_from = w.link_from, v->link_to = w.link_to;
	return 0;
}

static void keep_selection(struct view *v, const uint8_t *dirty) {
	const struct shm_head *h = (const struct shm_head *)T.map;
	if (!w.sel_on || w.selecting)
		return;
	int gone = h->offset != w.sel_offset;
	for (int y = v->sel_from / v->cols; y <= (v->sel_to - 1) / v->cols && !gone;
	     y++)
		gone = T.damage && y < v->rows && dirty[y];
	if (gone) {
		w.sel_on = 0;
		v->sel_from = v->sel_to = 0;
		w.full = 1;
	}
}

static void blink_arm(int on) {
	struct itimerspec t = {0};
	if (on)
		t.it_interval.tv_nsec = t.it_value.tv_nsec = BLINK_MS * 1000000L;
	timerfd_settime(w.blink, 0, &t, NULL);
	w.blinking = on;
	w.lit = 1;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = {frame_done};

static uint32_t theme_cell(const char *name, uint32_t fallback) {
	uint32_t c =
	    w.theme ? tdraw_theme_color(w.theme, name, fallback) : fallback;
	return COLOR_RGB(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF);
}

static void paint_line(struct buffer *b) {
	struct line_tab tabs[PANES_MAX];
	int cur = group_index(T.group);
	for (int g = 0; g < w.ngroups; g++) {
		int f = group_focus(g), marked = 0;
		const struct pane *t = f >= 0 ? &w.panes[f] : &T;
		for (int i = 0; i < w.npanes; i++)
			marked |= w.panes[i].group == w.groups[g].id && w.panes[i].marked;
		tabs[g] = (struct line_tab){t->name[0] ? t->name : t->title, g == cur,
		                            marked};
	}
	struct line_colors lc = {
	    theme_cell("frost.line", 0x1f2529), theme_cell("text", 0xccd2d6),
	    theme_cell("dim", 0x889196), theme_cell("alert", 0xff1712)};
	struct cell *row = calloc((size_t)w.cols, sizeof(*row));
	if (!row)
		return;
	int cx;
	int link = !w.line_open && w.link_kind && w.link_url[0];
#ifdef FROST_TEST
	static char logged[LINK_MAX];
	if (link && strcmp(logged, w.link_url)) {
		test_log("line shows a link, bytes", (long)strlen(w.link_url));
		memcpy(logged, w.link_url, sizeof(logged));
	}
#endif
	line_cells(row, w.cols, tabs, w.ngroups, w.line_open, w.line_input,
	           link ? w.link_url : w.line_msg, !link && w.line_error, &lc, &cx);
	uint16_t map = 0;
	struct view lv = {w.cols, 1,    row,     &map,       cx < 0 ? 0 : cx,
	                  0,      0,    cx >= 0, CURSOR_BAR, 0,
	                  0,      NULL, NULL,    NULL,       0,
	                  0,      0,    0};
	int oy = w.r.oy;
	w.r.oy += (w.rows - 1) * w.r.ch;
	render_row(&w.r, b->cv, &lv, 0);
	render_cursor(&w.r, b->cv, &lv, 1, 1);
	w.r.oy = oy;
	free(row);
	wl_surface_damage_buffer(w.surf, 0, oy + (w.rows - 1) * w.r.ch, w.bw,
	                         w.r.ch);
}

static void paint_cells(struct buffer *b, int x, int y, const struct cell *c,
                        int n) {
	uint16_t map = 0;
	struct view v = {n, 1, c,    &map, 0,    0, 0, 0, CURSOR_BAR,
	                 0, 0, NULL, NULL, NULL, 0, 0, 0, 0};
	int ox = w.r.ox, oy = w.r.oy;
	w.r.ox += x * w.r.cw, w.r.oy += y * w.r.ch;
	render_row(&w.r, b->cv, &v, 0);
	w.r.ox = ox, w.r.oy = oy;
}

static void paint_borders(struct buffer *b, const int *idx, int n) {
	int rows = term_rows();
	size_t cells = (size_t)w.cols * (size_t)rows;
	uint32_t *cp = malloc(cells * sizeof(*cp));
	uint8_t *lit = malloc(cells);
	struct cell *row = calloc((size_t)w.cols, sizeof(*row));
	struct box boxes[PANES_MAX];
	int focus = -1;
	for (int k = 0; k < n; k++) {
		boxes[k] = w.panes[idx[k]].box;
		if (idx[k] == w.cur)
			focus = k;
	}
	if (cp && lit && row) {
		mosaic_borders(w.cols, rows, n, boxes, focus, cp, lit);
#ifdef FROST_TEST
		long all = 0, on = 0;
		for (size_t i = 0; i < cells; i++)
			all += cp[i] != 0, on += lit[i];
		test_log("borders, cells", all);
		test_log("borders lit, cells", on);
#endif
		uint32_t dim = theme_cell("border", 0x3e494f);
		uint32_t bright = theme_cell("text", 0xccd2d6);
		for (int y = 0; y < rows; y++)
			for (int x = 0; x < w.cols;) {
				size_t i = (size_t)y * (size_t)w.cols + (size_t)x;
				if (!cp[i]) {
					x++;
					continue;
				}
				int from = x;
				for (; x < w.cols && cp[i]; x++, i++)
					row[x] =
					    (struct cell){.cp = cp[i], .fg = lit[i] ? bright : dim};
				paint_cells(b, from, y, row + from, x - from);
			}
	}
	free(cp);
	free(lit);
	free(row);
}

static void paint_titles(struct buffer *b, const int *idx, int n) {
	struct cell *row = calloc((size_t)w.cols, sizeof(*row));
	for (int k = 0; k < n && row; k++) {
		const struct box *bx = &w.panes[idx[k]].box;
		if (bx->title_y < 0)
			continue;
		int on = idx[k] == w.cur;
		int end = tmux_title(idx[k], on,
		                     on ? theme_cell("text", 0xccd2d6)
		                        : theme_cell("border", 0x3e494f),
		                     row, bx->cols);
		paint_cells(b, bx->x, bx->title_y, row, bx->cols);
#ifdef FROST_TEST
		char what[256];
		size_t o = (size_t)snprintf(what, sizeof(what), "title %%%u [",
		                            w.panes[idx[k]].id);
		long colour = 0;
		for (int x = 1; x < end && o + 12 < sizeof(what); x++) {
			if (!colour && row[x].cp != ' ')
				colour = row[x].fg;
			if (row[x].cp)
				what[o++] = row[x].cp < 0x80 ? (char)row[x].cp : '?';
		}
		snprintf(what + o, sizeof(what) - o, "], colour");
		test_log(what, colour);
#else
		(void)end;
#endif
	}
	free(row);
}

static void draw_pane(struct pane *t, int bi, int full) {
	struct view v;
	const struct box *bx = &t->box;
	uint8_t *stale = t->stale[bi], *other = t->stale[!bi];
	if (!stale || !other || view_of(t, &v) < 0)
		return;
	const uint8_t *dirty = t->map + shm_layout(v.cols, v.rows).dirty;
	int focus = t == &T;
	int limit = (line_shown() ? w.rows - 1 : w.rows) - bx->y;
	int below = bx->rows < limit ? bx->rows : limit;
	int rows = v.rows < below ? v.rows : below;
	if (full)
		memset(stale, 1, (size_t)bx->rows);
	if (focus && w.cur_drawn && w.cur_y >= 0 && w.cur_y < rows)
		stale[w.cur_y] = 1;
	if (focus && v.y < rows)
		stale[v.y] = 1;
	int ox = w.r.ox, oy = w.r.oy;
	w.r.ox += bx->x * w.r.cw, w.r.oy += bx->y * w.r.ch;
	w.r.clip_cols = bx->cols;
	int top = rows, bottom = -1;
	for (int y = 0; y < rows; y++) {
		if (!stale[y] && !(t->damage && dirty[y]))
			continue;
		render_row(&w.r, w.buf[bi].cv, &v, y);
		stale[y] = 0;
		other[y] = 1;
		top = y < top ? y : top;
		bottom = y;
	}
	if (focus) {
		if (v.y < rows)
			render_cursor(&w.r, w.buf[bi].cv, &v, w.focused && !w.line_open,
			              w.lit);
		w.cur_y = v.y, w.cur_drawn = 1;
	}
	if (full)
		memset(other, 1, (size_t)bx->rows);
	else if (bottom >= 0)
		wl_surface_damage_buffer(w.surf, w.r.ox, w.r.oy + top * w.r.ch,
		                         bx->cols * w.r.cw,
		                         (bottom - top + 1) * w.r.ch);
	w.r.ox = ox, w.r.oy = oy, w.r.clip_cols = 0;
	if (t->damage) {
		msg_send(t->sock, MSG_ACK, &t->seq, sizeof(t->seq), -1);
		t->damage = 0;
	}
}

static void draw(void) {
	struct view v;
	if (w.frame || !w.npanes || view_of(&T, &v) < 0)
		return;
	int idx[PANES_MAX], n = group_panes(group_index(T.group), idx);
	int any = w.full || w.tick;
	for (int k = 0; k < n && !any; k++)
		any = w.panes[idx[k]].damage;
	if (!any)
		return;
	struct buffer *b = !w.buf[0].busy   ? &w.buf[0]
	                   : !w.buf[1].busy ? &w.buf[1]
	                                    : NULL;
	if (!b)
		return;
	keep_selection(&v, T.map + shm_layout(v.cols, v.rows).dirty);
	int blinks = w.focused && v.cursor_visible && v.cursor_blink;
	if (blinks != w.blinking || (blinks && T.damage && v.y != w.cur_y))
		blink_arm(blinks);
	if (w.full)
		w.buf[0].full = w.buf[1].full = 1, w.full = 0;
	int full = b->full;
	if (full) {
		tdraw_fill(b->cv, 0, 0, w.bw, w.bh,
		           render_color(&w.r, &v, COLOR_DEFAULT, 0));
		if (n > 1) {
			paint_borders(b, idx, n);
			paint_titles(b, idx, n);
		}
	}
	for (int k = 0; k < n; k++)
		draw_pane(&w.panes[idx[k]], b == &w.buf[1], full);
	w.tick = 0;
	if (line_shown())
		paint_line(b);
	if (full) {
		b->full = 0;
		wl_surface_damage_buffer(w.surf, 0, 0, w.bw, w.bh);
	}
	wl_surface_attach(w.surf, b->wl, 0, 0);
	b->busy = 1;
	w.shown = b;
	w.frame = wl_surface_frame(w.surf);
	wl_callback_add_listener(w.frame, &frame_listener, NULL);
	wl_surface_commit(w.surf);
#ifdef FROST_TEST
	static long frames;
	test_log("frames, count", ++frames);
#endif
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	(void)data, (void)time;
	wl_callback_destroy(cb);
	w.frame = NULL;
}

static const struct shm_head *head(void) {
	const struct shm_head *h = (const struct shm_head *)T.map;
	return h && h->magic == SHM_MAGIC && h->version == SHM_VERSION ? h : NULL;
}

static void input(const char *bytes, size_t n) {
	if (n && T.sock >= 0)
		msg_send(T.sock, MSG_INPUT, bytes, n, -1);
}

static void repeat_arm(uint32_t key) {
	struct itimerspec t = {0};
	if (key && w.rep_rate > 0) {
		long first = w.rep_delay > 0 ? w.rep_delay : 1;
		t.it_value.tv_sec = first / 1000;
		t.it_value.tv_nsec = first % 1000 * 1000000L;
		t.it_interval.tv_nsec = 1000000000L / w.rep_rate;
	}
	timerfd_settime(w.rep, 0, &t, NULL);
	w.rep_key = key;
}

static void scroll_history(int32_t rows) {
	if (T.sock >= 0)
		msg_send(T.sock, MSG_SCROLL, &rows, sizeof(rows), -1);
}

static void source_target(void *data, struct wl_data_source *src,
                          const char *mime) {
	(void)data, (void)src, (void)mime;
}

static void source_send(void *data, struct wl_data_source *src,
                        const char *mime, int fd) {
	(void)data, (void)mime;
	fcntl(fd, F_SETFL, 0);
	for (size_t done = 0; src == w.source && done < w.clip_len;) {
		ssize_t k = write(fd, w.clip + done, w.clip_len - done);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			break;
		done += (size_t)k;
	}
	close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *src) {
	(void)data;
	if (src == w.source) {
		w.source = NULL;
		free(w.clip);
		w.clip = NULL, w.clip_len = 0;
	}
	wl_data_source_destroy(src);
}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
};

void set_clipboard(char *text, size_t len) {
	if (!w.dd || !text || !len) {
		free(text);
		return;
	}
	if (w.source)
		wl_data_source_destroy(w.source);
	free(w.clip);
	w.clip = text, w.clip_len = len;
	test_log("clipboard, bytes", (long)len);
	w.source = wl_data_device_manager_create_data_source(w.ddm);
	wl_data_source_add_listener(w.source, &source_listener, NULL);
	wl_data_source_offer(w.source, "text/plain;charset=utf-8");
	wl_data_source_offer(w.source, "text/plain");
	wl_data_source_offer(w.source, "UTF8_STRING");
	wl_data_device_set_selection(w.dd, w.source, w.serial);
}

static void psrc_send(void *data, struct zwp_primary_selection_source_v1 *src,
                      const char *mime, int fd) {
	(void)data, (void)mime;
	fcntl(fd, F_SETFL, 0);
	for (size_t done = 0; src == w.psrc && done < w.pclip_len;) {
		ssize_t k = write(fd, w.pclip + done, w.pclip_len - done);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			break;
		done += (size_t)k;
	}
	close(fd);
}

static void psrc_cancelled(void *data,
                           struct zwp_primary_selection_source_v1 *src) {
	(void)data;
	if (src == w.psrc) {
		w.psrc = NULL;
		free(w.pclip);
		w.pclip = NULL, w.pclip_len = 0;
	}
	zwp_primary_selection_source_v1_destroy(src);
}

static const struct zwp_primary_selection_source_v1_listener psrc_listener = {
    .send = psrc_send,
    .cancelled = psrc_cancelled,
};

static void set_primary(char *text, size_t len) {
	if (!w.pdev || !text || !len) {
		free(text);
		return;
	}
	if (w.psrc)
		zwp_primary_selection_source_v1_destroy(w.psrc);
	free(w.pclip);
	w.pclip = text, w.pclip_len = len;
	test_log("primary, bytes", (long)len);
	w.psrc = zwp_primary_selection_device_manager_v1_create_source(w.psm);
	zwp_primary_selection_source_v1_add_listener(w.psrc, &psrc_listener, NULL);
	zwp_primary_selection_source_v1_offer(w.psrc, "text/plain;charset=utf-8");
	zwp_primary_selection_source_v1_offer(w.psrc, "text/plain");
	zwp_primary_selection_source_v1_offer(w.psrc, "UTF8_STRING");
	zwp_primary_selection_device_v1_set_selection(w.pdev, w.psrc, w.serial);
}

static void select_done(void) {
	struct view v;
	size_t n;
	if (!w.sel_on || view_of(&T, &v) < 0 || v.sel_to <= v.sel_from)
		return;
	char *text = select_text(&v, v.sel_from, v.sel_to, &n);
	set_primary(text, n);
}

static void copy(void) {
	struct view v;
	size_t n;
	test_log("copy, selection", w.sel_on);
	if (!w.sel_on || view_of(&T, &v) < 0 || v.sel_to <= v.sel_from)
		return;
	char *text = select_text(&v, v.sel_from, v.sel_to, &n);
	set_clipboard(text, n);
}

static void paste_text(const char *t, size_t n) {
	const struct shm_head *h = head();
	char *out = malloc(n + PASTE_WRAP);
	if (!out || !h) {
		free(out);
		return;
	}
	size_t m = paste_filter(t, n, h->bracketed_paste, out);
	if (h->offset)
		scroll_history(INT32_MIN);
	for (size_t i = 0; i < m; i += MSG_MAX)
		input(out + i, m - i < MSG_MAX ? m - i : MSG_MAX);
	free(out);
}

struct mimes {
	int utf8, plain, utf8_string;
};

static void offer_mime(void *data, struct wl_data_offer *o, const char *mime) {
	(void)o;
	struct mimes *m = data;
	m->utf8 |= !strcmp(mime, "text/plain;charset=utf-8");
	m->plain |= !strcmp(mime, "text/plain");
	m->utf8_string |= !strcmp(mime, "UTF8_STRING");
}

static void offer_actions(void *data, struct wl_data_offer *o, uint32_t a) {
	(void)data, (void)o, (void)a;
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime,
    .source_actions = offer_actions,
    .action = offer_actions,
};

static void drop_offer(struct wl_data_offer *o) {
	if (o) {
		free(wl_data_offer_get_user_data(o));
		wl_data_offer_destroy(o);
	}
}

static void poffer_mime(void *data, struct zwp_primary_selection_offer_v1 *o,
                        const char *mime) {
	offer_mime(data, NULL, mime);
	(void)o;
}

static const struct zwp_primary_selection_offer_v1_listener poffer_listener = {
    .offer = poffer_mime,
};

static void drop_poffer(struct zwp_primary_selection_offer_v1 *o) {
	if (o) {
		free(zwp_primary_selection_offer_v1_get_user_data(o));
		zwp_primary_selection_offer_v1_destroy(o);
	}
}

static void pdev_offer(void *data, struct zwp_primary_selection_device_v1 *d,
                       struct zwp_primary_selection_offer_v1 *o) {
	(void)data, (void)d;
	struct mimes *m = calloc(1, sizeof(*m));
	if (!m) {
		zwp_primary_selection_offer_v1_destroy(o);
		return;
	}
	zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, m);
}

static void pdev_selection(void *data,
                           struct zwp_primary_selection_device_v1 *d,
                           struct zwp_primary_selection_offer_v1 *o) {
	(void)data, (void)d;
	drop_poffer(w.poffer);
	w.poffer = o;
}

static const struct zwp_primary_selection_device_v1_listener pdev_listener = {
    .data_offer = pdev_offer,
    .selection = pdev_selection,
};

static const char *mime_of(const struct mimes *m) {
	return m->utf8          ? "text/plain;charset=utf-8"
	       : m->utf8_string ? "UTF8_STRING"
	       : m->plain       ? "text/plain"
	                        : NULL;
}

static void paste_primary(void) {
	test_log("paste primary, offer", w.psrc ? 2 : w.poffer != NULL);
	if (w.psrc) {
		paste_text(w.pclip, w.pclip_len);
		return;
	}
	if (!w.poffer || w.paste_fd >= 0)
		return;
	const char *mime =
	    mime_of(zwp_primary_selection_offer_v1_get_user_data(w.poffer));
	int fds[2];
	if (!mime || pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	zwp_primary_selection_offer_v1_receive(w.poffer, mime, fds[1]);
	close(fds[1]);
	w.paste_fd = fds[0], w.paste_len = 0;
}

static void paste(void) {
	test_log("paste, offer", w.source ? 2 : w.offer != NULL);
	if (w.source) {
		paste_text(w.clip, w.clip_len);
		return;
	}
	if (!w.offer || w.paste_fd >= 0)
		return;
	const char *mime = mime_of(wl_data_offer_get_user_data(w.offer));
	int fds[2];
	if (!mime || pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	wl_data_offer_receive(w.offer, mime, fds[1]);
	close(fds[1]);
	w.paste_fd = fds[0], w.paste_len = 0;
}

static void paste_read(void) {
	if (w.paste_len + 65536 > w.paste_cap && w.paste_cap < PASTE_MAX) {
		size_t cap = w.paste_cap ? w.paste_cap * 2 : 65536;
		char *p = realloc(w.paste, cap);
		if (p)
			w.paste = p, w.paste_cap = cap;
	}
	ssize_t k =
	    w.paste_len < w.paste_cap
	        ? read(w.paste_fd, w.paste + w.paste_len, w.paste_cap - w.paste_len)
	        : 0;
	if (k < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (k > 0) {
		w.paste_len += (size_t)k;
		return;
	}
	close(w.paste_fd);
	w.paste_fd = -1;
	test_log("pasted, bytes", (long)w.paste_len);
	paste_text(w.paste, w.paste_len);
}

static void dd_offer(void *data, struct wl_data_device *dd,
                     struct wl_data_offer *o) {
	(void)data, (void)dd;
	struct mimes *m = calloc(1, sizeof(*m));
	if (!m) {
		wl_data_offer_destroy(o);
		return;
	}
	wl_data_offer_add_listener(o, &offer_listener, m);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
                     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *o) {
	(void)data, (void)dd, (void)serial, (void)s, (void)x, (void)y;
	drop_offer(o);
}

static void dd_leave(void *data, struct wl_data_device *dd) {
	(void)data, (void)dd;
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t time,
                      wl_fixed_t x, wl_fixed_t y) {
	(void)data, (void)dd, (void)time, (void)x, (void)y;
}

static void dd_selection(void *data, struct wl_data_device *dd,
                         struct wl_data_offer *o) {
	(void)data, (void)dd;
	drop_offer(w.offer);
	w.offer = o;
}

static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_leave,
    .selection = dd_selection,
};

static int shortcut(const struct key *k) {
	uint32_t c = k->code < 128 ? k->code : k->base;
	if ((k->mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER)) !=
	    (MOD_SHIFT | MOD_CTRL))
		return 0;
	return c == 'c' || c == 'v' || c == ' ' ? (int)c : 0;
}

static void run_line(void);

static void line_toggle(int open) {
	w.line_open = open;
	w.line_input[0] = w.line_msg[0] = 0;
	w.line_error = 0;
	w.full = 1;
}

static void line_key(const struct key *k) {
	size_t len = strlen(w.line_input);
	if (k->type == KEY_RELEASE)
		return;
	if (k->code == FKEY_ESCAPE) {
		line_toggle(0);
		return;
	}
	if (k->code == FKEY_ENTER || k->code == FKEY_KP_ENTER) {
		run_line();
		return;
	}
	if (k->code == FKEY_BACKSPACE) {
		while (len > 0 && ((unsigned char)w.line_input[len - 1] & 0xC0) == 0x80)
			len--;
		w.line_input[len > 0 ? len - 1 : 0] = 0;
	} else if (k->text[0] && (unsigned char)k->text[0] >= 0x20 &&
	           k->text[0] != 0x7F &&
	           !(k->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
		if (len + strlen(k->text) < sizeof(w.line_input))
			strcat(w.line_input, k->text);
	} else {
		return;
	}
	w.line_msg[0] = 0;
	w.full = 1;
}

static void on_key(uint32_t evdev, int type) {
	struct key k;
	if (kbd_key(&w.kbd, evdev, type, &k) < 0)
		return;
	int sc = shortcut(&k);
	if (sc && type == KEY_PRESS && sc == 'c')
		copy();
	else if (sc && type == KEY_PRESS && sc == 'v')
		paste();
	else if (sc && type == KEY_PRESS && sc == ' ')
		line_toggle(!w.line_open);
	if (sc)
		return;
	if (w.line_open) {
		line_key(&k);
		return;
	}
	const struct shm_head *h = head();
	struct key_modes m = {0, 0, 0};
	if (h)
		m = (struct key_modes){h->keys, h->app_cursor, h->newline};
	char out[KEY_OUT_MAX];
	size_t n = key_encode(&k, &m, out);
	if (n && h && h->offset)
		scroll_history(INT32_MIN);
	input(out, n);
}

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int fd, uint32_t size) {
	(void)data, (void)kb;
	if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
		char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (text != MAP_FAILED) {
			if (kbd_keymap(&w.kbd, text) < 0)
				fprintf(stderr, "frost: cannot read the keymap\n");
			munmap(text, size);
		}
	}
	close(fd);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *s, struct wl_array *keys) {
	(void)data, (void)kb, (void)serial, (void)s, (void)keys;
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *s) {
	(void)data, (void)kb, (void)serial, (void)s;
	repeat_arm(0);
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
	(void)data, (void)kb, (void)time;
	int down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (down)
		w.serial = serial;
	on_key(key, down ? KEY_PRESS : KEY_RELEASE);
	if (down && kbd_repeats(&w.kbd, key))
		repeat_arm(key);
	else if (!down && key == w.rep_key)
		repeat_arm(0);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t depressed, uint32_t latched, uint32_t locked,
                         uint32_t group) {
	(void)data, (void)kb, (void)serial;
	kbd_modifiers(&w.kbd, depressed, latched, locked, group);
	update_hover();
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                           int32_t delay) {
	(void)data, (void)kb;
	w.rep_rate = rate, w.rep_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat_info};

void set_cursor(void) {
	const struct shm_head *h = head();
	const char *name = w.link_kind     ? "pointer"
	                   : h && h->mouse ? "default"
	                                   : "text";
	if (!w.pointer || !w.cursor_theme || !w.enter_serial ||
	    name == w.cursor_now)
		return;
	const char *old = w.link_kind     ? "hand2"
	                  : h && h->mouse ? "left_ptr"
	                                  : "xterm";
	struct wl_cursor *c = wl_cursor_theme_get_cursor(w.cursor_theme, name);
	if (!c)
		c = wl_cursor_theme_get_cursor(w.cursor_theme, old);
	struct wl_buffer *b = c ? wl_cursor_image_get_buffer(c->images[0]) : NULL;
	if (!b)
		return;
	wl_surface_attach(w.cursor_surf, b, 0, 0);
	wl_surface_damage_buffer(w.cursor_surf, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(w.cursor_surf);
	wl_pointer_set_cursor(w.pointer, w.enter_serial, w.cursor_surf,
	                      (int32_t)c->images[0]->hotspot_x,
	                      (int32_t)c->images[0]->hotspot_y);
	w.cursor_now = name;
}

static int pointer_pane(void) {
	int x = (int)w.px - w.r.ox, y = (int)w.py - w.r.oy;
	if (x < 0 || y < 0 || w.r.cw <= 0 || w.r.ch <= 0 || !w.npanes)
		return -1;
	int col = x / w.r.cw, row = y / w.r.ch;
	for (int i = 0; i < w.npanes; i++) {
		const struct box *b = &w.panes[i].box;
		int top = b->title_y >= 0 ? b->title_y : b->y;
		if (w.panes[i].group == T.group && col >= b->x &&
		    col < b->x + b->cols && row >= top && row < b->y + b->rows)
			return i;
	}
	return -1;
}

static void pointer_at(struct mouse_event *e) {
	const struct box *b = &T.box;
	int gw = b->cols > 0 ? b->cols * w.r.cw : 1;
	int gh = b->rows > 0 ? b->rows * w.r.ch : 1;
	int x = (int)w.px - w.r.ox - b->x * w.r.cw;
	int y = (int)w.py - w.r.oy - b->y * w.r.ch;
	e->x = x < 0 ? 0 : x >= gw ? gw - 1 : x;
	e->y = y < 0 ? 0 : y >= gh ? gh - 1 : y;
	e->col = e->x / w.r.cw, e->row = e->y / w.r.ch;
}

static int report(enum mouse_kind kind, int button) {
	const struct shm_head *h = head();
	if (!h || !h->mouse)
		return 0;
	struct mouse_event e = {kind, button, kbd_mods(&w.kbd), 0, 0, 0, 0};
	if (e.mods & MOD_SHIFT)
		return 0;
	pointer_at(&e);
	struct mouse_modes m = {h->mouse, h->mouse_sgr, h->mouse_pixels};
	char out[MOUSE_OUT_MAX];
	size_t n = mouse_encode(&e, &m, out);
	input(out, n);
	return 1;
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	(void)data, (void)p, (void)s;
	w.enter_serial = serial;
	w.px = wl_fixed_to_double(x), w.py = wl_fixed_to_double(y);
	w.cursor_now = NULL;
	set_cursor();
	update_hover();
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {
	(void)data, (void)p, (void)serial, (void)s;
	w.enter_serial = 0;
	w.held = MB_NONE;
	update_hover();
}

static int cell_index(void) {
	const struct shm_head *h = head();
	struct mouse_event e;
	pointer_at(&e);
	int cols = h ? h->cols : w.cols, rows = h ? h->rows : w.rows;
	int col = e.col < cols ? e.col : cols - 1;
	int row = e.row < rows ? e.row : rows - 1;
	return row * cols + col;
}

static void update_hover(void) {
	struct view v;
	int kind = LINK_NONE, from = 0, to = 0, id = 0;
	if (w.enter_serial && (kbd_mods(&w.kbd) & MOD_CTRL) &&
	    pointer_pane() == w.cur && view_of(&T, &v) == 0)
		kind = link_at(&v, cell_index(), &from, &to, &id, w.link_url);
	if (kind != w.link_kind || from != w.link_from || to != w.link_to) {
		w.link_kind = kind, w.link_from = from, w.link_to = to;
		w.link_id = id;
		w.link_waiting = 0;
		w.full = 1;
		set_cursor();
		if (kind != LINK_BARE)
			w.link_url[0] = 0;
		if (kind == LINK_OSC8) {
			uint16_t n = (uint16_t)id;
			msg_send(T.sock, MSG_LINK_ASK, &n, sizeof(n), -1);
		}
	}
}

void open_url(const char *url) {
	if (!link_allowed(url)) {
		fprintf(stderr, "frost: only http, https, ftp, file and mailto links "
		                "are opened\n");
		return;
	}
	const char *b = getenv("BROWSER");
	if (!b || !*b)
		b = "caribou";
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) {
			setsid();
			execlp(b, b, url, (char *)NULL);
			fprintf(stderr, "frost: cannot run %s\n", b);
			_exit(127);
		}
		_exit(0);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

static void open_link(void) {
	if (w.link_url[0])
		open_url(w.link_url);
	else if (w.link_kind == LINK_OSC8)
		w.link_waiting = 1;
}

static void select_press(uint32_t time) {
	const struct shm_head *h = head();
	int pos = cell_index();
	if (pos == w.click_pos && time - w.click_time < CLICK_MS && w.clicks < 3)
		w.clicks++;
	else
		w.clicks = 1;
	w.click_pos = pos, w.click_time = time;
	w.sel_mode = w.clicks == 1   ? SELECT_CELLS
	             : w.clicks == 2 ? SELECT_WORDS
	                             : SELECT_LINES;
	w.sel_anchor = w.sel_head = pos;
	w.sel_offset = h ? h->offset : 0;
	w.selecting = 1;
	w.sel_on = w.clicks > 1;
	w.full = 1;
	test_log("click", w.clicks);
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t x, wl_fixed_t y) {
	(void)data, (void)p, (void)time;
	w.px = wl_fixed_to_double(x), w.py = wl_fixed_to_double(y);
	set_cursor();
	update_hover();
	if (w.selecting) {
		int pos = cell_index();
		if (pos != w.sel_head) {
			w.sel_head = pos;
			w.sel_on = 1;
			w.full = 1;
		}
		return;
	}
	const struct shm_head *h = head();
	struct mouse_event e;
	pointer_at(&e);
	int px = h && h->mouse_pixels;
	int at_x = px ? e.x : e.col, at_y = px ? e.y : e.row;
	if (at_x != w.last_x || at_y != w.last_y) {
		w.last_x = at_x, w.last_y = at_y;
		report(MOUSE_MOVE, w.held);
	}
}

static int button_of(uint32_t b) {
	switch (b) {
	case BTN_LEFT:
		return MB_LEFT;
	case BTN_MIDDLE:
		return MB_MIDDLE;
	case BTN_RIGHT:
		return MB_RIGHT;
	case BTN_SIDE:
		return MB_BACK;
	case BTN_EXTRA:
		return MB_FORWARD;
	}
	return -1;
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
	(void)data, (void)p;
	int b = button_of(button);
	if (b < 0)
		return;
	int pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (!pressed && b == w.swallow) {
		w.swallow = MB_NONE;
		return;
	}
	int at = pressed ? pointer_pane() : -1;
	if (at >= 0 && at != w.cur) {
		w.serial = serial;
		w.swallow = b;
		pane_focus(at);
		return;
	}
	if (b == MB_LEFT &&
	    (w.link_click ||
	     (w.link_kind && state == WL_POINTER_BUTTON_STATE_PRESSED))) {
		w.link_click = state == WL_POINTER_BUTTON_STATE_PRESSED;
		if (w.link_click)
			open_link();
		return;
	}
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		w.serial = serial;
		w.held = b;
		int sent = report(MOUSE_DOWN, b);
		if (!sent && b == MB_LEFT)
			select_press(time);
		else if (!sent && b == MB_MIDDLE)
			paste_primary();
	} else {
		if (w.selecting && b == MB_LEFT) {
			w.selecting = 0;
			select_done();
		} else {
			report(MOUSE_UP, b);
		}
		if (w.held == b)
			w.held = MB_NONE;
	}
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {
	(void)data, (void)p, (void)time;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		w.scroll += wl_fixed_to_double(value);
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src) {
	(void)data, (void)p;
	w.finger = src != WL_POINTER_AXIS_SOURCE_WHEEL;
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                          uint32_t axis) {
	(void)data, (void)p, (void)time;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		w.scroll = 0;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t discrete) {
	(void)data, (void)p;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		w.discrete += discrete;
}

static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t value120) {
	(void)data, (void)p;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		w.value120 += value120;
}

static void ptr_axis_direction(void *data, struct wl_pointer *p, uint32_t axis,
                               uint32_t direction) {
	(void)data, (void)p, (void)axis, (void)direction;
}

static void ptr_frame(void *data, struct wl_pointer *p) {
	(void)data, (void)p;
	int notches = 0, lines = 0;
	if (w.value120 / 120) {
		notches = w.value120 / 120;
		w.value120 -= notches * 120;
		lines = notches * WHEEL_LINES;
		w.scroll = 0;
	} else if (w.discrete) {
		notches = w.discrete;
		lines = notches * WHEEL_LINES;
		w.discrete = 0, w.scroll = 0;
	} else if (w.finger && w.r.ch > 0) {
		lines = (int)(w.scroll / w.r.ch);
		w.scroll -= lines * w.r.ch;
		notches = lines;
	}
	if (!lines)
		return;
	const struct shm_head *h = head();
	if (!h)
		return;
	int down = lines > 0;
	if (h->mouse) {
		for (int i = 0; i < abs(notches); i++)
			report(MOUSE_WHEEL, down ? MB_WHEEL_DOWN : MB_WHEEL_UP);
	} else if (!h->alt) {
		scroll_history(-lines);
	} else if (h->alt_scroll) {
		struct key k = {down ? FKEY_DOWN : FKEY_UP, 0, 0, 0, KEY_PRESS, ""};
		struct key_modes m = {h->keys, h->app_cursor, h->newline};
		char out[KEY_OUT_MAX];
		size_t n = key_encode(&k, &m, out);
		for (int i = 0; i < abs(lines); i++)
			input(out, n);
	}
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_direction,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
	(void)data;
	int has = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (has && !w.keyboard) {
		w.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(w.keyboard, &keyboard_listener, NULL);
	} else if (!has && w.keyboard) {
		wl_keyboard_destroy(w.keyboard);
		w.keyboard = NULL;
		repeat_arm(0);
	}
	int pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	if (pointer && !w.pointer) {
		w.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(w.pointer, &pointer_listener, NULL);
	} else if (!pointer && w.pointer) {
		wl_pointer_destroy(w.pointer);
		w.pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
	(void)data, (void)seat, (void)name;
}

static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

static void focus_changed(void) {
	const struct shm_head *h = head();
	if (h && h->focus_events)
		input(w.focused ? "\x1b[I" : "\x1b[O", 3);
	if (!w.focused)
		repeat_arm(0);
}

int line_shown(void) {
	return w.ngroups >= 2 || w.line_open || (w.link_kind && w.link_url[0]);
}

void show_title(void) {
	if (w.npanes)
		xdg_toplevel_set_title(w.top, T.title[0] ? T.title : "frost");
}

void say(int error, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(w.line_msg, sizeof(w.line_msg), fmt, ap);
	va_end(ap);
	w.line_error = error;
}

static int set_size(int size) {
	struct render r;
	if (size < 6 || size > 128 || render_init(&r, size) < 0)
		return -1;
	render_free(&w.r);
	w.r = r;
	render_theme(&w.r, w.theme);
	render_fit(&w.r, w.bw, w.bh, &w.cols, &w.rows);
	relayout();
	return 0;
}

static void run_line(void) {
	struct line_command c;
	line_parse(w.line_input, &c);
	w.line_input[0] = 0;
	switch (c.cmd) {
	case CMD_NONE:
		line_toggle(0);
		return;
	case CMD_GOTO:
		if (c.number < 1 || c.number > w.ngroups) {
			say(1, "no tab %d", c.number);
			break;
		}
		pane_focus(group_focus(c.number - 1));
		line_toggle(0);
		return;
	case CMD_NEW: {
		char cwd[PATH_MAX];
		memcpy(cwd, T.cwd, sizeof(cwd));
		if (w.npanes == PANES_MAX ||
		    pane_new(group_new(), -1, NULL, cwd, 1) < 0) {
			say(1, "no room for another tab");
			break;
		}
		line_toggle(0);
		return;
	}
	case CMD_LS:
		list_terminals();
		break;
	case CMD_SIZE:
		if (set_size(c.number) < 0) {
			say(1, "no mono font at size %d", c.number);
			break;
		}
		line_toggle(0);
		return;
	case CMD_ATTACH:
		if (attach_pane(c.arg, c.number) < 0)
			break;
		line_toggle(0);
		return;
	case CMD_DETACH:
		if (detach_pane(c.arg) < 0)
			break;
		line_toggle(0);
		return;
	case CMD_BAD:
		say(1, "orders: N, goto N, new, ls, size N, attach, detach");
		break;
	}
	w.full = 1;
}

static int apply_configure(const struct cli *c) {
	int width = w.next_w, height = w.next_h;
	if (width <= 0 || height <= 0) {
		width = DEFAULT_COLS * w.r.cw;
		height = DEFAULT_ROWS * w.r.ch;
	}
	if (w.next_focus != w.focused) {
		w.focused = w.next_focus;
		w.full = 1;
		focus_changed();
	}
	if (width == w.bw && height == w.bh && w.npanes)
		return 0;
	if (make_buffers(width, height) < 0)
		return -1;
	render_fit(&w.r, width, height, &w.cols, &w.rows);
	w.full = 1;
	if (!w.npanes)
		return pane_new(group_new(), -1, c->cmd, NULL, 1) < 0 ? -1 : 0;
	relayout();
	return 0;
}

void cut_copy(char *dst, size_t size, const char *src) {
	size_t k = strnlen(src, size - 1);
	while (k > 0 && src[k] && ((unsigned char)src[k] & 0xC0) == 0x80)
		k--;
	memcpy(dst, src, k);
	dst[k] = 0;
}

#ifdef FROST_TEST
#define DUMP_COLS 20
#define DUMP_ROWS 6

static void dump(void) {
	const char *path = getenv("FROST_TEST_DUMP");
	void *png;
	size_t len;
	if (!path || !w.shown ||
	    tdraw_capture(w.shown->cv, w.r.ox, w.r.oy, DUMP_COLS * w.r.cw,
	                  DUMP_ROWS * w.r.ch, &png, &len) < 0)
		return;
	FILE *f = fopen(path, "w");
	if (f) {
		fwrite(png, 1, len, f);
		fclose(f);
	}
	free(png);
}
#endif

static int open_window(const struct cli *c) {
	w.dpy = wl_display_connect(NULL);
	if (!w.dpy) {
		fprintf(stderr, "frost: cannot connect to the Wayland display\n");
		return -1;
	}
	w.reg = wl_display_get_registry(w.dpy);
	wl_registry_add_listener(w.reg, &registry_listener, NULL);
	if (wl_display_roundtrip(w.dpy) < 0 || !w.comp || !w.shm || !w.wm) {
		fprintf(stderr, "frost: the compositor lacks xdg-shell or wl_shm\n");
		return -1;
	}
	xdg_wm_base_add_listener(w.wm, &wm_listener, NULL);
	const char *size = getenv("XCURSOR_SIZE");
	w.cursor_theme = wl_cursor_theme_load(
	    getenv("XCURSOR_THEME"),
	    size && atoi(size) > 0 ? atoi(size) : CURSOR_SIZE, w.shm);
	w.cursor_surf = wl_compositor_create_surface(w.comp);
	w.held = w.swallow = MB_NONE;
	w.last_x = w.last_y = -1;
	if (w.seat)
		wl_seat_add_listener(w.seat, &seat_listener, NULL);
	if (w.seat && w.ddm) {
		w.dd = wl_data_device_manager_get_data_device(w.ddm, w.seat);
		wl_data_device_add_listener(w.dd, &dd_listener, NULL);
	}
	if (w.seat && w.psm) {
		w.pdev =
		    zwp_primary_selection_device_manager_v1_get_device(w.psm, w.seat);
		zwp_primary_selection_device_v1_add_listener(w.pdev, &pdev_listener,
		                                             NULL);
	}
	w.surf = wl_compositor_create_surface(w.comp);
	w.xs = xdg_wm_base_get_xdg_surface(w.wm, w.surf);
	xdg_surface_add_listener(w.xs, &xs_listener, NULL);
	w.top = xdg_surface_get_toplevel(w.xs);
	xdg_toplevel_add_listener(w.top, &top_listener, NULL);
	xdg_toplevel_set_app_id(w.top, c->app_id ? c->app_id : "frost");
	xdg_toplevel_set_title(w.top, "frost");
	if (w.deco_mgr) {
		w.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(w.deco_mgr,
		                                                            w.top);
		zxdg_toplevel_decoration_v1_set_mode(
		    w.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(w.surf);
	return 0;
}

static void close_window(void) {
	if (!w.dpy)
		return;
	free_buffers();
	if (w.frame)
		wl_callback_destroy(w.frame);
	if (w.deco)
		zxdg_toplevel_decoration_v1_destroy(w.deco);
	if (w.top)
		xdg_toplevel_destroy(w.top);
	if (w.xs)
		xdg_surface_destroy(w.xs);
	if (w.surf)
		wl_surface_destroy(w.surf);
	drop_offer(w.offer);
	if (w.source)
		wl_data_source_destroy(w.source);
	drop_poffer(w.poffer);
	if (w.psrc)
		zwp_primary_selection_source_v1_destroy(w.psrc);
	if (w.pdev)
		zwp_primary_selection_device_v1_destroy(w.pdev);
	if (w.psm)
		zwp_primary_selection_device_manager_v1_destroy(w.psm);
	free(w.pclip);
	if (w.dd)
		wl_data_device_release(w.dd);
	if (w.ddm)
		wl_data_device_manager_destroy(w.ddm);
	free(w.clip);
	free(w.paste);
	if (w.paste_fd >= 0)
		close(w.paste_fd);
	if (w.keyboard)
		wl_keyboard_destroy(w.keyboard);
	if (w.pointer)
		wl_pointer_destroy(w.pointer);
	if (w.cursor_surf)
		wl_surface_destroy(w.cursor_surf);
	if (w.cursor_theme)
		wl_cursor_theme_destroy(w.cursor_theme);
	if (w.seat)
		wl_seat_destroy(w.seat);
	if (w.deco_mgr)
		zxdg_decoration_manager_v1_destroy(w.deco_mgr);
	if (w.wm)
		xdg_wm_base_destroy(w.wm);
	if (w.shm)
		wl_shm_destroy(w.shm);
	if (w.comp)
		wl_compositor_destroy(w.comp);
	if (w.reg)
		wl_registry_destroy(w.reg);
	wl_display_disconnect(w.dpy);
}

static void loop(const struct cli *c) {
	const char *err;
	while (!w.ended) {
		while (wl_display_prepare_read(w.dpy) != 0)
			if (wl_display_dispatch_pending(w.dpy) < 0)
				return;
		if (wl_display_flush(w.dpy) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(w.dpy);
			return;
		}
		struct pollfd p[6 + PANES_MAX] = {
		    {wl_display_get_fd(w.dpy), POLLIN, 0},
		    {w.ctl, POLLIN, 0},
		    {w.theme ? tdraw_theme_fd(w.theme) : -1, POLLIN, 0},
		    {w.blink, POLLIN, 0},
		    {w.rep, POLLIN, 0},
		    {w.paste_fd, POLLIN, 0}};
		int nt = w.npanes;
		for (int i = 0; i < nt; i++)
			p[6 + i] = (struct pollfd){w.panes[i].sock, POLLIN, 0};
		if (poll(p, (nfds_t)(6 + nt), -1) < 0 && errno != EINTR) {
			wl_display_cancel_read(w.dpy);
			perror("poll error");
			return;
		}
		if (p[0].revents & POLLIN) {
			if (wl_display_read_events(w.dpy) < 0)
				return;
		} else {
			wl_display_cancel_read(w.dpy);
			if (p[0].revents & (POLLHUP | POLLERR))
				return;
		}
		if (wl_display_dispatch_pending(w.dpy) < 0)
			return;
		if (w.configured) {
			w.configured = 0;
			if (apply_configure(c) < 0)
				return;
		}
		for (int k = 0; k < nt && !w.ended; k++) {
			if (!(p[6 + k].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			for (int i = 0; i < w.npanes; i++)
				if (w.panes[i].sock == p[6 + k].fd) {
					from_term(i);
					break;
				}
		}
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		if ((p[2].revents & POLLIN) && tdraw_theme_load(w.theme, &err) == 1) {
			render_theme(&w.r, w.theme);
			w.full = 1;
		}
		if (p[3].revents & POLLIN) {
			uint64_t ticks;
			if (read(w.blink, &ticks, sizeof(ticks)) == sizeof(ticks))
				w.lit = !w.lit, w.tick = 1;
		}
		if (p[5].revents & (POLLIN | POLLHUP | POLLERR))
			paste_read();
		if (p[4].revents & POLLIN) {
			uint64_t ticks;
			if (read(w.rep, &ticks, sizeof(ticks)) == sizeof(ticks) &&
			    w.rep_key)
				on_key(w.rep_key, KEY_REPEAT);
		}
		if ((p[1].revents & POLLIN) && !w.ended)
			tmux_accept();
		draw();
	}
}

int win_run(const struct cli *c) {
	for (int i = 0; i < PANES_MAX; i++)
		w.panes[i].sock = -1;
	w.paste_fd = w.ctl = -1;
	w.status = 1;
	signal(SIGPIPE, SIG_IGN);
	if (render_init(&w.r, FONT_SIZE) < 0)
		return 1;
	w.theme = tdraw_theme_open();
	const char *err;
	if (w.theme && tdraw_theme_load(w.theme, &err) == -2)
		fprintf(stderr, "frost: %s\n", err);
	render_theme(&w.r, w.theme);
	w.blink = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	w.rep = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (w.blink < 0 || w.rep < 0)
		perror("timerfd error");
	if (kbd_init(&w.kbd, NULL) < 0)
		fprintf(stderr, "frost: no xkb context\n");
	if (open_window(c) == 0) {
		tmux_listen();
		loop(c);
	}
	tmux_close();
#ifdef FROST_TEST
	dump();
#endif
	for (int i = 0; i < w.npanes; i++) {
		close(w.panes[i].sock);
		if (w.panes[i].map)
			munmap(w.panes[i].map, w.panes[i].map_size);
		free(w.panes[i].stale[0]);
		free(w.panes[i].stale[1]);
	}
	close_window();
	if (w.blink >= 0)
		close(w.blink);
	if (w.rep >= 0)
		close(w.rep);
	kbd_free(&w.kbd);
	render_free(&w.r);
	tdraw_theme_free(w.theme);
	return w.status;
}
