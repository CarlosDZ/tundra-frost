#ifndef FROST_RENDER_H
#define FROST_RENDER_H

#include <stdint.h>

#include <tundra/draw.h>

#include "screen.h"

#define PAL_SLOTS 259

struct view {
	int cols, rows;
	const struct cell *cells;
	const uint16_t *map;
	int x, y, pending;
	int cursor_visible, cursor_shape, cursor_blink, reverse;
	const uint32_t *palette;
	const uint8_t *palette_set;
	const uint8_t *lines;
	int sel_from, sel_to;
	int link_from, link_to;
};

struct render {
	struct tdraw_font *font, *bold;
	int cw, ch, ascent;
	int ox, oy;
	int width, height;
	int clip_cols;
	uint32_t theme[PAL_SLOTS];
	uint32_t selection;
};

int render_init(struct render *r, int size);
void render_free(struct render *r);
void render_theme(struct render *r, const struct tdraw_theme *t);
void render_fit(struct render *r, int width, int height, int *cols, int *rows);
uint32_t render_color(const struct render *r, const struct view *v,
                      uint32_t color, int fg);
void render_row(struct render *r, struct tdraw_canvas *c, const struct view *v,
                int y);
void render_cursor(struct render *r, struct tdraw_canvas *c,
                   const struct view *v, int focused, int lit);

#endif
