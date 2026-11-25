#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "proto.h"

int msg_send(int fd, uint32_t type, const void *data, size_t len, int pass) {
	if (len > MSG_MAX)
		return -1;
	struct iovec iov[2] = {{&type, sizeof(type)}, {(void *)data, len}};
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} ctl;
	struct msghdr m = {.msg_iov = iov, .msg_iovlen = len ? 2 : 1};
	if (pass >= 0) {
		memset(&ctl, 0, sizeof(ctl));
		m.msg_control = ctl.buf;
		m.msg_controllen = sizeof(ctl.buf);
		struct cmsghdr *c = CMSG_FIRSTHDR(&m);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &pass, sizeof(int));
	}
	ssize_t n;
	do
		n = sendmsg(fd, &m, MSG_NOSIGNAL);
	while (n < 0 && errno == EINTR);
	return n < 0 ? -1 : 0;
}

ssize_t msg_recv(int fd, uint32_t *type, void *data, size_t max, int *got) {
	struct iovec iov[2] = {{type, sizeof(*type)}, {data, max}};
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} ctl;
	struct msghdr m = {.msg_iov = iov,
	                   .msg_iovlen = 2,
	                   .msg_control = ctl.buf,
	                   .msg_controllen = sizeof(ctl.buf)};
	*got = -1;
	*type = 0;
	ssize_t n;
	do
		n = recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
	while (n < 0 && errno == EINTR);
	if (n <= 0)
		return n;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(got, CMSG_DATA(c), sizeof(int));
	if ((size_t)n < sizeof(*type) || (m.msg_flags & MSG_TRUNC)) {
		if (*got >= 0)
			close(*got), *got = -1;
		return -1;
	}
	return n - (ssize_t)sizeof(*type);
}
