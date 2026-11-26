#ifndef FROST_MOSAIC_H
#define FROST_MOSAIC_H

#include <stddef.h>
#include <stdint.h>

enum layout {
	LAYOUT_MAIN_VERTICAL,
	LAYOUT_TILED,
	LAYOUT_EVEN_HORIZONTAL,
	LAYOUT_EVEN_VERTICAL,
};

struct box {
	int x, y, cols, rows;
	int title_y;
};

void mosaic_layout(int cols, int rows, int n, enum layout layout, int main,
                   int titles, struct box *out);
void mosaic_borders(int cols, int rows, int n, const struct box *b, int focus,
                    uint32_t *cp, uint8_t *lit);

#endif
