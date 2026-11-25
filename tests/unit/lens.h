#ifndef FROST_LENS_H
#define FROST_LENS_H

#include <string.h>

static int lens_hold(const struct scr *s) {
	static const struct cell zero;
	for (int i = 0; i < 2; i++)
		for (int r = 0; r < s->rows; r++)
			for (int x = s->grid[i].len[r]; x < s->cols; x++)
				if (memcmp(&s->grid[i].cells[(size_t)r * s->cols + x], &zero,
				           sizeof(zero)))
					return 0;
	return 1;
}

#endif
