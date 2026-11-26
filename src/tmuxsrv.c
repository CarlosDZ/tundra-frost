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
		return "0";
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
	char *sh[] = {"/bin/sh", "-c", NULL, NULL};
	char **argv = NULL;
	if (c->argc == 1)
		sh[2] = c->argv[0], argv = sh;
	else if (c->argc > 1)
		argv = c->argv;
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
		w.full |= line_shown();
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

static int order(struct run *r, int argc, char **argv) {
	static const struct {
		const char *name;
		int (*fn)(struct run *, const struct tmux_cmd *);
	} orders[] = {
	    {"display-message", display}, {"list-panes", list},
	    {"split-window", split},      {"kill-pane", kill_pane},
	    {"select-pane", select_pane}, {"select-layout", select_layout},
	    {"resize-pane", resize}};
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
