#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "proto.h"
#include "pty.h"
#include "sandbox.h"
#include "screen.h"
#include "shm.h"

#define HISTORY_BYTES (32u << 20)
#define INPUT_MAX (16u << 20)
#define SYNC_MAX_MS 150
#define TERMS_MAX 999
#define FIRST_MSG_MS 500

struct mapping {
	int fd;
	uint8_t *base;
	size_t size;
};

static struct term {
	struct scr scr;
	struct hist hist;
	int master, win, sig;
	pid_t pid;
	struct mapping cur;
	int waiting, changed, full, synced;
	uint8_t *inq;
	size_t inq_len, inq_cap;
	int offset;
	struct cell *line;
	int *starts;
	long long sync_since;
	int lsock, dirfd;
	unsigned number;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char leaf[16];
	char name[TERM_NAME_MAX];
	char buf[MSG_MAX];
} t;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct shm_head *head(void) { return (struct shm_head *)t.cur.base; }

static int shm_make(int cols, int rows) {
	struct shm_layout l = shm_layout(cols, rows);
	int fd = memfd_create("frost", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)l.size) < 0) {
		perror("memfd error");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	uint8_t *p = mmap(NULL, l.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap error");
		close(fd);
		return -1;
	}
	struct shm_head *h = (struct shm_head *)p;
	h->magic = SHM_MAGIC;
	h->version = SHM_VERSION;
	h->cols = (uint16_t)cols;
	h->rows = (uint16_t)rows;
	uint16_t *map = (uint16_t *)(p + l.map);
	for (int y = 0; y < rows; y++)
		map[y] = (uint16_t)y;
	if (t.cur.base) {
		h->seq = head()->seq;
		munmap(t.cur.base, t.cur.size);
		close(t.cur.fd);
	}
	t.cur = (struct mapping){fd, p, l.size};
	return 0;
}

static void to_window(uint32_t type, const void *data, size_t len) {
	if (t.win >= 0 && msg_send(t.win, type, data, len, -1) < 0) {
		close(t.win);
		t.win = -1;
	}
}

static void on_title(void *ctx, const char *title) {
	(void)ctx;
	to_window(MSG_TITLE, title, strlen(title));
}
static void on_bell(void *ctx) { (void)ctx, to_window(MSG_BELL, NULL, 0); }
static void on_clipboard(void *ctx, const char *b64, size_t len) {
	(void)ctx;
	to_window(MSG_CLIPBOARD, b64, len);
}
static void on_notify(void *ctx, const char *text, size_t len) {
	(void)ctx;
	to_window(MSG_NOTIFY, text, len);
}
static void on_cwd(void *ctx, const char *uri, size_t len) {
	(void)ctx;
	to_window(MSG_CWD, uri, len);
}
static void on_scrollout(void *ctx, const struct cell *c, int cols,
                         uint8_t flags) {
	(void)ctx;
	if (t.offset)
		t.offset++;
	hist_push(&t.hist, c, cols, flags);
}
static void on_seal(void *ctx) { (void)ctx, hist_seal(&t.hist); }
static void on_clear(void *ctx) {
	(void)ctx, hist_clear(&t.hist);
	t.offset = 0;
}

static int clamp_offset(long want) {
	if (want <= 0)
		return 0;
	long acc = 0;
	for (size_t i = hist_count(&t.hist); i > 0 && acc < want;)
		acc += hist_rows(&t.hist, --i, t.scr.cols);
	return (int)(acc < want ? acc : want > INT_MAX ? INT_MAX : want);
}

static int copy_view(struct cell *cells, uint8_t *lines) {
	struct scr *s = &t.scr;
	int cols = s->cols, rows = s->rows, y = 0;
	size_t n = hist_count(&t.hist), i = n;
	int acc = 0;
	if (!t.line)
		t.line = malloc(HIST_LINE_MAX * sizeof(*t.line));
	if (!t.starts)
		t.starts = malloc((HIST_LINE_MAX + 1) * sizeof(*t.starts));
	if (!t.line || !t.starts)
		t.offset = 0;
	while (i > 0 && acc < t.offset)
		acc += hist_rows(&t.hist, --i, cols);
	if (acc < t.offset)
		t.offset = acc;
	int skip = acc - t.offset;
	for (; t.offset && i < n && y < rows; i++, skip = 0) {
		uint8_t fl;
		int k = hist_get(&t.hist, i, t.line, HIST_LINE_MAX, &fl);
		if (k < 0)
			break;
		int nr = split_rows(t.line, k, cols, t.starts, HIST_LINE_MAX + 1);
		for (int r = skip; r < nr && y < rows; r++, y++) {
			int a = t.starts[r], b = r + 1 < nr ? t.starts[r + 1] : k;
			struct cell *row = cells + (size_t)y * cols;
			memset(row, 0, (size_t)cols * sizeof(*row));
			memcpy(row, t.line + a, (size_t)(b - a) * sizeof(*row));
			lines[y] =
			    (uint8_t)((r + 1 < nr ? LINE_WRAPPED : 0) | (r == 0 ? fl : 0));
		}
	}
	int shown = y;
	struct grid *g = &s->grid[s->alt];
	for (int sy = 0; y < rows; y++, sy++) {
		memcpy(cells + (size_t)y * cols, g->cells + (size_t)g->map[sy] * cols,
		       (size_t)cols * sizeof(*cells));
		lines[y] = g->lines[g->map[sy]];
	}
	return shown;
}

static void publish(int all) {
	struct shm_head *h = head();
	struct scr *s = &t.scr;
	struct grid *g = &s->grid[s->alt];
	struct shm_layout l = shm_layout(s->cols, s->rows);
	struct cell *cells = (struct cell *)(t.cur.base + l.cells);
	uint8_t *lines = t.cur.base + l.lines, *dirty = t.cur.base + l.dirty;
	size_t row = (size_t)s->cols * sizeof(*cells);

	__atomic_store_n(&h->seq, h->seq + 1, __ATOMIC_RELEASE);
	if (s->alt)
		t.offset = 0;
	int shown = 0;
	if (t.offset || h->offset) {
		shown = copy_view(cells, lines);
		memset(dirty, 1, (size_t)s->rows);
		memset(s->dirty, 0, (size_t)s->rows);
	} else
		for (int y = 0; y < s->rows; y++) {
			if (!all && !s->dirty[y])
				continue;
			memcpy(cells + (size_t)y * s->cols,
			       g->cells + (size_t)g->map[y] * s->cols, row);
			lines[y] = g->lines[g->map[y]];
			dirty[y] = 1;
			s->dirty[y] = 0;
		}
	h->x = (uint16_t)s->x, h->y = (uint16_t)(s->y + shown);
	h->alt = (uint8_t)s->alt, h->pending = (uint8_t)s->pending;
	h->cursor_visible = (uint8_t)(s->cursor_visible && s->y + shown < s->rows);
	h->offset = (uint32_t)t.offset;
	h->cursor_shape = (uint8_t)s->cursor_shape;
	h->cursor_blink = (uint8_t)s->cursor_blink;
	h->reverse = (uint8_t)s->reverse, h->sync = (uint8_t)s->sync;
	h->focus_events = (uint8_t)s->focus_events;
	h->keys = (uint8_t)(g->nkeys ? g->keys[g->nkeys - 1] : 0);
	h->app_cursor = (uint8_t)s->app_cursor, h->newline = (uint8_t)s->newline;
	h->bracketed_paste = (uint8_t)s->bracketed_paste;
	h->mouse = (uint16_t)s->mouse, h->mouse_sgr = (uint8_t)s->mouse_sgr;
	h->mouse_pixels = (uint8_t)s->mouse_pixels;
	h->alt_scroll = (uint8_t)s->alt_scroll;
	memcpy(h->palette, s->palette, sizeof(h->palette));
	memcpy(h->palette_set, s->palette_set, sizeof(h->palette_set));
	__atomic_store_n(&h->seq, h->seq + 1, __ATOMIC_RELEASE);
}

static void notify(void) {
	if (t.scr.sync && !t.synced)
		t.sync_since = now_ms();
	t.synced = t.scr.sync;
	if (t.win < 0 || t.waiting || !t.changed)
		return;
	if (t.scr.sync && now_ms() - t.sync_since < SYNC_MAX_MS)
		return;
	publish(t.full);
	uint32_t seq = head()->seq;
	to_window(MSG_DAMAGE, &seq, sizeof(seq));
	t.waiting = t.win >= 0;
	t.changed = t.full = 0;
}

static size_t write_some(const uint8_t *p, size_t n) {
	size_t done = 0;
	while (done < n) {
		ssize_t k = write(t.master, p + done, n - done);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			break;
		done += (size_t)k;
	}
	return done;
}

static void flush_input(void) {
	size_t k = write_some(t.inq, t.inq_len);
	memmove(t.inq, t.inq + k, t.inq_len - k);
	t.inq_len -= k;
}

static void to_program(const uint8_t *p, size_t n) {
	size_t k = t.inq_len ? 0 : write_some(p, n);
	p += k, n -= k;
	if (!n)
		return;
	if (t.inq_len + n > INPUT_MAX)
		return;
	if (t.inq_len + n > t.inq_cap) {
		size_t cap = t.inq_cap ? t.inq_cap : 4096;
		while (cap < t.inq_len + n)
			cap *= 2;
		uint8_t *q = realloc(t.inq, cap);
		if (!q)
			return;
		t.inq = q, t.inq_cap = cap;
	}
	memcpy(t.inq + t.inq_len, p, n);
	t.inq_len += n;
}

static void feed(const uint8_t *p, size_t n) {
	scr_feed(&t.scr, p, n);
	t.changed = 1;
	if (t.scr.reply_len) {
		to_program(t.scr.reply, t.scr.reply_len);
		t.scr.reply_len = 0;
	}
}

static int read_pty(void) {
	ssize_t n = read(t.master, t.buf, sizeof(t.buf));
	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	if (n <= 0)
		return -1;
	feed((const uint8_t *)t.buf, (size_t)n);
	return 0;
}

static void send_info(int fd) {
	struct msg_info in = {0};
	in.number = t.number;
	in.attached = t.win >= 0;
	memcpy(in.name, t.name, sizeof(in.name));
	size_t k = strnlen(t.scr.title, sizeof(in.title) - 1);
	while (k > 0 && k < strlen(t.scr.title) &&
	       ((uint8_t)t.scr.title[k] & 0xC0) == 0x80)
		k--;
	memcpy(in.title, t.scr.title, k);
	msg_send(fd, MSG_INFO, &in, sizeof(in), -1);
}

static void send_shm(void) {
	if (t.win >= 0 && msg_send(t.win, MSG_SHM, NULL, 0, t.cur.fd) < 0) {
		close(t.win);
		t.win = -1;
	}
}

static void resize(const struct msg_resize *r) {
	if (r->cols < 1 || r->rows < 1)
		return;
	t.scr.cell_w = r->cell_w, t.scr.cell_h = r->cell_h;
	if (r->cols != t.scr.cols || r->rows != t.scr.rows) {
		t.offset = 0;
		if (shm_make(r->cols, r->rows) < 0)
			return;
		if (scr_resize(&t.scr, r->cols, r->rows, &t.hist) < 0) {
			shm_make(t.scr.cols, t.scr.rows);
			return;
		}
		send_shm();
		t.waiting = 0;
		t.changed = t.full = 1;
	}
	pty_resize(t.master, r->cols, r->rows, r->cell_w, r->cell_h);
}

static void hang_up(void) { kill(-t.pid, SIGHUP); }

static void detach(void) {
	if (t.win >= 0)
		close(t.win);
	t.win = -1;
	t.waiting = 0;
}

static void from_window(void) {
	uint32_t type;
	int fd;
	ssize_t n = msg_recv(t.win, &type, t.buf, sizeof(t.buf), &fd);
	if (fd >= 0)
		close(fd);
	if (n < 0 || type == 0) {
		detach();
		return;
	}
	switch (type) {
	case MSG_INPUT:
		to_program((uint8_t *)t.buf, (size_t)n);
		break;
	case MSG_RESIZE:
		if ((size_t)n == sizeof(struct msg_resize))
			resize((struct msg_resize *)t.buf);
		break;
	case MSG_ACK: {
		uint32_t seq;
		if ((size_t)n != sizeof(seq))
			break;
		memcpy(&seq, t.buf, sizeof(seq));
		if (seq != head()->seq)
			break;
		uint8_t *dirty = t.cur.base + shm_layout(t.scr.cols, t.scr.rows).dirty;
		memset(dirty, 0, (size_t)t.scr.rows);
		t.waiting = 0;
		break;
	}
	case MSG_DETACH:
		detach();
		break;
	case MSG_CLOSE:
		hang_up();
		break;
	case MSG_NAME:
		if ((size_t)n < sizeof(t.name)) {
			memcpy(t.name, t.buf, (size_t)n);
			t.name[n] = 0;
		}
		break;
	case MSG_LINK_ASK: {
		uint16_t id;
		if ((size_t)n != sizeof(id))
			break;
		memcpy(&id, t.buf, sizeof(id));
		if (id >= 1 && id <= t.scr.nlinks)
			to_window(MSG_LINK, t.scr.links[id - 1],
			          strlen(t.scr.links[id - 1]));
		break;
	}
	case MSG_SCROLL: {
		int32_t d;
		if ((size_t)n != sizeof(d) || t.scr.alt)
			break;
		memcpy(&d, t.buf, sizeof(d));
		int off = d == INT32_MIN ? 0 : clamp_offset((long)t.offset + d);
		if (off != t.offset) {
			t.offset = off;
			t.changed = 1;
		}
		break;
	}
	}
}

static int sock_dir(char *dir, size_t size) {
	const char *run = getenv("XDG_RUNTIME_DIR");
	if (!run || !*run)
		return -1;
	if ((size_t)snprintf(dir, size, "%s/frost", run) >= size)
		return -1;
	if (mkdir(dir, 0700) < 0 && errno != EEXIST)
		return -1;
	struct stat st;
	if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
	    (st.st_mode & 077)) {
		fprintf(stderr, "frost-term: %s is not private\n", dir);
		return -1;
	}
	return 0;
}

static void listen_numbered(void) {
	char dir[sizeof(t.path) - 8];
	t.lsock = -1;
	t.dirfd = -1;
	if (sock_dir(dir, sizeof(dir)) < 0)
		return;
	for (unsigned n = 1; n <= TERMS_MAX; n++) {
		struct sockaddr_un a = {.sun_family = AF_UNIX};
		snprintf(a.sun_path, sizeof(a.sun_path), "%s/%u", dir, n);
		int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (fd < 0)
			return;
		if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
		    listen(fd, 8) == 0) {
			t.lsock = fd;
			t.number = n;
			memcpy(t.path, a.sun_path, sizeof(t.path));
			snprintf(t.leaf, sizeof(t.leaf), "%u", n);
			t.dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			return;
		}
		int taken = errno == EADDRINUSE;
		close(fd);
		if (!taken)
			return;
		fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 &&
		    errno == ECONNREFUSED && unlink(a.sun_path) == 0)
			n--;
		if (fd >= 0)
			close(fd);
	}
}

static void accept_one(void) {
	int fd = accept4(t.lsock, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return;
	struct ucred cr;
	socklen_t len = sizeof(cr);
	struct pollfd p = {fd, POLLIN, 0};
	uint32_t type;
	int got;
	ssize_t n;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0 ||
	    cr.uid != getuid() || poll(&p, 1, FIRST_MSG_MS) != 1 ||
	    (n = msg_recv(fd, &type, t.buf, sizeof(t.buf), &got)) < 0) {
		close(fd);
		return;
	}
	if (got >= 0)
		close(got);
	if (type == MSG_QUERY) {
		send_info(fd);
	} else if (type == MSG_ATTACH && t.win < 0) {
		t.win = fd;
		int cols = t.scr.cols, rows = t.scr.rows;
		if ((size_t)n == sizeof(struct msg_resize))
			resize((struct msg_resize *)t.buf);
		if (t.scr.cols == cols && t.scr.rows == rows)
			send_shm();
		if (t.win >= 0)
			send_info(t.win);
		t.waiting = 0;
		t.changed = t.full = 1;
		return;
	} else if (type == MSG_ATTACH) {
		msg_send(fd, MSG_BUSY, NULL, 0, -1);
	}
	close(fd);
}

static int usage(void) {
	fprintf(stderr, "usage: frost-term fd cols rows [program [argument...]]\n");
	return 2;
}

static int number(const char *s, int lo, int hi) {
	char *end;
	long v = strtol(s, &end, 10);
	return *s && !*end && v >= lo && v <= hi ? (int)v : -1;
}

int main(int argc, char **argv) {
	if (argc < 4)
		return usage();
	t.win = number(argv[1], 0, 1 << 20);
	int cols = number(argv[2], 1, 65535);
	int rows = number(argv[3], 1, 65535);
	if (t.win < 0 || cols < 0 || rows < 0)
		return usage();
	char **cmd = argc > 4 ? argv + 4 : NULL;
	fcntl(t.win, F_SETFD, FD_CLOEXEC);

	setsid();
	signal(SIGPIPE, SIG_IGN);
	int null = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null >= 0) {
		dup2(null, 0);
		dup2(null, 1);
		if (null > 2)
			close(null);
	}

	sigset_t chld;
	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigprocmask(SIG_BLOCK, &chld, NULL);
	t.sig = signalfd(-1, &chld, SFD_CLOEXEC | SFD_NONBLOCK);
	if (t.sig < 0) {
		perror("signalfd error");
		return 1;
	}

	if (hist_init(&t.hist, HISTORY_BYTES) < 0 ||
	    scr_init(&t.scr, cols, rows) < 0 || shm_make(cols, rows) < 0) {
		fprintf(stderr, "frost-term: cannot make the screen\n");
		return 1;
	}
	t.scr.hooks = (struct scr_hooks){.bell = on_bell,
	                                 .title = on_title,
	                                 .cwd = on_cwd,
	                                 .clipboard = on_clipboard,
	                                 .notify = on_notify,
	                                 .scrollout = on_scrollout,
	                                 .clear_history = on_clear,
	                                 .seal = on_seal};
	t.master = pty_spawn(cmd, cols, rows, &t.pid);
	if (t.master < 0)
		return 1;
	fcntl(t.master, F_SETFL, O_NONBLOCK);
	listen_numbered();
#ifndef FROST_NO_SECCOMP
	if (sandbox_term(t.master, t.dirfd, t.pid) < 0)
		return 1;
#endif
	t.changed = t.full = 1;
	send_shm();
	if (t.win >= 0)
		send_info(t.win);
	notify();

	int pty_open = 1, status = 0;
	for (;;) {
		struct pollfd p[4] = {{t.sig, POLLIN, 0},
		                      {t.win, POLLIN, 0},
		                      {pty_open ? t.master : -1,
		                       (short)(POLLIN | (t.inq_len ? POLLOUT : 0)), 0},
		                      {t.lsock, POLLIN, 0}};
		int timeout = -1;
		if (!t.waiting && t.changed && t.scr.sync) {
			long long left = t.sync_since + SYNC_MAX_MS - now_ms();
			timeout = left > 0 ? (int)left : 0;
		}
		if (poll(p, 4, timeout) < 0 && errno != EINTR) {
			perror("poll error");
			return 1;
		}
		if (p[2].revents & POLLOUT)
			flush_input();
		if (p[2].revents & (POLLIN | POLLHUP | POLLERR))
			if (read_pty() < 0)
				pty_open = 0;
		if (t.win >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
			from_window();
		if (p[3].revents & POLLIN)
			accept_one();
		if (p[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(t.sig, &si, sizeof(si)) == sizeof(si))
				;
			if (waitpid(t.pid, &status, WNOHANG) == t.pid)
				break;
		}
		notify();
	}

	fcntl(t.master, F_SETFL, O_NONBLOCK);
	for (;;) {
		ssize_t n = read(t.master, t.buf, sizeof(t.buf));
		if (n <= 0)
			break;
		feed((const uint8_t *)t.buf, (size_t)n);
	}
	t.changed = 1;
	t.waiting = 0;
	notify();
	int32_t st = status;
	to_window(MSG_EXIT, &st, sizeof(st));
	if (t.lsock >= 0 && t.dirfd >= 0)
		unlinkat(t.dirfd, t.leaf, 0);
	return 0;
}
