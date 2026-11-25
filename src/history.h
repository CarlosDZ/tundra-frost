#ifndef FROST_HISTORY_H
#define FROST_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "screen.h"

#define HIST_LINE_MAX (1 << 16)

struct hist_style {
	uint32_t fg, bg, ul;
	uint16_t attr, link;
};

struct hist_line {
	uint32_t off;
	uint32_t len;
	uint32_t cells;
	uint16_t rows;
	uint16_t rows_cols;
	uint8_t flags;
};

struct hist {
	size_t limit;
	uint8_t *buf;
	size_t cap;
	size_t wr;
	struct hist_line *lines;
	size_t lcap, lmax, first, n;
	int open;
	struct hist_style open_style;
	uint8_t *scratch;
	size_t scratch_size;
	struct cell *cells;
	size_t cells_size;
};

int hist_init(struct hist *h, size_t limit);
void hist_free(struct hist *h);
void hist_clear(struct hist *h);
void hist_seal(struct hist *h);
int hist_push(struct hist *h, const struct cell *cells, int cols,
              uint8_t flags);
size_t hist_count(const struct hist *h);
int hist_get(const struct hist *h, size_t i, struct cell *out, int max,
             uint8_t *flags);
int hist_pop(struct hist *h, struct cell *out, int max, uint8_t *flags,
             int *open);
int hist_rows(struct hist *h, size_t i, int cols);
size_t hist_bytes(const struct hist *h);

int split_rows(const struct cell *c, int n, int cols, int *starts, int max);

#endif
