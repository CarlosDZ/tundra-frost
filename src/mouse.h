#ifndef FROST_MOUSE_H
#define FROST_MOUSE_H

#include <stddef.h>

enum mouse_kind { MOUSE_DOWN, MOUSE_UP, MOUSE_MOVE, MOUSE_WHEEL };

enum {
	MB_LEFT = 0,
	MB_MIDDLE = 1,
	MB_RIGHT = 2,
	MB_NONE = 3,
	MB_WHEEL_UP = 64,
	MB_WHEEL_DOWN = 65,
	MB_WHEEL_LEFT = 66,
	MB_WHEEL_RIGHT = 67,
	MB_BACK = 128,
	MB_FORWARD = 129,
};

struct mouse_event {
	enum mouse_kind kind;
	int button;
	unsigned mods;
	int col, row;
	int x, y;
};

struct mouse_modes {
	int mode, sgr, pixels;
};

#define MOUSE_OUT_MAX 32

size_t mouse_encode(const struct mouse_event *e, const struct mouse_modes *m,
                    char *out);

#endif
