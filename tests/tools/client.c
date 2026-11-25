#include "history.c"
#include "proto.c"
#include "screen.c"
#include "vt.c"
#include "width.c"

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "shm.h"

static uint8_t *map;
static size_t map_size;
static unsigned number;

struct opts {
	char *input, *name;
	int rc, rr, wait_ms, late_ms, detach, close_it;
};

static void unescape(char *s) {
	char *o = s;
	for (; *s; s++) {
		if (*s == '\\' && s[1] == 'n')
			*o++ = '\n', s++;
		else if (*s == '\\' && s[1] == 'e')
			*o++ = 0x1b, s++;
		else
			*o++ = *s;
	}
	*o = 0;
}

static void print_screen(void) {
	const struct shm_head *h = (const void *)map;
	struct cell *cells[2];
	uint16_t *maps[2];
	uint8_t *lines[2];
	scr_block_layout(map + h->block_off, h->cols, h->rows, cells, maps, lines);
	int g = h->alt;
	for (int y = 0; y < h->rows; y++) {
		const struct cell *row = cells[g] + (size_t)maps[g][y] * h->cols;
		int end = h->cols;
		while (end > 0 && !row[end - 1].cp)
			end--;
		printf("|");
		for (int x = 0; x < end; x++) {
			uint8_t b[4];
			if (row[x].attr & ATTR_WIDE_TAIL)
				continue;
			fwrite(b, 1, put_utf8(b, row[x].cp ? row[x].cp : ' '), stdout);
		}
		printf("\n");
	}
	printf("cursor %d,%d\n", h->x, h->y);
}

static void print_dirty(void) {
	const struct shm_head *h = (const void *)map;
	printf("dirty");
	for (int y = 0; y < h->rows; y++)
		if (map[h->dirty_off + y])
			printf(" %d", y);
	printf("\n");
}

static int parse(int argc, char **argv, int i, struct opts *o) {
	for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
			o->input = argv[++i], unescape(o->input);
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			o->name = argv[++i];
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
			o->wait_ms = atoi(argv[++i]);
		else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
			o->late_ms = atoi(argv[++i]);
		else if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
			o->rc = atoi(argv[i + 1]), o->rr = atoi(argv[i + 2]), i += 2;
		else if (strcmp(argv[i], "-d") == 0)
			o->detach = 1;
		else if (strcmp(argv[i], "-c") == 0)
			o->close_it = 1;
	}
	return i;
}

static void session(int fd, const struct opts *o, int attached) {
	static char buf[MSG_MAX];
	int first = 1, status = -1;
	for (;;) {
		uint32_t type;
		int got;
		ssize_t n = msg_recv(fd, &type, buf, sizeof(buf), &got);
		if (n < 0 || type == 0)
			break;
		if (type == MSG_SHM && got >= 0) {
			struct stat st;
			fstat(got, &st);
			if (map)
				munmap(map, map_size);
			map_size = (size_t)st.st_size;
			map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, got, 0);
			close(got);
		} else if (type == MSG_INFO && (size_t)n == sizeof(struct msg_info)) {
			number = ((struct msg_info *)buf)->number;
		} else if (type == MSG_BUSY) {
			printf("busy\n");
			return;
		} else if (type == MSG_TITLE) {
			printf("title %.*s\n", (int)n, buf);
		} else if (type == MSG_DAMAGE) {
			uint32_t seq;
			memcpy(&seq, buf, sizeof(seq));
			if (first && o->late_ms) {
				poll(NULL, 0, o->late_ms);
				msg_send(fd, MSG_ACK, &seq, sizeof(seq), -1);
				poll(NULL, 0, 50);
				print_dirty();
			} else {
				msg_send(fd, MSG_ACK, &seq, sizeof(seq), -1);
			}
			if (!first)
				continue;
			first = 0;
			if (o->input)
				msg_send(fd, MSG_INPUT, o->input, strlen(o->input), -1);
			if (o->rc) {
				struct msg_resize r = {(uint16_t)o->rc, (uint16_t)o->rr, 10,
				                       20};
				msg_send(fd, MSG_RESIZE, &r, sizeof(r), -1);
			}
			if (o->name)
				msg_send(fd, MSG_NAME, o->name, strlen(o->name), -1);
			if (o->wait_ms)
				poll(NULL, 0, o->wait_ms);
			if (o->detach) {
				msg_send(fd, MSG_DETACH, NULL, 0, -1);
				printf("number %u\n", number);
				return;
			}
			if (o->close_it) {
				msg_send(fd, MSG_CLOSE, NULL, 0, -1);
			} else if (attached) {
				poll(NULL, 0, 100);
				print_screen();
				msg_send(fd, MSG_DETACH, NULL, 0, -1);
				return;
			}
		} else if (type == MSG_EXIT) {
			int32_t st;
			memcpy(&st, buf, sizeof(st));
			status = WIFEXITED(st)     ? WEXITSTATUS(st)
			         : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
			                           : 255;
			break;
		}
	}
	if (map)
		print_screen();
	printf("exit %d\n", status);
}

static int dial(const char *path) {
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int list(const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return 1;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		int fd = dial(path);
		if (fd < 0)
			continue;
		msg_send(fd, MSG_QUERY, NULL, 0, -1);
		struct msg_info in;
		uint32_t type;
		int got;
		if (msg_recv(fd, &type, &in, sizeof(in), &got) == sizeof(in) &&
		    type == MSG_INFO)
			printf("%u %s %s %s\n", in.number, in.name[0] ? in.name : "-",
			       in.attached ? "attached" : "detached",
			       in.title[0] ? in.title : "-");
		close(fd);
	}
	closedir(d);
	return 0;
}

int main(int argc, char **argv) {
	struct opts o = {0};
	if (argc == 3 && strcmp(argv[1], "ls") == 0)
		return list(argv[2]);
	if (argc >= 5 && strcmp(argv[1], "attach") == 0) {
		parse(argc, argv, 5, &o);
		int fd = dial(argv[2]);
		if (fd < 0) {
			printf("no terminal\n");
			return 1;
		}
		struct msg_resize r = {(uint16_t)atoi(argv[3]), (uint16_t)atoi(argv[4]),
		                       10, 20};
		msg_send(fd, MSG_ATTACH, &r, sizeof(r), -1);
		session(fd, &o, 1);
		return 0;
	}
	if (argc < 5)
		return 2;
	const char *term = argv[1];
	int cols = atoi(argv[2]), rows = atoi(argv[3]);
	int i = parse(argc, argv, 4, &o);
	if (i >= argc)
		return 2;
	char **prog = argv + i + 1;

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0)
		return 1;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(sv[1], 3);
		int n = 0;
		while (prog[n])
			n++;
		char *args[n + 5];
		char c[16], r[16];
		snprintf(c, sizeof(c), "%d", cols);
		snprintf(r, sizeof(r), "%d", rows);
		args[0] = (char *)term, args[1] = "3", args[2] = c, args[3] = r;
		for (int k = 0; k <= n; k++)
			args[4 + k] = prog[k];
		execv(term, args);
		_exit(127);
	}
	close(sv[1]);
	session(sv[0], &o, 0);
	if (!o.detach)
		waitpid(pid, NULL, 0);
	return 0;
}
