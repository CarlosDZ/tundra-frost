#include "win.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "kbd.h"
#include "mouse.h"
#include "paste.h"
#include "proto.h"
#include "render.h"
#include "select.h"
#include "shm.h"
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

struct buffer {
	struct wl_buffer *wl;
	struct tdraw_canvas *cv;
	uint8_t *stale;
	int busy;
};

static struct win {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct zxdg_toplevel_decoration_v1 *deco;
	struct wl_surface *surf;
	struct xdg_surface *xs;
	struct xdg_toplevel *top;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct wl_cursor_theme *cursor_theme;
	struct wl_surface *cursor_surf;
	const char *cursor_now;
	uint32_t enter_serial;
	double px, py;
	int held;
	int last_x, last_y;
	double scroll;
	int value120, discrete, finger;

	int sel_on, selecting, sel_mode, sel_anchor, sel_head, clicks, click_pos;
	uint32_t click_time, sel_offset;
	uint32_t serial;
	struct wl_data_device_manager *ddm;
	struct wl_data_device *dd;
	struct wl_data_source *source;
	struct wl_data_offer *offer;
	char *clip, *paste;
	size_t clip_len, paste_len, paste_cap;
	int paste_fd;

	int next_w, next_h, next_focus, configured, focused;
	struct buffer buf[2], *shown;
	void *pool;
	size_t pool_size;
	int bw, bh;
	struct wl_callback *frame;

	int sock, closing, ended, status;
	pid_t term;
	uint8_t *map;
	size_t map_size;
	int cols, rows;
	uint32_t seq;
	int damage, full;

	struct render r;
	struct tdraw_theme *theme;
	int blink, blinking, lit, tick;
	int cur_y, cur_drawn;

	struct kbd kbd;
	int rep, rep_rate, rep_delay;
	uint32_t rep_key;
	char msg[MSG_MAX];
} w;

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
	if (w.sock >= 0 && !w.closing) {
		msg_send(w.sock, MSG_CLOSE, NULL, 0, -1);
		w.closing = 1;
	}
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
		free(b->stale);
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
		b->stale = malloc((size_t)height);
	}
	wl_shm_pool_destroy(pool);
	close(fd);
	w.bw = width, w.bh = height;
	for (int i = 0; i < 2; i++)
		if (!w.buf[i].cv || !w.buf[i].stale)
			return -1;
	return 0;
}

static int view_of(struct view *v) {
	if (!w.map)
		return -1;
	const struct shm_head *h = (const struct shm_head *)w.map;
	if (h->magic != SHM_MAGIC || h->version != SHM_VERSION || !h->cols ||
	    !h->rows || w.map_size < shm_layout(h->cols, h->rows).size)
		return -1;
	struct shm_layout l = shm_layout(h->cols, h->rows);
	*v = (struct view){h->cols,
	                   h->rows,
	                   (const struct cell *)(w.map + l.cells),
	                   (const uint16_t *)(w.map + l.map),
	                   h->x,
	                   h->y,
	                   h->pending,
	                   h->cursor_visible,
	                   h->cursor_shape,
	                   h->cursor_blink,
	                   h->reverse,
	                   h->palette,
	                   h->palette_set,
	                   w.map + l.lines,
	                   0,
	                   0};
	if (w.sel_on)
		select_span(v, w.sel_anchor, w.sel_head, w.sel_mode, &v->sel_from,
		            &v->sel_to);
	return 0;
}

static void keep_selection(struct view *v, const uint8_t *dirty) {
	const struct shm_head *h = (const struct shm_head *)w.map;
	if (!w.sel_on || w.selecting)
		return;
	int gone = h->offset != w.sel_offset;
	for (int y = v->sel_from / v->cols; y <= (v->sel_to - 1) / v->cols && !gone;
	     y++)
		gone = w.damage && y < v->rows && dirty[y];
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

static void draw(void) {
	struct view v;
	if (w.frame || !(w.damage || w.full || w.tick) || view_of(&v) < 0)
		return;
	struct buffer *b = !w.buf[0].busy   ? &w.buf[0]
	                   : !w.buf[1].busy ? &w.buf[1]
	                                    : NULL;
	if (!b)
		return;
	struct buffer *other = b == &w.buf[0] ? &w.buf[1] : &w.buf[0];
	const uint8_t *dirty = w.map + shm_layout(v.cols, v.rows).dirty;
	int rows = v.rows < w.rows ? v.rows : w.rows;
	keep_selection(&v, dirty);

	int blinks = w.focused && v.cursor_visible && v.cursor_blink;
	if (blinks != w.blinking || (blinks && w.damage && v.y != w.cur_y))
		blink_arm(blinks);
	if (w.full) {
		render_margins(&w.r, b->cv, &v);
		memset(b->stale, 1, (size_t)w.bh);
	}
	if (w.cur_drawn && w.cur_y < rows)
		b->stale[w.cur_y] = 1;
	if (v.y < rows)
		b->stale[v.y] = 1;
	int top = rows, bottom = -1;
	for (int y = 0; y < rows; y++) {
		if (!b->stale[y] && !(w.damage && dirty[y]))
			continue;
		render_row(&w.r, b->cv, &v, y);
		b->stale[y] = 0;
		other->stale[y] = 1;
		top = y < top ? y : top;
		bottom = y;
	}
	render_cursor(&w.r, b->cv, &v, w.focused, w.lit);
	w.cur_y = v.y, w.cur_drawn = 1, w.tick = 0;
	if (w.full) {
		w.full = 0;
		memset(other->stale, 1, (size_t)w.bh);
		wl_surface_damage_buffer(w.surf, 0, 0, w.bw, w.bh);
	} else if (bottom >= 0) {
		wl_surface_damage_buffer(w.surf, 0, w.r.oy + top * w.r.ch, w.bw,
		                         (bottom - top + 1) * w.r.ch);
	}
	if (w.damage) {
		msg_send(w.sock, MSG_ACK, &w.seq, sizeof(w.seq), -1);
		w.damage = 0;
	}
	wl_surface_attach(w.surf, b->wl, 0, 0);
	b->busy = 1;
	w.shown = b;
	w.frame = wl_surface_frame(w.surf);
	wl_callback_add_listener(w.frame, &frame_listener, NULL);
	wl_surface_commit(w.surf);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	(void)data, (void)time;
	wl_callback_destroy(cb);
	w.frame = NULL;
}

static const struct shm_head *head(void) {
	const struct shm_head *h = (const struct shm_head *)w.map;
	return h && h->magic == SHM_MAGIC && h->version == SHM_VERSION ? h : NULL;
}

static void input(const char *bytes, size_t n) {
	if (n && w.sock >= 0)
		msg_send(w.sock, MSG_INPUT, bytes, n, -1);
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
	if (w.sock >= 0)
		msg_send(w.sock, MSG_SCROLL, &rows, sizeof(rows), -1);
}

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

static void set_clipboard(char *text, size_t len) {
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

static void copy(void) {
	struct view v;
	size_t n;
	test_log("copy, selection", w.sel_on);
	if (!w.sel_on || view_of(&v) < 0 || v.sel_to <= v.sel_from)
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

static void paste(void) {
	test_log("paste, offer", w.source ? 2 : w.offer != NULL);
	if (w.source) {
		paste_text(w.clip, w.clip_len);
		return;
	}
	if (!w.offer || w.paste_fd >= 0)
		return;
	struct mimes *m = wl_data_offer_get_user_data(w.offer);
	const char *mime = m->utf8          ? "text/plain;charset=utf-8"
	                   : m->utf8_string ? "UTF8_STRING"
	                   : m->plain       ? "text/plain"
	                                    : NULL;
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

static void on_key(uint32_t evdev, int type) {
	struct key k;
	if (kbd_key(&w.kbd, evdev, type, &k) < 0)
		return;
	int sc = shortcut(&k);
	if (sc && type == KEY_PRESS && sc == 'c')
		copy();
	else if (sc && type == KEY_PRESS && sc == 'v')
		paste();
	if (sc)
		return;
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
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                           int32_t delay) {
	(void)data, (void)kb;
	w.rep_rate = rate, w.rep_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat_info};

static void set_cursor(void) {
	const struct shm_head *h = head();
	const char *name = h && h->mouse ? "default" : "text";
	if (!w.pointer || !w.cursor_theme || !w.enter_serial ||
	    name == w.cursor_now)
		return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(w.cursor_theme, name);
	if (!c)
		c = wl_cursor_theme_get_cursor(w.cursor_theme,
		                               h && h->mouse ? "left_ptr" : "xterm");
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

static void pointer_at(struct mouse_event *e) {
	int gw = w.cols * w.r.cw, gh = w.rows * w.r.ch;
	int x = (int)w.px - w.r.ox, y = (int)w.py - w.r.oy;
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
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {
	(void)data, (void)p, (void)serial, (void)s;
	w.enter_serial = 0;
	w.held = MB_NONE;
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
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		w.serial = serial;
		w.held = b;
		if (!report(MOUSE_DOWN, b) && b == MB_LEFT)
			select_press(time);
	} else {
		if (w.selecting && b == MB_LEFT)
			w.selecting = 0;
		else
			report(MOUSE_UP, b);
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

static const char *term_path(void) {
#ifdef FROST_TEST
	const char *p = getenv("FROST_TEST_TERM");
	if (p && *p)
		return p;
#endif
	return TUNDRA_FROST_TERM;
}

static int spawn_term(const struct cli *c) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
		perror("socketpair error");
		return -1;
	}
	const char *path = term_path();
	w.term = fork();
	if (w.term < 0) {
		perror("fork error");
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (w.term == 0) {
		if (sv[1] == 3 ? fcntl(3, F_SETFD, 0) < 0 : dup2(sv[1], 3) < 0)
			_exit(127);
		char cols[16], rows[16];
		snprintf(cols, sizeof(cols), "%d", w.cols);
		snprintf(rows, sizeof(rows), "%d", w.rows);
		int n = 0;
		while (c->cmd && c->cmd[n])
			n++;
		char **argv = calloc((size_t)n + 5, sizeof(*argv));
		if (!argv)
			_exit(127);
		argv[0] = (char *)path, argv[1] = "3";
		argv[2] = cols, argv[3] = rows;
		for (int i = 0; i < n; i++)
			argv[4 + i] = c->cmd[i];
		execv(path, argv);
		fprintf(stderr, "frost: cannot run %s\n", path);
		_exit(127);
	}
	close(sv[1]);
	w.sock = sv[0];
	return 0;
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
	if (width == w.bw && height == w.bh && w.sock >= 0)
		return 0;
	if (make_buffers(width, height) < 0)
		return -1;
	render_fit(&w.r, width, height, &w.cols, &w.rows);
	w.full = 1;
	if (w.sock < 0)
		return spawn_term(c);
	struct msg_resize rs = {(uint16_t)w.cols, (uint16_t)w.rows,
	                        (uint16_t)w.r.cw, (uint16_t)w.r.ch};
	msg_send(w.sock, MSG_RESIZE, &rs, sizeof(rs), -1);
	return 0;
}

static void take_shm(int fd) {
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)SHM_HEAD_SIZE)
		return;
	if (w.map)
		munmap(w.map, w.map_size);
	w.map_size = (size_t)st.st_size;
	w.map = mmap(NULL, w.map_size, PROT_READ, MAP_SHARED, fd, 0);
	if (w.map == MAP_FAILED)
		w.map = NULL;
	w.full = 1;
}

static void from_term(void) {
	uint32_t type;
	int fd;
	ssize_t n = msg_recv(w.sock, &type, w.msg, sizeof(w.msg) - 1, &fd);
	if (n < 0 || type == 0) {
		w.ended = 1;
		return;
	}
	switch (type) {
	case MSG_SHM:
		if (fd >= 0)
			take_shm(fd);
		break;
	case MSG_DAMAGE:
		if ((size_t)n == sizeof(w.seq)) {
			memcpy(&w.seq, w.msg, sizeof(w.seq));
			w.damage = 1;
			set_cursor();
		}
		break;
	case MSG_CLIPBOARD: {
		char *text = malloc((size_t)n);
		long len = text ? base64_decode(w.msg, (size_t)n, text) : -1;
		if (len > 0)
			set_clipboard(text, (size_t)len);
		else
			free(text);
		break;
	}
	case MSG_TITLE:
		w.msg[n] = 0;
		xdg_toplevel_set_title(w.top, w.msg);
		break;
	case MSG_EXIT: {
		int32_t st;
		if ((size_t)n == sizeof(st)) {
			memcpy(&st, w.msg, sizeof(st));
			w.status =
			    w.closing || (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
		}
		w.ended = 1;
		break;
	}
	}
	if (fd >= 0)
		close(fd);
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
	w.held = MB_NONE;
	w.last_x = w.last_y = -1;
	if (w.seat)
		wl_seat_add_listener(w.seat, &seat_listener, NULL);
	if (w.seat && w.ddm) {
		w.dd = wl_data_device_manager_get_data_device(w.ddm, w.seat);
		wl_data_device_add_listener(w.dd, &dd_listener, NULL);
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
		struct pollfd p[6] = {
		    {wl_display_get_fd(w.dpy), POLLIN, 0},
		    {w.sock, POLLIN, 0},
		    {w.theme ? tdraw_theme_fd(w.theme) : -1, POLLIN, 0},
		    {w.blink, POLLIN, 0},
		    {w.rep, POLLIN, 0},
		    {w.paste_fd, POLLIN, 0}};
		if (poll(p, 6, -1) < 0 && errno != EINTR) {
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
		if (w.sock >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
			from_term();
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
		draw();
	}
}

int win_run(const struct cli *c) {
	w.sock = -1;
	w.paste_fd = -1;
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
	if (open_window(c) == 0)
		loop(c);
#ifdef FROST_TEST
	dump();
#endif
	if (w.sock >= 0)
		close(w.sock);
	if (w.term > 0)
		waitpid(w.term, NULL, WNOHANG);
	if (w.map)
		munmap(w.map, w.map_size);
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
