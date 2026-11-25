#include "history.c"
#include "proto.c"
#include "screen.c"
#include "vt.c"
#include "width.c"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "shm.h"

static uint8_t *map;
static size_t map_size;

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
			uint32_t cp = row[x].cp ? row[x].cp : ' ';
			if (row[x].attr & ATTR_WIDE_TAIL)
				continue;
			fwrite(b, 1, put_utf8(b, cp), stdout);
		}
		printf("\n");
	}
	printf("cursor %d,%d\n", h->x, h->y);
}

int main(int argc, char **argv) {
	if (argc < 5)
		return 2;
	const char *term = argv[1];
	int cols = atoi(argv[2]), rows = atoi(argv[3]);
	char *input = NULL;
	int rc = 0, rr = 0, i = 4;
	for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
			input = argv[++i], unescape(input);
		else if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
			rc = atoi(argv[i + 1]), rr = atoi(argv[i + 2]), i += 2;
	}
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
	static char buf[MSG_MAX];
	int first = 1, status = -1;
	for (;;) {
		uint32_t type;
		int fd;
		ssize_t n = msg_recv(sv[0], &type, buf, sizeof(buf), &fd);
		if (n < 0 || (n == 0 && type == 0))
			break;
		if (type == MSG_SHM && fd >= 0) {
			struct stat st;
			fstat(fd, &st);
			if (map)
				munmap(map, map_size);
			map_size = (size_t)st.st_size;
			map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
			close(fd);
		} else if (type == MSG_DAMAGE) {
			uint32_t seq;
			memcpy(&seq, buf, sizeof(seq));
			msg_send(sv[0], MSG_ACK, &seq, sizeof(seq), -1);
			if (first) {
				first = 0;
				if (input)
					msg_send(sv[0], MSG_INPUT, input, strlen(input), -1);
				if (rc) {
					struct msg_resize r = {(uint16_t)rc, (uint16_t)rr, 10, 20};
					msg_send(sv[0], MSG_RESIZE, &r, sizeof(r), -1);
				}
			}
		} else if (type == MSG_TITLE) {
			printf("title %.*s\n", (int)n, buf);
		} else if (type == MSG_EXIT) {
			int32_t st;
			memcpy(&st, buf, sizeof(st));
			status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
			break;
		}
	}
	if (map)
		print_screen();
	printf("exit %d\n", status);
	waitpid(pid, NULL, 0);
	return 0;
}
