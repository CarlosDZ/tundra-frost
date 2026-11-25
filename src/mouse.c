#include "mouse.h"

#include <stdio.h>

#include "keys.h"

size_t mouse_encode(const struct mouse_event *e, const struct mouse_modes *m,
                    char *out) {
	if (!m->mode)
		return 0;
	if (e->kind == MOUSE_MOVE &&
	    (m->mode == 1000 || (m->mode == 1002 && e->button == MB_NONE)))
		return 0;
	if (e->kind == MOUSE_UP && e->button >= MB_WHEEL_UP)
		return 0;

	int sgr = m->sgr || m->pixels;
	int b = e->button;
	if (e->kind == MOUSE_UP && !sgr)
		b = MB_NONE;
	if (e->kind == MOUSE_MOVE)
		b += 32;
	if (e->mods & MOD_SHIFT)
		b += 4;
	if (e->mods & MOD_ALT)
		b += 8;
	if (e->mods & MOD_CTRL)
		b += 16;

	int x = m->pixels ? e->x : e->col, y = m->pixels ? e->y : e->row;
	if (sgr) {
		int n = snprintf(out, MOUSE_OUT_MAX, "\x1b[<%d;%d;%d%c", b, x + 1,
		                 y + 1, e->kind == MOUSE_UP ? 'm' : 'M');
		return n > 0 && n < MOUSE_OUT_MAX ? (size_t)n : 0;
	}
	if (b + 32 > 255 || x + 33 > 255 || y + 33 > 255)
		return 0;
	out[0] = '\x1b', out[1] = '[', out[2] = 'M';
	out[3] = (char)(b + 32);
	out[4] = (char)(x + 33);
	out[5] = (char)(y + 33);
	return 6;
}
