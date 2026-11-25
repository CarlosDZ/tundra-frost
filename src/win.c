#include "win.h"

#include <errno.h>
#include <fcntl.h>
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

#include "proto.h"
#include "render.h"
#include "shm.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FONT_SIZE 24
#define BLINK_MS 500
#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

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
	if (h->magic != SHM_MAGIC || h->version != SHM_VERSION ||
	    h->block_off != SHM_HEAD_SIZE ||
	    w.map_size < shm_size(h->cols, h->rows))
		return -1;
	struct cell *cells[2];
	uint16_t *maps[2];
	uint8_t *lines[2];
	scr_block_layout(w.map + h->block_off, h->cols, h->rows, cells, maps,
	                 lines);
	int g = h->alt ? 1 : 0;
	*v = (struct view){h->cols,
	                   h->rows,
	                   cells[g],
	                   maps[g],
	                   h->x,
	                   h->y,
	                   h->pending,
	                   h->cursor_visible,
	                   h->cursor_shape,
	                   h->cursor_blink,
	                   h->reverse,
	                   h->palette,
	                   h->palette_set};
	return 0;
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
	const struct shm_head *h = (const struct shm_head *)w.map;
	const uint8_t *dirty = w.map + h->dirty_off;
	int rows = v.rows < w.rows ? v.rows : w.rows;

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
		}
		break;
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
		struct pollfd p[4] = {
		    {wl_display_get_fd(w.dpy), POLLIN, 0},
		    {w.sock, POLLIN, 0},
		    {w.theme ? tdraw_theme_fd(w.theme) : -1, POLLIN, 0},
		    {w.blink, POLLIN, 0}};
		if (poll(p, 4, -1) < 0 && errno != EINTR) {
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
		draw();
	}
}

int win_run(const struct cli *c) {
	w.sock = -1;
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
	if (w.blink < 0)
		perror("timerfd error");
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
	render_free(&w.r);
	tdraw_theme_free(w.theme);
	return w.status;
}
