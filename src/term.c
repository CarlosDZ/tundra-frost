#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "proto.h"
#include "pty.h"
#include "screen.h"
#include "shm.h"

#define HISTORY_BYTES (32u << 20)
#define ACK_WAIT_MS 50
#define SYNC_MAX_MS 150

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
	struct mapping cur, next;
	int cols, rows;
	int waiting, changed, synced;
	long long since, sync_since;
	char buf[MSG_MAX];
} t;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct shm_head *head(void) { return (struct shm_head *)t.cur.base; }

static void *shm_get(void *ctx, size_t size) {
	(void)ctx, (void)size;
	size_t total = shm_size(t.cols, t.rows);
	int fd = memfd_create("frost", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)total) < 0) {
		perror("memfd error");
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	uint8_t *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap error");
		close(fd);
		return NULL;
	}
	struct shm_head *h = (struct shm_head *)p;
	h->magic = SHM_MAGIC;
	h->version = SHM_VERSION;
	h->block_off = SHM_HEAD_SIZE;
	h->dirty_off = (uint32_t)(SHM_HEAD_SIZE + scr_block_size(t.cols, t.rows));
	h->cols = (uint16_t)t.cols;
	h->rows = (uint16_t)t.rows;
	t.next = (struct mapping){fd, p, total};
	return p + SHM_HEAD_SIZE;
}

static void shm_put(void *ctx, void *block, size_t size) {
	(void)ctx, (void)size;
	struct mapping *m =
	    (uint8_t *)block - SHM_HEAD_SIZE == t.cur.base ? &t.cur : &t.next;
	munmap(m->base, m->size);
	close(m->fd);
	*m = (struct mapping){-1, NULL, 0};
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
	hist_push(&t.hist, c, cols, flags);
}
static void on_seal(void *ctx) { (void)ctx, hist_seal(&t.hist); }
static void on_clear(void *ctx) { (void)ctx, hist_clear(&t.hist); }

static void write_begin(void) {
	__atomic_store_n(&head()->seq, head()->seq + 1, __ATOMIC_RELEASE);
}

static void write_end(void) {
	struct shm_head *h = head();
	struct scr *s = &t.scr;
	h->x = (uint16_t)s->x, h->y = (uint16_t)s->y;
	h->alt = (uint8_t)s->alt, h->pending = (uint8_t)s->pending;
	h->cursor_visible = (uint8_t)s->cursor_visible;
	h->cursor_shape = (uint8_t)s->cursor_shape;
	h->cursor_blink = (uint8_t)s->cursor_blink;
	h->reverse = (uint8_t)s->reverse, h->sync = (uint8_t)s->sync;
	h->focus_events = (uint8_t)s->focus_events;
	memcpy(h->palette, s->palette, sizeof(h->palette));
	memcpy(h->palette_set, s->palette_set, sizeof(h->palette_set));
	uint8_t *dirty = t.cur.base + h->dirty_off;
	for (int y = 0; y < s->rows; y++)
		if (s->dirty[y]) {
			dirty[y] = 1;
			s->dirty[y] = 0;
			t.changed = 1;
		}
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
	uint32_t seq = head()->seq;
	to_window(MSG_DAMAGE, &seq, sizeof(seq));
	t.waiting = t.win >= 0;
	t.since = now_ms();
	t.changed = 0;
}

static void write_all(int fd, const uint8_t *p, size_t n) {
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return;
		p += k, n -= (size_t)k;
	}
}

static void feed(const uint8_t *p, size_t n) {
	write_begin();
	scr_feed(&t.scr, p, n);
	write_end();
	if (t.scr.reply_len) {
		write_all(t.master, t.scr.reply, t.scr.reply_len);
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

static void send_shm(void) {
	if (t.win >= 0 && msg_send(t.win, MSG_SHM, NULL, 0, t.cur.fd) < 0) {
		close(t.win);
		t.win = -1;
	}
}

static void resize(const struct msg_resize *r) {
	if (r->cols < 1 || r->rows < 1)
		return;
	t.cols = r->cols, t.rows = r->rows;
	t.scr.cell_w = r->cell_w, t.scr.cell_h = r->cell_h;
	if (r->cols != t.scr.cols || r->rows != t.scr.rows) {
		write_begin();
		if (scr_resize(&t.scr, r->cols, r->rows, &t.hist) < 0) {
			write_end();
			return;
		}
		t.cur = t.next;
		t.next = (struct mapping){-1, NULL, 0};
		head()->seq = 0;
		write_begin();
		write_end();
		send_shm();
		t.waiting = 0;
		t.changed = 1;
	}
	pty_resize(t.master, r->cols, r->rows, r->cell_w, r->cell_h);
}

static void hang_up(void) {
	kill(-t.pid, SIGHUP);
	if (t.win >= 0)
		close(t.win);
	t.win = -1;
}

static void from_window(void) {
	uint32_t type;
	int fd;
	ssize_t n = msg_recv(t.win, &type, t.buf, sizeof(t.buf), &fd);
	if (fd >= 0)
		close(fd);
	if (n < 0 || type == 0) {
		hang_up();
		return;
	}
	switch (type) {
	case MSG_INPUT:
		write_all(t.master, (uint8_t *)t.buf, (size_t)n);
		break;
	case MSG_RESIZE:
		if ((size_t)n == sizeof(struct msg_resize))
			resize((struct msg_resize *)t.buf);
		break;
	case MSG_ACK: {
		uint8_t *dirty = t.cur.base + head()->dirty_off;
		memset(dirty, 0, (size_t)t.scr.rows);
		t.waiting = 0;
		break;
	}
	case MSG_DETACH:
		close(t.win);
		t.win = -1;
		t.waiting = 0;
		break;
	case MSG_CLOSE:
		hang_up();
		break;
	}
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
	t.cols = number(argv[2], 1, 65535);
	t.rows = number(argv[3], 1, 65535);
	if (t.win < 0 || t.cols < 0 || t.rows < 0)
		return usage();
	char **cmd = argc > 4 ? argv + 4 : NULL;
	fcntl(t.win, F_SETFD, FD_CLOEXEC);

	sigset_t chld;
	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigprocmask(SIG_BLOCK, &chld, NULL);
	t.sig = signalfd(-1, &chld, SFD_CLOEXEC | SFD_NONBLOCK);
	if (t.sig < 0) {
		perror("signalfd error");
		return 1;
	}

	struct scr_mem mem = {shm_get, shm_put, NULL};
	if (hist_init(&t.hist, HISTORY_BYTES) < 0 ||
	    scr_init_mem(&t.scr, t.cols, t.rows, &mem) < 0) {
		fprintf(stderr, "frost-term: cannot make the screen\n");
		return 1;
	}
	t.cur = t.next;
	t.next = (struct mapping){-1, NULL, 0};
	t.scr.hooks = (struct scr_hooks){.bell = on_bell,
	                                 .title = on_title,
	                                 .cwd = on_cwd,
	                                 .clipboard = on_clipboard,
	                                 .notify = on_notify,
	                                 .scrollout = on_scrollout,
	                                 .clear_history = on_clear,
	                                 .seal = on_seal};
	t.master = pty_spawn(cmd, t.cols, t.rows, &t.pid);
	if (t.master < 0)
		return 1;
	write_begin();
	write_end();
	t.changed = 1;
	send_shm();
	notify();

	int pty_open = 1, status = 0;
	for (;;) {
		struct pollfd p[3] = {
		    {t.sig, POLLIN, 0},
		    {t.win, POLLIN, 0},
		    {pty_open && !t.waiting ? t.master : -1, POLLIN, 0}};
		int timeout = -1;
		if (t.waiting) {
			long long left = t.since + ACK_WAIT_MS - now_ms();
			timeout = left > 0 ? (int)left : 0;
		} else if (t.changed && t.scr.sync) {
			long long left = t.sync_since + SYNC_MAX_MS - now_ms();
			timeout = left > 0 ? (int)left : 0;
		}
		if (poll(p, 3, timeout) < 0 && errno != EINTR) {
			perror("poll error");
			return 1;
		}
		if (t.waiting && now_ms() - t.since >= ACK_WAIT_MS)
			t.waiting = 0;
		if (p[2].revents & (POLLIN | POLLHUP | POLLERR))
			if (read_pty() < 0)
				pty_open = 0;
		if (t.win >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
			from_window();
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
	return 0;
}
