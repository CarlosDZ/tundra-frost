#ifndef FROST_SHM_H
#define FROST_SHM_H

#include <stddef.h>
#include <stdint.h>

#include "screen.h"

#define SHM_MAGIC 0x74736F66u
#define SHM_VERSION 2

struct shm_head {
	uint32_t magic, version;
	uint32_t seq;
	uint16_t cols, rows;
	uint16_t x, y;
	uint8_t alt, pending, cursor_visible, cursor_shape, cursor_blink;
	uint8_t reverse, sync, focus_events;
	uint32_t palette[259];
	uint8_t palette_set[259];
};

#define SHM_HEAD_SIZE ((sizeof(struct shm_head) + 63) & ~(size_t)63)

struct shm_layout {
	size_t cells, map, lines, dirty, size;
};

static inline struct shm_layout shm_layout(int cols, int rows) {
	struct shm_layout l;
	l.cells = SHM_HEAD_SIZE;
	l.map = l.cells + (size_t)cols * (size_t)rows * sizeof(struct cell);
	l.lines = l.map + (size_t)rows * sizeof(uint16_t);
	l.dirty = l.lines + (size_t)rows;
	l.size = l.dirty + (size_t)rows;
	return l;
}

#endif
