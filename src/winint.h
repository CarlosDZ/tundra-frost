#ifndef FROST_WININT_H
#define FROST_WININT_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-client.h>

#include "kbd.h"
#include "line.h"
#include "links.h"
#include "mosaic.h"
#include "proto.h"
#include "render.h"

#define PANES_MAX 32

struct pane {
	int sock;
	uint8_t *map;
	size_t map_size;
	uint32_t seq;
	int damage, closing, marked;
	unsigned id;
	unsigned group;
	unsigned number;
	char title[TERM_TITLE_MAX], name[TERM_NAME_MAX];
	char cwd[PATH_MAX];
	char program[32];
	struct box box;
	uint8_t *stale[2];
};

struct group {
	unsigned id;
	enum layout layout;
	int main;
	int titles;
	unsigned focus;
};

struct buffer {
	struct wl_buffer *wl;
	struct tdraw_canvas *cv;
	int busy;
};

struct win {
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
	int held, swallow;
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

	int link_kind, link_from, link_to, link_id, link_click, link_waiting;
	char link_url[LINK_MAX];

	int next_w, next_h, next_focus, configured, focused;
	struct buffer buf[2], *shown;
	void *pool;
	size_t pool_size;
	int bw, bh;
	struct wl_callback *frame;

	struct pane panes[PANES_MAX];
	int npanes, cur, ended, status;
	struct group groups[PANES_MAX];
	int ngroups;
	unsigned next_pane, next_group;
	int cols, rows;
	int told_cw, told_ch;
	int full;

	int line_open, line_error;
	char line_input[LINE_INPUT_MAX], line_msg[256];

	struct render r;
	struct tdraw_theme *theme;
	int blink, blinking, lit, tick;
	int cur_y, cur_drawn;

	struct kbd kbd;
	int rep, rep_rate, rep_delay;
	uint32_t rep_key;
	char msg[MSG_MAX];
};

extern struct win w;

#define T (w.panes[w.cur])

void say(int error, const char *fmt, ...);
void show_title(void);
int line_shown(void);
void set_cursor(void);
void set_clipboard(char *text, size_t len);
void open_url(const char *url);
void cut_copy(char *dst, size_t size, const char *src);

int group_index(unsigned id);
int pane_index(unsigned id);
int group_panes(int gi, int *out);
int group_focus(int gi);
unsigned group_new(void);
int pane_new(unsigned group, int after, char **argv, const char *cwd,
             int focus);
void pane_drop(int i, int status);
void pane_focus(int i);
void relayout(void);
void from_term(int i);
int term_rows(void);
void list_terminals(void);
int detach_pane(const char *name);
int attach_pane(const char *arg, int number);

#endif
