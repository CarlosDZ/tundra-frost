#ifndef FROST_SCREEN_H
#define FROST_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#include "vt.h"

#define COLOR_DEFAULT 0u
#define COLOR_INDEX(i) (0x01000000u | (uint32_t)(i))
#define COLOR_RGB(r, g, b)                                                     \
	(0x02000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define COLOR_KIND(c) ((c) >> 24)

enum {
	ATTR_BOLD = 1 << 0,
	ATTR_DIM = 1 << 1,
	ATTR_ITALIC = 1 << 2,
	ATTR_BLINK = 1 << 3,
	ATTR_INVERSE = 1 << 4,
	ATTR_HIDDEN = 1 << 5,
	ATTR_STRIKE = 1 << 6,
	ATTR_OVERLINE = 1 << 7,
	ATTR_UL_SHIFT = 8,
	ATTR_UL_MASK = 7 << 8,
	ATTR_WIDE = 1 << 11,
	ATTR_WIDE_TAIL = 1 << 12,
};

enum ul_style { UL_NONE, UL_SINGLE, UL_DOUBLE, UL_CURLY, UL_DOTTED, UL_DASHED };

#define CELL_COMB 2

struct cell {
	uint32_t cp;
	uint32_t comb[CELL_COMB];
	uint32_t fg, bg, ul;
	uint16_t attr;
	uint16_t link;
};

enum {
	LINE_WRAPPED = 1 << 0,
	LINE_PROMPT = 1 << 1,
};

enum {
	MOUSE_OFF,
	MOUSE_PRESS = 1000,
	MOUSE_DRAG = 1002,
	MOUSE_MOTION = 1003,
};

enum cursor_shape {
	CURSOR_DEFAULT,
	CURSOR_BLOCK,
	CURSOR_UNDERLINE,
	CURSOR_BAR
};

struct pen {
	uint32_t fg, bg, ul;
	uint16_t attr;
	uint16_t link;
};

struct saved {
	int x, y, pending, origin;
	struct pen pen;
	char g[4];
	int gl;
};

#define KEY_STACK 8
#define REPLY_MAX 4096
#define TITLE_MAX 256
#define LINKS_MAX 256

struct grid {
	struct cell *cells;
	uint8_t *lines;
	uint16_t *map;
	struct saved saved;
	int keys[KEY_STACK];
	int nkeys;
};

struct scr_hooks {
	void (*bell)(void *ctx);
	void (*title)(void *ctx, const char *title);
	void (*cwd)(void *ctx, const char *uri, size_t len);
	void (*clipboard)(void *ctx, const char *b64, size_t len);
	void (*notify)(void *ctx, const char *text, size_t len);
	void (*scrollout)(void *ctx, const struct cell *cells, int cols,
	                  uint8_t flags);
	void (*clear_history)(void *ctx);
};

struct scr {
	int cols, rows;
	struct grid grid[2];
	int alt;
	int x, y;
	int pending;
	int top, bot;
	struct pen pen;
	char g[4];
	int gl;
	uint32_t last;
	uint8_t *tabs;
	uint8_t *dirty;

	int autowrap, origin, insert, newline;
	int app_cursor, app_keypad, cursor_visible, reverse;
	int bracketed_paste, focus_events, alt_scroll, sync;
	int mouse, mouse_sgr, mouse_pixels;
	enum cursor_shape cursor_shape;
	int cursor_blink;

	uint32_t palette[259];
	uint8_t palette_set[259];
	uint8_t light[259];
	int cell_w, cell_h;

	char title[TITLE_MAX];
	int nlinks;
	char *links[LINKS_MAX];

	uint8_t reply[REPLY_MAX];
	size_t reply_len;

	struct scr_hooks hooks;
	void *ctx;
	struct vt vt;
};

int scr_init(struct scr *s, int cols, int rows);
void scr_free(struct scr *s);
void scr_feed(struct scr *s, const uint8_t *buf, size_t len);
struct cell *scr_cell(struct scr *s, int x, int y);
uint8_t scr_line(struct scr *s, int y);

#endif
