#ifndef FROST_SHM_H
#define FROST_SHM_H

#include <stdint.h>

#include "screen.h"

#define SHM_MAGIC 0x74736F66u
#define SHM_VERSION 1

struct shm_head {
	uint32_t magic, version;
	uint32_t seq;
	uint32_t block_off, dirty_off;
	uint16_t cols, rows;
	uint16_t x, y;
	uint8_t alt, pending, cursor_visible, cursor_shape, cursor_blink;
	uint8_t reverse, sync, focus_events;
	uint32_t palette[259];
	uint8_t palette_set[259];
};

#define SHM_HEAD_SIZE ((sizeof(struct shm_head) + 63) & ~(size_t)63)

static inline size_t shm_size(int cols, int rows) {
	return SHM_HEAD_SIZE + scr_block_size(cols, rows) + (size_t)rows;
}

#endif
