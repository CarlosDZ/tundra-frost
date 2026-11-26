#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "links.h"
#include "paste.h"
#include "shm.h"
#include "winint.h"

static const char *term_path(void) {
#ifdef FROST_TEST
	static char abs[PATH_MAX];
	const char *p = getenv("FROST_TEST_TERM");
	if (p && *p)
		return realpath(p, abs) ? abs : p;
#endif
	return TUNDRA_FROST_TERM;
}

static const char *bin_path(void) {
#ifdef FROST_TEST
	static char abs[PATH_MAX];
	const char *p = getenv("FROST_TEST_BIN");
	if (p && *p)
		return realpath(p, abs) ? abs : p;
#endif
	return TUNDRA_FROST_BIN;
}

static void tmux_env(const struct pane *t) {
	char v[PATH_MAX + 64];
	snprintf(v, sizeof(v), "%s,%d,0", w.ctl_path, (int)getppid());
	setenv("TMUX", v, 1);
	snprintf(v, sizeof(v), "%%%u", t->id);
	setenv("TMUX_PANE", v, 1);
	setenv("TERM_PROGRAM", "tmux", 1);
	setenv("TERM_PROGRAM_VERSION", TMUX_VERSION, 1);
	const char *old = getenv("PATH");
	snprintf(v, sizeof(v), "%s:%s", bin_path(), old ? old : "/usr/bin:/bin");
	setenv("PATH", v, 1);
}

int term_rows(void) {
	return w.ngroups >= 2 && w.rows > 1 ? w.rows - 1 : w.rows;
}

int group_index(unsigned id) {
	for (int i = 0; i < w.ngroups; i++)
		if (w.groups[i].id == id)
			return i;
	return -1;
}

int pane_index(unsigned id) {
	for (int i = 0; i < w.npanes; i++)
		if (w.panes[i].id == id)
			return i;
	return -1;
}

int group_panes(int gi, int *out) {
	int n = 0;
	for (int i = 0; gi >= 0 && i < w.npanes; i++)
		if (w.panes[i].group == w.groups[gi].id)
			out[n++] = i;
	return n;
}

int group_focus(int gi) {
	int idx[PANES_MAX], n = group_panes(gi, idx);
	int f = gi >= 0 ? pane_index(w.groups[gi].focus) : -1;
	return f >= 0 && w.panes[f].group == w.groups[gi].id ? f : n ? idx[0] : -1;
}

unsigned group_new(void) {
	struct group *g = &w.groups[w.ngroups++];
	*g = (struct group){w.next_group++, LAYOUT_MAIN_VERTICAL, 50, 0, 0};
	return g->id;
}

static void tell_size(struct pane *t) {
	struct msg_resize rs = {(uint16_t)t->box.cols, (uint16_t)t->box.rows,
	                        (uint16_t)w.r.cw, (uint16_t)w.r.ch};
	if (t->sock >= 0)
		msg_send(t->sock, MSG_RESIZE, &rs, sizeof(rs), -1);
}

void relayout(void) {
	int rows = term_rows();
	if (w.cols <= 0 || rows <= 0)
		return;
	int cell = w.told_cw != w.r.cw || w.told_ch != w.r.ch;
	w.told_cw = w.r.cw, w.told_ch = w.r.ch;
	for (int g = 0; g < w.ngroups; g++) {
		int idx[PANES_MAX], n = group_panes(g, idx);
		struct box b[PANES_MAX];
		mosaic_layout(w.cols, rows, n, w.groups[g].layout, w.groups[g].main,
		              w.groups[g].titles && n > 1, b);
		for (int k = 0; k < n; k++) {
			struct pane *t = &w.panes[idx[k]];
			int resized = t->box.cols != b[k].cols || t->box.rows != b[k].rows;
			t->box = b[k];
			for (int s = 0; s < 2 && (resized || !t->stale[s]); s++) {
				free(t->stale[s]);
				t->stale[s] = malloc((size_t)b[k].rows);
				if (t->stale[s])
					memset(t->stale[s], 1, (size_t)b[k].rows);
			}
			if (resized || cell)
				tell_size(t);
		}
	}
	w.full = 1;
}

static void tell_focus(int i, int in) {
	struct pane *t = &w.panes[i];
	const struct shm_head *h = (const struct shm_head *)t->map;
	if (w.focused && t->sock >= 0 && h && h->magic == SHM_MAGIC &&
	    h->version == SHM_VERSION && h->focus_events)
		msg_send(t->sock, MSG_INPUT, in ? "\x1b[I" : "\x1b[O", 3, -1);
}

static void focus_to(int i, int from) {
	if (i < 0 || i >= w.npanes)
		return;
	if (from >= 0 && from < w.npanes && from != i)
		tell_focus(from, 0);
	w.cur = i;
	for (int k = 0; k < w.npanes; k++)
		if (w.panes[k].group == T.group)
			w.panes[k].marked = 0;
	int gi = group_index(T.group);
	if (gi >= 0)
		w.groups[gi].focus = T.id;
	if (from != i)
		tell_focus(i, 1);
	w.sel_on = w.selecting = 0;
	w.link_kind = LINK_NONE;
	w.full = 1;
	show_title();
}

void pane_focus(int i) { focus_to(i, w.cur); }

static void program_of(struct pane *t, char **argv) {
	const char *p = argv && argv[0] ? argv[0] : getenv("SHELL");
	if (argv && argv[0] && argv[1] && !strcmp(argv[1], "-c") && argv[2])
		p = argv[2];
	if (!p || !*p)
		p = "sh";
	size_t n = strcspn(p, " ");
	const char *slash = memrchr(p, '/', n);
	if (slash)
		n -= (size_t)(slash + 1 - p), p = slash + 1;
	snprintf(t->program, sizeof(t->program), "%.*s", (int)n, p);
	memcpy(t->title, t->program, strlen(t->program) + 1);
}

static int spawn(struct pane *t, char **argv, const char *cwd) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
		perror("socketpair error");
		return -1;
	}
	const char *path = term_path();
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork error");
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (pid == 0) {
		if (sv[1] == 3 ? fcntl(3, F_SETFD, 0) < 0 : dup2(sv[1], 3) < 0)
			_exit(127);
		if (cwd && *cwd && chdir(cwd) < 0)
			perror("chdir error");
		if (w.ctl >= 0)
			tmux_env(t);
		char cols[16], rows[16];
		snprintf(cols, sizeof(cols), "%d", t->box.cols > 0 ? t->box.cols : 80);
		snprintf(rows, sizeof(rows), "%d", t->box.rows > 0 ? t->box.rows : 24);
		int n = 0;
		while (argv && argv[n])
			n++;
		char **args = calloc((size_t)n + 5, sizeof(*args));
		if (!args)
			_exit(127);
		args[0] = (char *)path, args[1] = "3";
		args[2] = cols, args[3] = rows;
		for (int i = 0; i < n; i++)
			args[4 + i] = argv[i];
		execv(path, args);
		fprintf(stderr, "frost: cannot run %s\n", path);
		_exit(127);
	}
	close(sv[1]);
	t->sock = sv[0];
	return 0;
}

int pane_new(unsigned group, int at, char **argv, const char *cwd, int focus) {
	if (w.npanes == PANES_MAX)
		return -1;
	int i = at < 0 || at > w.npanes ? w.npanes : at;
	memmove(&w.panes[i + 1], &w.panes[i],
	        (size_t)(w.npanes - i) * sizeof(w.panes[0]));
	if (w.npanes && w.cur >= i)
		w.cur++;
	w.npanes++;
	struct pane *t = &w.panes[i];
	*t = (struct pane){.sock = -1, .id = w.next_pane++, .group = group};
	program_of(t, argv);
	if (cwd)
		cut_copy(t->cwd, sizeof(t->cwd), cwd);
	relayout();
	if (spawn(t, argv, t->cwd) < 0) {
		pane_drop(i, 1);
		return -1;
	}
	if (focus || w.npanes == 1)
		pane_focus(i);
	return i;
}

void pane_drop(int i, int status) {
	struct pane *t = &w.panes[i];
	unsigned gid = t->group;
	int was_cur = i == w.cur;
	if (t->sock >= 0)
		close(t->sock);
	if (t->map)
		munmap(t->map, t->map_size);
	free(t->stale[0]);
	free(t->stale[1]);
	memmove(&w.panes[i], &w.panes[i + 1],
	        (size_t)(w.npanes - i - 1) * sizeof(*t));
	w.npanes--;
	w.panes[w.npanes] = (struct pane){.sock = -1};
	if (w.cur > i)
		w.cur--;
	int gi = group_index(gid);
	int idx[PANES_MAX];
	if (gi >= 0 && !group_panes(gi, idx)) {
		memmove(&w.groups[gi], &w.groups[gi + 1],
		        (size_t)(w.ngroups - gi - 1) * sizeof(w.groups[0]));
		w.ngroups--;
		gi = gi < w.ngroups ? gi : w.ngroups - 1;
	}
	if (!w.npanes) {
		w.status = status;
		w.ended = 1;
		return;
	}
	if (w.cur >= w.npanes)
		w.cur = w.npanes - 1;
	relayout();
	if (was_cur) {
		int n = group_focus(gi >= 0 ? gi : 0);
		focus_to(n >= 0 ? n : w.cur, -1);
	}
}

static void take_shm(struct pane *t, int fd) {
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)SHM_HEAD_SIZE)
		return;
	if (t->map)
		munmap(t->map, t->map_size);
	t->map_size = (size_t)st.st_size;
	t->map = mmap(NULL, t->map_size, PROT_READ, MAP_SHARED, fd, 0);
	if (t->map == MAP_FAILED)
		t->map = NULL;
	if (t->group == T.group)
		w.full = 1;
}

static void ended(int i, int failed) {
	pane_drop(i, w.panes[i].closing || !failed ? 0 : 1);
}

void from_term(int i) {
	struct pane *t = &w.panes[i];
	uint32_t type;
	int fd;
	ssize_t n = msg_recv(t->sock, &type, w.msg, sizeof(w.msg) - 1, &fd);
	if (n < 0 || type == 0) {
		ended(i, 1);
		return;
	}
	int shown = t == &T, seen = t->group == T.group;
	switch (type) {
	case MSG_SHM:
		if (fd >= 0)
			take_shm(t, fd);
		break;
	case MSG_DAMAGE:
		if ((size_t)n == sizeof(t->seq)) {
			memcpy(&t->seq, w.msg, sizeof(t->seq));
			t->damage = 1;
			if (shown)
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
	case MSG_LINK:
		w.msg[n] = 0;
		if (shown && w.link_kind == LINK_OSC8) {
			cut_copy(w.link_url, sizeof(w.link_url), w.msg);
			w.full = 1;
			if (w.link_waiting)
				open_url(w.link_url);
			w.link_waiting = 0;
		}
		break;
	case MSG_INFO:
		if ((size_t)n == sizeof(struct msg_info)) {
			struct msg_info *in = (struct msg_info *)w.msg;
			in->name[sizeof(in->name) - 1] = 0;
			in->title[sizeof(in->title) - 1] = 0;
			t->number = in->number;
			snprintf(t->name, sizeof(t->name), "%s", in->name);
			if (in->title[0])
				snprintf(t->title, sizeof(t->title), "%s", in->title);
			w.full |= line_shown();
		}
		break;
	case MSG_CWD: {
		char host[256] = "", dir[PATH_MAX];
		w.msg[n] = 0;
		gethostname(host, sizeof(host) - 1);
		if (link_cwd(w.msg, host, dir, sizeof(dir)) == 0)
			memcpy(t->cwd, dir, strlen(dir) + 1);
		break;
	}
	case MSG_BELL:
	case MSG_NOTIFY:
		if (!seen) {
			t->marked = 1;
			w.full |= line_shown();
		}
		break;
	case MSG_BUSY:
		say(1, "terminal %u has a window", t->number);
		w.line_open = 1;
		pane_drop(i, 0);
		return;
	case MSG_TITLE:
		w.msg[n] = 0;
		cut_copy(t->title, sizeof(t->title), w.msg);
		if (shown)
			show_title();
		w.full |= line_shown();
		break;
	case MSG_EXIT: {
		int32_t st = 1;
		if ((size_t)n == sizeof(st))
			memcpy(&st, w.msg, sizeof(st));
		if (fd >= 0)
			close(fd);
		ended(i, !(WIFEXITED(st) && WEXITSTATUS(st) == 0));
		return;
	}
	}
	if (fd >= 0)
		close(fd);
}

static int dial_terminal(unsigned number) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	if (!rt || snprintf(a.sun_path, sizeof(a.sun_path), "%s/frost/%u", rt,
	                    number) >= (int)sizeof(a.sun_path))
		return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int by_number(const void *a, const void *b) {
	const struct msg_info *x = a, *y = b;
	return x->number < y->number ? -1 : x->number > y->number;
}

static int scan_terminals(struct msg_info *out, int max) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char dir[PATH_MAX];
	int n = 0;
	snprintf(dir, sizeof(dir), "%s/frost", rt ? rt : "");
	DIR *d = rt ? opendir(dir) : NULL;
	struct dirent *e;
	while (d && (e = readdir(d)) && n < max) {
		char *end;
		unsigned long v = strtoul(e->d_name, &end, 10);
		if (!*e->d_name || *end || !v || v > 99999)
			continue;
		int fd = dial_terminal((unsigned)v);
		struct timeval tv = {0, 300000};
		uint32_t type;
		int got;
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (msg_send(fd, MSG_QUERY, NULL, 0, -1) == 0 &&
		    msg_recv(fd, &type, &out[n], sizeof(out[n]), &got) ==
		        sizeof(out[n]) &&
		    type == MSG_INFO) {
			out[n].name[sizeof(out[n].name) - 1] = 0;
			out[n].title[sizeof(out[n].title) - 1] = 0;
			n++;
		}
		close(fd);
	}
	if (d)
		closedir(d);
	qsort(out, (size_t)n, sizeof(*out), by_number);
	return n;
}

void list_terminals(void) {
	static struct msg_info all[256];
	int n = scan_terminals(all, 256);
	char out[sizeof(w.line_msg)] = "";
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		int k = snprintf(out + o, sizeof(out) - o, "%s%u%s%s%s", o ? "  " : "",
		                 all[i].number, all[i].name[0] ? " " : "", all[i].name,
		                 all[i].attached ? "" : " detached");
		if (k > 0 && (size_t)k < sizeof(out) - o)
			o += (size_t)k;
	}
	if (o)
		say(0, "%s", out);
	else
		say(0, "no terminals");
}

int detach_pane(const char *name) {
	if (*name) {
		static struct msg_info all[256];
		int n = scan_terminals(all, 256);
		if (strlen(name) >= TERM_NAME_MAX) {
			say(1, "a name has at most %d letters", TERM_NAME_MAX - 1);
			return -1;
		}
		for (int i = 0; i < n; i++)
			if (!strcmp(all[i].name, name) && all[i].number != T.number) {
				say(1, "%s is the name of terminal %u", name, all[i].number);
				return -1;
			}
		msg_send(T.sock, MSG_NAME, name, strlen(name), -1);
	}
	msg_send(T.sock, MSG_DETACH, NULL, 0, -1);
	pane_drop(w.cur, 0);
	return 0;
}

int attach_pane(const char *arg, int number) {
	unsigned num = number > 0 ? (unsigned)number : 0;
	if (!num) {
		static struct msg_info all[256];
		int n = scan_terminals(all, 256);
		for (int i = 0; i < n && !num; i++)
			if (!strcmp(all[i].name, arg))
				num = all[i].number;
	}
	for (int i = 0; i < w.npanes && num; i++)
		if (w.panes[i].number == num) {
			pane_focus(i);
			return 0;
		}
	int fd = num && w.npanes < PANES_MAX ? dial_terminal(num) : -1;
	if (fd < 0) {
		say(1, "no terminal %s", arg);
		return -1;
	}
	unsigned g = group_new();
	struct pane *t = &w.panes[w.npanes];
	*t = (struct pane){.sock = -1, .id = w.next_pane++, .group = g};
	t->number = num;
	w.npanes++;
	relayout();
	struct msg_resize rs = {(uint16_t)t->box.cols, (uint16_t)t->box.rows,
	                        (uint16_t)w.r.cw, (uint16_t)w.r.ch};
	msg_send(fd, MSG_ATTACH, &rs, sizeof(rs), -1);
	t->sock = fd;
	pane_focus(w.npanes - 1);
	return 0;
}
