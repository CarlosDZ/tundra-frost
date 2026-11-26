#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "tmuxcmd.h"
#include "width.h"
#include "winint.h"

#define SESSION "frost"
#define ARGS_MAX 256

struct run {
	int caller;
	const char *cwd;
	char out[MSG_MAX / 2];
	size_t nout;
	char err[512];
};

struct fmt {
	int pane;
	char num[32];
};

static const char *num(struct fmt *f, const char *pre, long v) {
	snprintf(f->num, sizeof(f->num), "%s%ld", pre, v);
	return f->num;
}

static int index_in_group(int i) {
	int k = 0;
	for (int j = 0; j < i; j++)
		k += w.panes[j].group == w.panes[i].group;
	return k;
}

static const char *var(void *ctx, const char *name) {
	struct fmt *f = ctx;
	if (!strcmp(name, "session_name"))
		return SESSION;
	if (!strcmp(name, "session_id"))
		return "$0";
	if (!strcmp(name, "version"))
		return TMUX_VERSION;
	if (!strcmp(name, "client_control_mode"))
		return "0";
	if (f->pane < 0 || f->pane >= w.npanes)
		return NULL;
	const struct pane *t = &w.panes[f->pane];
	int gi = group_index(t->group), idx[PANES_MAX];
	if (!strcmp(name, "pane_id"))
		return num(f, "%", t->id);
	if (!strcmp(name, "pane_index"))
		return num(f, "", index_in_group(f->pane));
	if (!strcmp(name, "pane_title"))
		return t->title;
	if (!strcmp(name, "pane_current_command"))
		return t->program;
	if (!strcmp(name, "pane_current_path"))
		return t->cwd;
	if (!strcmp(name, "pane_dead"))
		return t->dead ? "1" : "0";
	if (!strcmp(name, "pane_active"))
		return group_focus(gi) == f->pane ? "1" : "0";
	if (!strcmp(name, "pane_left"))
		return num(f, "", t->box.x);
	if (!strcmp(name, "pane_top"))
		return num(f, "", t->box.y);
	if (!strcmp(name, "pane_width"))
		return num(f, "", t->box.cols);
	if (!strcmp(name, "pane_height"))
		return num(f, "", t->box.rows);
	if (!strcmp(name, "window_id"))
		return num(f, "@", t->group);
	if (!strcmp(name, "window_index"))
		return num(f, "", gi + 1);
	if (!strcmp(name, "window_name"))
		return t->program;
	if (!strcmp(name, "window_active"))
		return t->group == T.group ? "1" : "0";
	if (!strcmp(name, "window_panes"))
		return num(f, "", group_panes(gi, idx));
	if (!strcmp(name, "window_width"))
		return num(f, "", w.cols);
	if (!strcmp(name, "window_height"))
		return num(f, "", term_rows());
	return NULL;
}

static void put(struct run *r, const char *format, int pane) {
	struct fmt f = {pane, ""};
	char line[4096];
	size_t n = tmux_format(format, var, &f, line, sizeof(line));
	if (r->nout + n + 1 > sizeof(r->out))
		return;
	memcpy(r->out + r->nout, line, n);
	r->nout += n;
	r->out[r->nout++] = '\n';
}

static int fail(struct run *r, const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vsnprintf(r->err, sizeof(r->err), format, ap);
	va_end(ap);
	return 1;
}

static int unsupported(struct run *r, const char *order, char flag) {
	return fail(r, "frost: %s -%c is not supported", order, flag);
}

static int caller(const struct run *r) {
	return r->caller >= 0 ? r->caller : w.npanes ? w.cur : -1;
}

static int number(const char *s, unsigned *out) {
	char *end;
	unsigned long v = strtoul(s, &end, 10);
	if (!*s || *end || v > 99999)
		return -1;
	*out = (unsigned)v;
	return 0;
}

static int group_by(const char *s) {
	unsigned v;
	if (*s == '@')
		return number(s + 1, &v) < 0 ? -1 : group_index(v);
	return number(s, &v) < 0 || v < 1 || (int)v > w.ngroups ? -1 : (int)v - 1;
}

static int pane_in(int gi, const char *s) {
	unsigned v;
	if (!*s)
		return group_focus(gi);
	if (*s == '%') {
		int i = number(s + 1, &v) < 0 ? -1 : pane_index(v);
		return i >= 0 && w.panes[i].group == w.groups[gi].id ? i : -1;
	}
	int idx[PANES_MAX], n = group_panes(gi, idx);
	return number(s, &v) < 0 || (int)v >= n ? -1 : idx[v];
}

static int find_pane(const struct run *r, const char *t) {
	unsigned v;
	if (!t || !*t)
		return caller(r);
	if (t[0] == '%')
		return number(t + 1, &v) < 0 ? -1 : pane_index(v);
	if (!strcmp(t, SESSION))
		return w.npanes ? w.cur : -1;
	const char *colon = strchr(t, ':'), *win = colon ? colon + 1 : t;
	if (colon && colon != t &&
	    ((size_t)(colon - t) != strlen(SESSION) ||
	     strncmp(t, SESSION, strlen(SESSION))))
		return -1;
	char part[32];
	size_t n = strcspn(win, ".");
	if (n >= sizeof(part))
		return -1;
	memcpy(part, win, n);
	part[n] = 0;
	int c = caller(r);
	int gi = *part    ? group_by(part)
	         : c >= 0 ? group_index(w.panes[c].group)
	                  : -1;
	return gi < 0 ? -1 : pane_in(gi, win[n] ? win + n + 1 : "");
}

static int find_group(const struct run *r, const char *t) {
	if (t && t[0] == '@')
		return group_by(t);
	int p = find_pane(r, t);
	return p >= 0 ? group_index(w.panes[p].group) : -1;
}

static int percent(const char *v, int total, int *out) {
	char *end;
	long n = strtol(v, &end, 10);
	if (end == v || n < 0)
		return -1;
	if (*end == '%' && !end[1])
		*out = n > 100 ? 100 : (int)n;
	else if (!*end && total > 0)
		*out = n >= total ? 100 : (int)(n * 100 / total);
	else
		return -1;
	return 0;
}

static int display(struct run *r, const struct tmux_cmd *c) {
	const char *t = tmux_value(c, 't');
	int p = find_pane(r, t);
	if (p < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	const char *format = c->argc ? c->argv[0] : "";
	if (tmux_flag(c, 'p')) {
		put(r, format, p);
		return 0;
	}
	struct fmt f = {p, ""};
	char line[256];
	tmux_format(format, var, &f, line, sizeof(line));
	say(0, "%s", line);
	w.full = 1;
	return 0;
}

static int list(struct run *r, const struct tmux_cmd *c) {
	if (tmux_value(c, 'f'))
		return unsupported(r, c->name, 'f');
	const char *t = tmux_value(c, 't');
	const char *format = tmux_value(c, 'F');
	int all = tmux_flag(c, 'a') || tmux_flag(c, 's');
	int gi = all ? -1 : find_group(r, t);
	if (!all && gi < 0)
		return fail(r, "can't find window: %s", t ? t : "");
	for (int g = 0; g < w.ngroups; g++) {
		int idx[PANES_MAX], n = group_panes(g, idx);
		for (int k = 0; k < n && (all || g == gi); k++)
			put(r,
			    format ? format
			           : "#{pane_index}: [#{pane_width}x#{pane_height}] "
			             "#{pane_id}",
			    idx[k]);
	}
	return 0;
}

static char **command(const struct tmux_cmd *c, char **sh) {
	if (c->argc == 1) {
		sh[0] = "/bin/sh", sh[1] = "-c", sh[2] = c->argv[0], sh[3] = NULL;
		return sh;
	}
	return c->argc > 1 ? c->argv : NULL;
}

static int split(struct run *r, const struct tmux_cmd *c) {
	if (tmux_value(c, 'e'))
		return unsupported(r, c->name, 'e');
	if (tmux_flag(c, 'I'))
		return unsupported(r, c->name, 'I');
	const char *t = tmux_value(c, 't');
	int p = find_pane(r, t);
	if (p < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	int across = tmux_flag(c, 'h'), size = -1;
	const char *l = tmux_value(c, 'l');
	if (l && percent(l, across ? w.cols : term_rows(), &size) < 0)
		return fail(r, "size is not a number of cells or a percentage: %s", l);
	if (w.npanes == PANES_MAX)
		return fail(r, "no room for another pane");
	int gi = group_index(w.panes[p].group), idx[PANES_MAX];
	struct group *g = &w.groups[gi];
	unsigned gid = g->id;
	if (group_panes(gi, idx) == 1) {
		g->layout = across ? LAYOUT_MAIN_VERTICAL : LAYOUT_EVEN_VERTICAL;
		g->main = across && size >= 0 ? 100 - size : 50;
	}
	char *sh[4], **argv = command(c, sh);
	char cwd[PATH_MAX];
	cut_copy(cwd, sizeof(cwd),
	         tmux_value(c, 'c') ? tmux_value(c, 'c') : r->cwd);
	int i = pane_new(gid, tmux_flag(c, 'b') ? p : p + 1, argv, cwd,
	                 !tmux_flag(c, 'd'));
	if (i < 0)
		return fail(r, "cannot start the pane");
	if (tmux_flag(c, 'P'))
		put(r,
		    tmux_value(c, 'F')
		        ? tmux_value(c, 'F')
		        : "#{session_name}:#{window_index}.#{pane_index}",
		    i);
	return 0;
}

static int kill_pane(struct run *r, const struct tmux_cmd *c) {
	if (tmux_flag(c, 'a'))
		return unsupported(r, c->name, 'a');
	const char *t = tmux_value(c, 't');
	int p = find_pane(r, t);
	if (p < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	msg_send(w.panes[p].sock, MSG_CLOSE, NULL, 0, -1);
	w.panes[p].closing = 1;
	pane_drop(p, 0);
	return 0;
}

static int select_pane(struct run *r, const struct tmux_cmd *c) {
	for (const char *f = "DdeLlMmRU"; *f; f++)
		if (tmux_flag(c, *f))
			return unsupported(r, c->name, *f);
	const char *t = tmux_value(c, 't');
	int p = find_pane(r, t);
	if (p < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	const char *title = tmux_value(c, 'T');
	if (title) {
		cut_copy(w.panes[p].title, sizeof(w.panes[p].title), title);
		if (p == w.cur)
			show_title();
		w.full = 1;
		return 0;
	}
	w.groups[group_index(w.panes[p].group)].focus = w.panes[p].id;
	if (w.panes[p].group == T.group)
		pane_focus(p);
	return 0;
}

static int select_layout(struct run *r, const struct tmux_cmd *c) {
	static const struct {
		const char *name;
		enum layout layout;
	} names[] = {{"main-vertical", LAYOUT_MAIN_VERTICAL},
	             {"tiled", LAYOUT_TILED},
	             {"even-horizontal", LAYOUT_EVEN_HORIZONTAL},
	             {"even-vertical", LAYOUT_EVEN_VERTICAL}};
	for (const char *f = "Enop"; *f; f++)
		if (tmux_flag(c, *f))
			return unsupported(r, c->name, *f);
	const char *t = tmux_value(c, 't');
	int gi = find_group(r, t);
	if (gi < 0)
		return fail(r, "can't find window: %s", t ? t : "");
	if (c->argc) {
		size_t k = 0;
		while (k < sizeof(names) / sizeof(names[0]) &&
		       strcmp(names[k].name, c->argv[0]))
			k++;
		if (k == sizeof(names) / sizeof(names[0]))
			return fail(r, "frost: no layout %s", c->argv[0]);
		w.groups[gi].layout = names[k].layout;
	}
	relayout();
	return 0;
}

static int resize(struct run *r, const struct tmux_cmd *c) {
	for (const char *f = "DLMRTUZ"; *f; f++)
		if (tmux_flag(c, *f))
			return unsupported(r, c->name, *f);
	if (tmux_value(c, 'y'))
		return unsupported(r, c->name, 'y');
	const char *t = tmux_value(c, 't'), *x = tmux_value(c, 'x');
	int p = find_pane(r, t), pct;
	if (p < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	if (!x)
		return 0;
	if (percent(x, w.cols, &pct) < 0)
		return fail(r, "size is not a number of cells or a percentage: %s", x);
	int gi = group_index(w.panes[p].group), idx[PANES_MAX];
	if (group_panes(gi, idx) < 2)
		return 0;
	if (w.groups[gi].layout != LAYOUT_MAIN_VERTICAL)
		return fail(r, "frost: only main-vertical changes the width of a pane");
	w.groups[gi].main = idx[0] == p ? pct : 100 - pct;
	relayout();
	return 0;
}

static int respawn(struct run *r, const struct tmux_cmd *c) {
	if (tmux_value(c, 'e'))
		return unsupported(r, c->name, 'e');
	const char *t = tmux_value(c, 't');
	int p = find_pane(r, t);
	if (p < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	if (!w.panes[p].dead && !tmux_flag(c, 'k'))
		return fail(r, "pane %%%u still active", w.panes[p].id);
	char *sh[4];
	if (pane_respawn(p, command(c, sh), tmux_value(c, 'c')) < 0)
		return fail(r, "cannot start the pane");
	return 0;
}

enum { O_STYLE, O_ACTIVE, O_FORMAT, O_REMAIN, O_TITLES, O_IGNORED, O_FIXED };

static const struct option {
	const char *name;
	int type;
} options[] = {
    {"pane-active-border-style", O_ACTIVE},
    {"pane-border-format", O_FORMAT},
    {"pane-border-status", O_TITLES},
    {"pane-border-style", O_STYLE},
    {"remain-on-exit", O_REMAIN},
    {"window-active-style", O_IGNORED},
    {"window-style", O_IGNORED},
    {"default-shell", O_FIXED},
    {"prefix", O_FIXED},
};

#define NOPTIONS (sizeof(options) / sizeof(options[0]))

static const char *const remains[] = {NULL, "off", "on", "failed"};
static const char *const titles[] = {NULL, "off", "top"};

static const struct option *option_named(const char *name) {
	for (size_t k = 0; k < NOPTIONS; k++)
		if (!strcmp(options[k].name, name))
			return &options[k];
	return NULL;
}

static int word(const char *const *words, int n, const char *v) {
	for (int k = 1; k < n; k++)
		if (!strcmp(words[k], v))
			return k;
	return 0;
}

static struct opts *group_opts(const struct pane *t) {
	int gi = group_index(t->group);
	return gi >= 0 ? &w.groups[gi].o : &w.global;
}

static const char *pick(const char *a, const char *b, const char *c) {
	return *a ? a : *b ? b : *c ? c : NULL;
}

int pane_remain(int i) {
	const struct pane *t = &w.panes[i];
	int v = t->o.remain             ? t->o.remain
	        : group_opts(t)->remain ? group_opts(t)->remain
	                                : w.global.remain;
	return v ? v : REMAIN_OFF;
}

int group_titles(int gi) {
	int v = gi >= 0 && w.groups[gi].o.titles ? w.groups[gi].o.titles
	                                         : w.global.titles;
	return v == TITLES_TOP;
}

static const char *own_value(const struct opts *o, const struct option *op,
                             struct fmt *f) {
	switch (op->type) {
	case O_STYLE:
		return *o->border_style ? o->border_style : NULL;
	case O_ACTIVE:
		return *o->active_style ? o->active_style : NULL;
	case O_FORMAT:
		return *o->border_format ? o->border_format : NULL;
	case O_REMAIN:
		return remains[o->remain];
	case O_TITLES:
		return titles[o->titles];
	case O_FIXED:
		if (o != &w.global)
			return NULL;
		if (!strcmp(op->name, "prefix"))
			return "C-b";
		snprintf(f->num, sizeof(f->num), "%s",
		         getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
		return f->num;
	}
	return NULL;
}

static const char *fallback(const struct option *op) {
	switch (op->type) {
	case O_STYLE:
	case O_ACTIVE:
	case O_IGNORED:
		return "default";
	case O_FORMAT:
		return " #{pane_title} ";
	case O_REMAIN:
	case O_TITLES:
		return "off";
	}
	return NULL;
}

struct level {
	struct opts *o;
	int pane, window;
};

static int level_of(struct run *r, const struct tmux_cmd *c, int window,
                    struct level *l) {
	const char *t = tmux_value(c, 't');
	if (tmux_flag(c, 's'))
		return unsupported(r, c->name, 's');
	l->pane = find_pane(r, t);
	if (tmux_flag(c, 'g')) {
		l->o = &w.global, l->window = -1;
		return 0;
	}
	if (l->pane < 0)
		return fail(r, "can't find pane: %s", t ? t : "");
	l->window = group_index(w.panes[l->pane].group);
	l->o = tmux_flag(c, 'p') && !window ? &w.panes[l->pane].o
	                                    : &w.groups[l->window].o;
	return 0;
}

static int put_text(char *dst, size_t size, const char *v) {
	if (strlen(v) >= size)
		return -1;
	memcpy(dst, v, strlen(v) + 1);
	return 0;
}

static int set_option(struct run *r, const struct tmux_cmd *c) {
	for (const char *f = "aU"; *f; f++)
		if (tmux_flag(c, *f))
			return unsupported(r, c->name, *f);
	if (!c->argc)
		return fail(r, "an option is needed");
	const struct option *op = option_named(c->argv[0]);
	if (!op)
		return tmux_flag(c, 'q') ? 0
		                         : fail(r, "invalid option: %s", c->argv[0]);
	if (op->type == O_FIXED)
		return fail(r, "frost: %s cannot be set", op->name);
	struct level l;
	struct fmt f = {-1, ""};
	if (level_of(r, c, op->type == O_TITLES, &l))
		return 1;
	if (tmux_flag(c, 'o') && own_value(l.o, op, &f))
		return tmux_flag(c, 'q') ? 0 : fail(r, "already set: %s", op->name);
	const char *v = c->argc > 1 ? c->argv[1] : NULL;
	char expanded[FORMAT_MAX];
	if (v && tmux_flag(c, 'F')) {
		f.pane = l.pane;
		tmux_format(v, var, &f, expanded, sizeof(expanded));
		v = expanded;
	}
	if (!v && !tmux_flag(c, 'u'))
		return fail(r, "a value is needed for %s", op->name);
	int bad = 0;
	switch (op->type) {
	case O_STYLE:
		bad = put_text(l.o->border_style, STYLE_MAX, v ? v : "");
		break;
	case O_ACTIVE:
		bad = put_text(l.o->active_style, STYLE_MAX, v ? v : "");
		break;
	case O_FORMAT:
		bad = put_text(l.o->border_format, FORMAT_MAX, v ? v : "");
		break;
	case O_REMAIN:
		l.o->remain = v ? word(remains, 4, v) : REMAIN_UNSET;
		bad = v && !l.o->remain;
		break;
	case O_TITLES:
		if (v && !strcmp(v, "bottom"))
			return fail(r, "frost: the titles go on top");
		l.o->titles = v ? word(titles, 3, v) : TITLES_UNSET;
		bad = v && !l.o->titles;
		break;
	}
	if (bad)
		return fail(r, "bad value for %s: %s", op->name, v);
	relayout();
	return 0;
}

static void show_one(struct run *r, const struct option *op, const char *v,
                     int bare) {
	char line[FORMAT_MAX + 64];
	int plain = *v && !strpbrk(v, " \t\"#");
	if (bare)
		snprintf(line, sizeof(line), "%s", v);
	else
		snprintf(line, sizeof(line), plain ? "%s %s" : "%s \"%s\"", op->name,
		         v);
	size_t n = strlen(line);
	if (r->nout + n + 1 > sizeof(r->out))
		return;
	memcpy(r->out + r->nout, line, n);
	r->nout += n;
	r->out[r->nout++] = '\n';
}

static int show_options(struct run *r, const struct tmux_cmd *c) {
	if (tmux_flag(c, 'H'))
		return unsupported(r, c->name, 'H');
	const struct option *only = NULL;
	if (c->argc && !(only = option_named(c->argv[0])))
		return tmux_flag(c, 'q') ? 0
		                         : fail(r, "invalid option: %s", c->argv[0]);
	struct level l;
	if (level_of(r, c, only && only->type == O_TITLES, &l))
		return 1;
	for (size_t k = 0; k < NOPTIONS; k++) {
		const struct option *op = &options[k];
		if (only && op != only)
			continue;
		struct fmt f = {l.pane, ""};
		const char *v = own_value(l.o, op, &f);
		if (!v && tmux_flag(c, 'A') && l.pane >= 0) {
			const struct pane *t = &w.panes[l.pane];
			v = own_value(&t->o, op, &f);
			v = v ? v : own_value(group_opts(t), op, &f);
		}
		if (!v && tmux_flag(c, 'A'))
			v = own_value(&w.global, op, &f);
		if (!v && tmux_flag(c, 'A'))
			v = fallback(op);
		if (v)
			show_one(r, op, v, tmux_flag(c, 'v'));
	}
	return 0;
}

struct title {
	struct cell *row;
	int cols, x;
};

static size_t utf8_next(const char *s, size_t n, uint32_t *cp) {
	const unsigned char *u = (const unsigned char *)s;
	int need = u[0] < 0x80    ? 0
	           : u[0] >= 0xF5 ? -1
	           : u[0] >= 0xF0 ? 3
	           : u[0] >= 0xE0 ? 2
	           : u[0] >= 0xC2 ? 1
	                          : -1;
	if (need < 0 || (size_t)need >= n) {
		*cp = u[0] < 0x80 ? u[0] : 0xFFFD;
		return 1;
	}
	uint32_t v = need ? u[0] & (0x3Fu >> need) : u[0];
	for (int k = 1; k <= need; k++) {
		if ((u[k] & 0xC0) != 0x80) {
			*cp = 0xFFFD;
			return 1;
		}
		v = v << 6 | (u[k] & 0x3F);
	}
	*cp = v;
	return (size_t)need + 1;
}

static void title_piece(void *ctx, const char *s, size_t n,
                        const struct tmux_style *st) {
	struct title *tc = ctx;
	uint16_t attr = st->bold ? ATTR_BOLD : 0;
	for (size_t k = 0; k < n && tc->x < tc->cols;) {
		uint32_t cp;
		k += utf8_next(s + k, n - k, &cp);
		int wd = cp < 0x20 ? 0 : width_of(cp);
		if (wd <= 0)
			continue;
		if (tc->x + wd > tc->cols)
			break;
		tc->row[tc->x] = (struct cell){
		    .cp = cp, .fg = st->fg, .attr = wd == 2 ? attr | ATTR_WIDE : attr};
		if (wd == 2)
			tc->row[tc->x + 1] =
			    (struct cell){.fg = st->fg, .attr = ATTR_WIDE_TAIL};
		tc->x += wd;
	}
}

int tmux_title(int i, int active, uint32_t fg, struct cell *row, int cols) {
	const struct pane *t = &w.panes[i];
	const struct opts *g = group_opts(t);
	const char *spec =
	    active
	        ? pick(t->o.active_style, g->active_style, w.global.active_style)
	        : pick(t->o.border_style, g->border_style, w.global.border_style);
	struct tmux_style base = {fg, 0}, st = base;
	if (spec)
		tmux_style(spec, &base, &st);
	for (int x = 0; x < cols; x++)
		row[x] = (struct cell){
		    .cp = 0x2500, .fg = st.fg, .attr = st.bold ? ATTR_BOLD : 0};
	const char *format =
	    pick(t->o.border_format, g->border_format, w.global.border_format);
	struct fmt f = {i, ""};
	struct title tc = {row, cols, cols > 1 ? 1 : 0};
	tmux_format_styled(format ? format : " #{pane_title} ", &st, var, &f,
	                   title_piece, &tc);
	return tc.x;
}

static int order(struct run *r, int argc, char **argv) {
	static const struct {
		const char *name;
		int (*fn)(struct run *, const struct tmux_cmd *);
	} orders[] = {
	    {"display-message", display},  {"list-panes", list},
	    {"split-window", split},       {"kill-pane", kill_pane},
	    {"select-pane", select_pane},  {"select-layout", select_layout},
	    {"resize-pane", resize},       {"respawn-pane", respawn},
	    {"set-option", set_option},    {"set-window-option", set_option},
	    {"show-options", show_options}};
	struct tmux_cmd c;
	if (tmux_parse(argc, argv, &c) < 0)
		return fail(r, "frost: tmux cannot read the order %s", argv[0]);
	for (size_t k = 0; k < sizeof(orders) / sizeof(orders[0]); k++)
		if (!strcmp(orders[k].name, c.name))
			return orders[k].fn(r, &c);
	return fail(r, "frost: tmux %s is not supported", c.name);
}

static void serve(int fd, char *req, size_t n) {
	static struct run r;
	static char rep[MSG_MAX];
	char *argv[ARGS_MAX + 1], *p = req, *end = req + n;
	int argc = 0;
	const char *strs[2];
	for (int k = 0; k < 2; k++) {
		strs[k] = p;
		p += strlen(p) + 1;
		if (p > end)
			return;
	}
	while (p < end && argc < ARGS_MAX) {
		argv[argc++] = p;
		p += strlen(p) + 1;
	}
	argv[argc] = NULL;
	unsigned id;
	r.caller = strs[0][0] == '%' && number(strs[0] + 1, &id) == 0
	               ? pane_index(id)
	               : -1;
	r.cwd = strs[1];
	r.nout = 0;
	r.err[0] = 0;
	int code = argc ? order(&r, argc, argv) : fail(&r, "an order is needed");
	struct msg_tmux_done d = {code, (uint32_t)r.nout};
	size_t e = strlen(r.err);
	if (e && e + 1 < sizeof(r.err))
		r.err[e++] = '\n';
	memcpy(rep, &d, sizeof(d));
	memcpy(rep + sizeof(d), r.out, r.nout);
	memcpy(rep + sizeof(d) + r.nout, r.err, e);
	msg_send(fd, MSG_TMUX_DONE, rep, sizeof(d) + r.nout + e, -1);
}

int tmux_listen(void) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	char dir[sizeof(a.sun_path)];
	if (!rt || snprintf(dir, sizeof(dir), "%s/frost", rt) >= (int)sizeof(dir) ||
	    snprintf(a.sun_path, sizeof(a.sun_path), "%s/tmux-%d", dir,
	             (int)getpid()) >= (int)sizeof(a.sun_path))
		return -1;
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
		perror("mkdir error");
		return -1;
	}
	unlink(a.sun_path);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    listen(fd, 8) < 0) {
		perror("tmux socket error");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	w.ctl = fd;
	memcpy(w.ctl_path, a.sun_path, sizeof(w.ctl_path));
	return 0;
}

void tmux_accept(void) {
	static char req[MSG_MAX];
	int fd = accept4(w.ctl, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return;
	struct timeval tv = {0, 300000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	uint32_t type;
	int got;
	ssize_t n = msg_recv(fd, &type, req, sizeof(req) - 1, &got);
	if (got >= 0)
		close(got);
	if (n > 0 && type == MSG_TMUX) {
		req[n] = 0;
		serve(fd, req, (size_t)n);
	}
	close(fd);
}

void tmux_close(void) {
	if (w.ctl < 0)
		return;
	close(w.ctl);
	unlink(w.ctl_path);
	w.ctl = -1;
}
