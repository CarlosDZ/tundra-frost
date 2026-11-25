#include "mouse.c"
#include "test.h"

#include <string.h>

#include "keys.h"

#define D MOUSE_DOWN
#define U MOUSE_UP
#define M MOUSE_MOVE
#define W MOUSE_WHEEL

struct row {
	int mode, sgr, pixels;
	enum mouse_kind kind;
	int button;
	unsigned mods;
	int col, row, x, y;
	const char *want;
};

static const struct row rows[] = {
    {0, 0, 0, D, MB_LEFT, 0, 4, 2, 70, 50, ""},
    {1000, 1, 0, D, MB_LEFT, 0, 4, 2, 70, 50, "\x1b[<0;5;3M"},
    {1000, 1, 0, U, MB_LEFT, 0, 4, 2, 70, 50, "\x1b[<0;5;3m"},
    {1000, 1, 0, D, MB_RIGHT, 0, 0, 0, 0, 0, "\x1b[<2;1;1M"},
    {1000, 1, 0, D, MB_MIDDLE, MOD_CTRL, 9, 9, 0, 0, "\x1b[<17;10;10M"},
    {1000, 1, 0, D, MB_LEFT, MOD_ALT | MOD_SHIFT, 0, 0, 0, 0, "\x1b[<12;1;1M"},
    {1000, 1, 0, M, MB_LEFT, 0, 5, 2, 0, 0, ""},
    {1002, 1, 0, M, MB_LEFT, 0, 5, 2, 0, 0, "\x1b[<32;6;3M"},
    {1002, 1, 0, M, MB_NONE, 0, 5, 2, 0, 0, ""},
    {1003, 1, 0, M, MB_NONE, 0, 5, 2, 0, 0, "\x1b[<35;6;3M"},
    {1000, 1, 0, W, MB_WHEEL_UP, 0, 1, 1, 0, 0, "\x1b[<64;2;2M"},
    {1000, 1, 0, W, MB_WHEEL_DOWN, 0, 1, 1, 0, 0, "\x1b[<65;2;2M"},
    {1000, 1, 0, U, MB_WHEEL_DOWN, 0, 1, 1, 0, 0, ""},
    {1000, 1, 0, D, MB_BACK, 0, 0, 0, 0, 0, "\x1b[<128;1;1M"},
    {1000, 1, 1, D, MB_LEFT, 0, 4, 2, 70, 50, "\x1b[<0;71;51M"},
    {1000, 0, 1, D, MB_LEFT, 0, 4, 2, 70, 50, "\x1b[<0;71;51M"},
    {1000, 0, 0, D, MB_LEFT, 0, 4, 2, 0, 0, "\x1b[M %#"},
    {1000, 0, 0, U, MB_LEFT, 0, 4, 2, 0, 0, "\x1b[M#%#"},
    {1000, 0, 0, W, MB_WHEEL_UP, 0, 0, 0, 0, 0, "\x1b[M`!!"},
    {1002, 0, 0, M, MB_LEFT, 0, 0, 0, 0, 0, "\x1b[M@!!"},
    {1000, 0, 0, D, MB_LEFT, 0, 222, 0, 0, 0, "\x1b[M \xff!"},
    {1000, 0, 0, D, MB_LEFT, 0, 223, 0, 0, 0, ""},
    {1000, 1, 0, D, MB_LEFT, 0, 400, 300, 0, 0, "\x1b[<0;401;301M"},
};

int main(void) {
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		const struct row *r = &rows[i];
		struct mouse_event e = {r->kind, r->button, r->mods, r->col,
		                        r->row,  r->x,      r->y};
		struct mouse_modes m = {r->mode, r->sgr, r->pixels};
		char out[MOUSE_OUT_MAX];
		size_t n = mouse_encode(&e, &m, out);
		char desc[128];
		snprintf(desc, sizeof(desc),
		         "mode %d sgr %d pixels %d, event %d button %d: the table's "
		         "report",
		         r->mode, r->sgr, r->pixels, r->kind, r->button);
		CHECK(n == strlen(r->want) && !memcmp(out, r->want, n), desc);
	}
	return test_summary();
}
