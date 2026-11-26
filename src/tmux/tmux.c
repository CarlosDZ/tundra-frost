#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "proto.h"

#define WAIT_S 10

static char req[MSG_MAX], rep[MSG_MAX];

static int say(const char *what) {
	fprintf(stderr, "tmux: %s\n", what);
	return 1;
}

static int dial(const char *path, size_t len) {
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	if (len >= sizeof(a.sun_path))
		return -1;
	memcpy(a.sun_path, path, len);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int put(size_t *n, const char *s) {
	size_t k = strlen(s) + 1;
	if (*n + k > sizeof(req))
		return -1;
	memcpy(req + *n, s, k);
	*n += k;
	return 0;
}

int main(int argc, char **argv) {
	const char *sock = NULL;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *o = argv[i];
		if (!strcmp(o, "--")) {
			i++;
			break;
		}
		if (!strcmp(o, "-V")) {
			puts("tmux " TMUX_VERSION);
			return 0;
		}
		if (o[1] == 'S' || o[1] == 'f') {
			const char *v = o[2] ? o + 2 : i + 1 < argc ? argv[++i] : NULL;
			if (!v)
				return say("an option without its value");
			if (o[1] == 'S')
				sock = v;
			continue;
		}
		if (strcmp(o, "-2") && strcmp(o, "-u"))
			return say("frost takes only -S, -f, -2, -u and -V");
	}
	const char *env = getenv("TMUX");
	size_t len = sock ? strlen(sock) : env ? strcspn(env, ",") : 0;
	if (!len)
		return say("not inside frost");
	if (i == argc)
		return say("frost keeps no sessions: an order is needed");
	char cwd[PATH_MAX];
	const char *pane = getenv("TMUX_PANE");
	size_t n = 0;
	if (put(&n, pane ? pane : "") < 0 ||
	    put(&n, getcwd(cwd, sizeof(cwd)) ? cwd : "/") < 0)
		return say("the order is too long");
	for (; i < argc; i++)
		if (put(&n, argv[i]) < 0)
			return say("the order is too long");
	int fd = dial(sock ? sock : env, len);
	if (fd < 0)
		return say("cannot reach the frost window");
	struct timeval tv = {WAIT_S, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	uint32_t type = 0;
	int got = -1;
	ssize_t k = msg_send(fd, MSG_TMUX, req, n, -1) < 0
	                ? -1
	                : msg_recv(fd, &type, rep, sizeof(rep), &got);
	close(fd);
	if (k >= 0 && got >= 0)
		close(got);
	struct msg_tmux_done d;
	if (k < (ssize_t)sizeof(d) || type != MSG_TMUX_DONE)
		return say("the frost window did not answer");
	memcpy(&d, rep, sizeof(d));
	size_t body = (size_t)k - sizeof(d);
	size_t out = d.out_len < body ? d.out_len : body;
	fwrite(rep + sizeof(d), 1, out, stdout);
	fwrite(rep + sizeof(d) + out, 1, body - out, stderr);
	return d.code;
}
