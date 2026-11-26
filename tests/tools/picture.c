#include "history.c"
#include "line.c"
#include "render.c"
#include "screen.c"
#include "vt.c"
#include "width.c"

#include "bdf-write.h"

#include <stdlib.h>

#define SIZE 24

static const uint8_t G_A[12] = {0x00, 0x18, 0x24, 0x42, 0x42, 0x7e,
                                0x42, 0x42, 0x42, 0x00, 0x00, 0x00};
static const uint8_t G_B[12] = {0x00, 0x7c, 0x42, 0x42, 0x7c, 0x42,
                                0x42, 0x42, 0x7c, 0x00, 0x00, 0x00};
static const uint8_t G_X[12] = {0x00, 0x00, 0x00, 0x42, 0x24, 0x18,
                                0x18, 0x24, 0x42, 0x00, 0x00, 0x00};
static const uint8_t G_DOT[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x18, 0x18, 0x00, 0x00};
static const uint8_t G_SPACE[12] = {0};

static int font(const char *path) {
	static const struct bdf_glyph g[] = {
	    {'A', G_A}, {'B', G_B}, {'x', G_X}, {'.', G_DOT}, {' ', G_SPACE}};
	return bdf_write(path, 8, 12, 9, g, 5) < 0;
}

static void *slurp(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	size_t cap = 1 << 16, n = 0;
	char *buf = malloc(cap);
	size_t k;
	while (buf && (k = fread(buf + n, 1, cap - n, f)) > 0) {
		n += k;
		if (n == cap) {
			char *more = realloc(buf, cap *= 2);
			if (!more)
				free(buf);
			buf = more;
		}
	}
	fclose(f);
	*len = n;
	return buf;
}

static uint32_t *load(const char *path, int *w, int *h) {
	size_t len;
	void *data = slurp(path, &len);
	const char *err;
	struct tdraw_image *im = data ? tdraw_image_decode(data, len, &err) : NULL;
	free(data);
	if (!im) {
		fprintf(stderr, "picture: cannot read %s\n", path);
		return NULL;
	}
	tdraw_image_size(im, w, h);
	uint32_t *px = calloc((size_t)*w * (size_t)*h, 4);
	struct tdraw_canvas *c = px ? tdraw_canvas_new(px, *w, *h, *w * 4) : NULL;
	if (c)
		tdraw_image(c, im, 0, 0, 1);
	tdraw_canvas_free(c);
	tdraw_image_free(im);
	return px;
}

static int same(const char *a, const char *b) {
	int aw, ah, bw, bh;
	uint32_t *pa = load(a, &aw, &ah), *pb = load(b, &bw, &bh);
	int rc = 1;
	if (pa && pb && aw == bw && ah == bh) {
		rc = 0;
		for (int i = 0; i < aw * ah && !rc; i++)
			if (pa[i] != pb[i]) {
				printf("differ at %d,%d: %08x %08x\n", i % aw, i / aw, pa[i],
				       pb[i]);
				rc = 1;
			}
	} else if (pa && pb) {
		printf("sizes %dx%d and %dx%d\n", aw, ah, bw, bh);
	}
	free(pa);
	free(pb);
	return rc;
}

static int pixel(const char *path, int x, int y) {
	int w, h;
	uint32_t *px = load(path, &w, &h);
	if (!px || x < 0 || y < 0 || x >= w || y >= h) {
		free(px);
		return 1;
	}
	printf("%08x\n", px[y * w + x]);
	free(px);
	return 0;
}

static int paint(const char *in, int cols, int rows, const char *out) {
	struct render r;
	if (render_init(&r, SIZE) < 0)
		return 1;
	struct tdraw_theme *t = tdraw_theme_open();
	const char *err;
	if (t && tdraw_theme_load(t, &err) >= 0)
		render_theme(&r, t);
	tdraw_theme_free(t);

	size_t len;
	uint8_t *data = slurp(in, &len);
	struct scr s;
	if (!data || scr_init(&s, cols, rows) < 0)
		return 1;
	scr_feed(&s, data, len);
	free(data);
	struct grid *g = &s.grid[s.alt];
	struct view v = {s.cols,
	                 s.rows,
	                 g->cells,
	                 g->map,
	                 s.x,
	                 s.y,
	                 s.pending,
	                 s.cursor_visible,
	                 s.cursor_shape,
	                 s.cursor_blink,
	                 s.reverse,
	                 s.palette,
	                 s.palette_set,
	                 NULL,
	                 0,
	                 0,
	                 0,
	                 0};

	int w = cols * r.cw, h = rows * r.ch, c_, r_;
	uint32_t *px = calloc((size_t)w * (size_t)h, 4);
	struct tdraw_canvas *c = px ? tdraw_canvas_new(px, w, h, w * 4) : NULL;
	if (!c)
		return 1;
	render_fit(&r, w, h, &c_, &r_);
	tdraw_fill(c, 0, 0, w, h, render_color(&r, &v, COLOR_DEFAULT, 0));
	for (int y = 0; y < rows; y++)
		render_row(&r, c, &v, y);
	render_cursor(&r, c, &v, 1, 1);
	void *png;
	int rc = tdraw_capture(c, 0, 0, w, h, &png, &len) < 0;
	FILE *f = rc ? NULL : fopen(out, "wb");
	if (f) {
		fwrite(png, 1, len, f);
		fclose(f);
		free(png);
	}
	tdraw_canvas_free(c);
	free(px);
	scr_free(&s);
	render_free(&r);
	return rc || !f;
}

static int line_sample(const char *out) {
	enum { COLS = 64, ROWS = 6 };
	struct render r;
	if (render_init(&r, SIZE) < 0)
		return 1;
	struct tdraw_theme *t = tdraw_theme_open();
	const char *err;
	if (t && tdraw_theme_load(t, &err) >= 0)
		render_theme(&r, t);
	uint32_t c[4] = {0x1f2529, 0xccd2d6, 0x889196, 0xff1712};
	const char *names[4] = {"frost.line", "text", "dim", "alert"};
	for (int i = 0; i < 4; i++) {
		uint32_t v = t ? tdraw_theme_color(t, names[i], c[i]) : c[i];
		c[i] = COLOR_RGB(v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF);
	}
	tdraw_theme_free(t);
	struct line_colors lc = {c[0], c[1], c[2], c[3]};

	struct scr s;
	scr_init(&s, COLS, ROWS);
	const char *screen = "~/src/tundra-frost $ make check\r\n"
	                     "# 457 passed, 0 failed\r\n~/src/tundra-frost $ ";
	scr_feed(&s, (const uint8_t *)screen, strlen(screen));
	struct line_tab tabs[3] = {
	    {"shell", 0, 0}, {"nvim src/win.c", 1, 0}, {"make", 0, 1}};
	struct {
		int open;
		const char *input, *msg;
		int error;
	} lines[4] = {{0, "", "", 0},
	              {1, "goto 2", "", 0},
	              {1, "", "1  2 build detached  4 notes detached", 0},
	              {1, "", "no tab 9", 1}};

	int w = COLS * r.cw, h = (ROWS + 4) * r.ch, c_, r_;
	uint32_t *px = calloc((size_t)w * (size_t)h, 4);
	struct tdraw_canvas *cv = px ? tdraw_canvas_new(px, w, h, w * 4) : NULL;
	if (!cv)
		return 1;
	render_fit(&r, w, ROWS * r.ch, &c_, &r_);
	struct grid *g = &s.grid[0];
	struct view v = {
	    s.cols, s.rows, g->cells,  g->map,        s.x,  s.y, 0, 1, CURSOR_BAR,
	    0,      0,      s.palette, s.palette_set, NULL, 0,   0, 0, 0};
	tdraw_fill(cv, 0, 0, w, ROWS * r.ch,
	           render_color(&r, &v, COLOR_DEFAULT, 0));
	for (int y = 0; y < ROWS; y++)
		render_row(&r, cv, &v, y);
	render_cursor(&r, cv, &v, 0, 1);
	for (int i = 0; i < 4; i++) {
		struct cell row[COLS];
		int cx;
		line_cells(row, COLS, tabs, 3, lines[i].open, lines[i].input,
		           lines[i].msg, lines[i].error, &lc, &cx);
		uint16_t map = 0;
		struct view lv = {COLS, 1,    row,     &map,       cx < 0 ? 0 : cx,
		                  0,    0,    cx >= 0, CURSOR_BAR, 0,
		                  0,    NULL, NULL,    NULL,       0,
		                  0,    0,    0};
		r.oy = (ROWS + i) * r.ch;
		render_row(&r, cv, &lv, 0);
		render_cursor(&r, cv, &lv, 1, 1);
	}
	void *png;
	size_t len;
	int rc = tdraw_capture(cv, 0, 0, w, h, &png, &len) < 0;
	FILE *f = rc ? NULL : fopen(out, "wb");
	if (f) {
		fwrite(png, 1, len, f);
		fclose(f);
		free(png);
	}
	tdraw_canvas_free(cv);
	free(px);
	scr_free(&s);
	render_free(&r);
	return rc || !f;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "font") == 0)
		return font(argv[2]);
	if (argc == 6 && strcmp(argv[1], "paint") == 0)
		return paint(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
	if (argc == 4 && strcmp(argv[1], "same") == 0)
		return same(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "line") == 0)
		return line_sample(argv[2]);
	if (argc == 5 && strcmp(argv[1], "pixel") == 0)
		return pixel(argv[2], atoi(argv[3]), atoi(argv[4]));
	fprintf(stderr, "usage: picture font|paint|same|pixel|line ...\n");
	return 2;
}
