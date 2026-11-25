#ifndef FROST_PROTO_H
#define FROST_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum msg_type {
	MSG_INPUT = 1,
	MSG_RESIZE,
	MSG_ACK,
	MSG_DETACH,
	MSG_CLOSE,
	MSG_NAME,
	MSG_ATTACH,
	MSG_QUERY,

	MSG_SHM = 32,
	MSG_DAMAGE,
	MSG_TITLE,
	MSG_BELL,
	MSG_CLIPBOARD,
	MSG_NOTIFY,
	MSG_CWD,
	MSG_EXIT,
	MSG_INFO,
	MSG_BUSY,
};

#define TERM_NAME_MAX 32
#define TERM_TITLE_MAX 96

struct msg_info {
	uint32_t number;
	uint8_t attached;
	char name[TERM_NAME_MAX];
	char title[TERM_TITLE_MAX];
};

struct msg_resize {
	uint16_t cols, rows, cell_w, cell_h;
};

#define MSG_MAX (1 << 17)

int msg_send(int fd, uint32_t type, const void *data, size_t len, int pass);
ssize_t msg_recv(int fd, uint32_t *type, void *data, size_t max, int *got);

#endif
