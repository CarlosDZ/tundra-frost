#include "history.c"
#include "render.c"
#include "screen.c"
#include "test.h"
#include "vt.c"
#include "width.c"

#include "bdf-write.h"

#include <stdlib.h>
#include <sys/stat.h>

#define W 36
#define H 34
#define BLACK 0xff000000u
#define WHITE 0xffffffffu
#define RED 0xffff0000u
#define BLUE 0xff0000ffu

static const uint8_t A[16] = {0x00, 0x18, 0x24, 0x42, 0x42, 0x7e, 0x42, 0x42,
                              0x42, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SPACE[16] = {0};

static char home[256];
static uint32_t px[W * H];
static struct tdraw_canvas *cv;
static struct render r;
static struct scr s;
static struct view v;

static void write_file(const char *name, const char *text) {
	char p[512];
	snprintf(p, sizeof(p), "%s/.config/tundra/%s", home, name);
	FILE *f = fopen(p, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void setup(void) {
	const char *tmp = getenv("TMPDIR");
	snprintf(home, sizeof(home), "%s/frost-render-XXXXXX", tmp ? tmp : "/tmp");
	if (!mkdtemp(home))
		exit(1);
	char p[512], conf[600];
	snprintf(p, sizeof(p), "%s/.config", home);
	mkdir(p, 0700);
	snprintf(p, sizeof(p), "%s/.config/tundra", home);
	mkdir(p, 0700);
	static const struct bdf_glyph g[] = {{'A', A}, {' ', SPACE}};
	snprintf(p, sizeof(p), "%s/test.bdf", home);
	bdf_write(p, 8, 16, 12, g, 2);
	snprintf(conf, sizeof(conf), "mono = %s\n", p);
	write_file("fonts", conf);
	write_file("theme", "background = 000000\ntext = ffffff\n"
	                    "color1 = ff0000\ncolor4 = 0000ff\n");
	setenv("HOME", home, 1);
	cv = tdraw_canvas_new(px, W, H, W * 4);
}

static void start(const char *seq) {
	scr_free(&s);
	scr_init(&s, 4, 2);
	scr_feed(&s, (const uint8_t *)seq, strlen(seq));
	struct grid *g = &s.grid[s.alt];
	v = (struct view){
	    s.cols,    s.rows,    g->cells,         g->map,         s.x,
	    s.y,       s.pending, s.cursor_visible, s.cursor_shape, s.cursor_blink,
	    s.reverse, s.palette, s.palette_set};
	memset(px, 0, sizeof(px));
}

static uint32_t at(int x, int y) { return px[y * W + x]; }

static uint32_t pix(int cx, int cy, int x, int y) {
	return at(r.ox + cx * r.cw + x, r.oy + cy * r.ch + y);
}

static void test_layout(void) {
	int cols, rows;
	render_fit(&r, W, H, &cols, &rows);
	CHECK(r.cw == 8 && r.ch == 16 && r.ascent == 12,
	      "the cell comes from the mono font");
	CHECK(cols == 4 && rows == 2 && r.ox == 2 && r.oy == 1,
	      "the grid is centered, what is left split around it");
	start("");
	render_margins(&r, cv, &v);
	CHECK(at(0, 0) == BLACK && at(W - 1, H - 1) == BLACK && at(5, 5) == 0,
	      "the margins take the background, the grid is left alone");
}

static void test_colors(void) {
	start("");
	CHECK(render_color(&r, &v, COLOR_DEFAULT, 1) == WHITE &&
	          render_color(&r, &v, COLOR_DEFAULT, 0) == BLACK,
	      "default colours from text and background");
	CHECK(render_color(&r, &v, COLOR_INDEX(1), 1) == RED &&
	          render_color(&r, &v, COLOR_INDEX(2), 1) == 0xff65c55bu,
	      "colours 0 to 15 from the theme, Tundra oscura for what it lacks");
	CHECK(render_color(&r, &v, COLOR_INDEX(16), 1) == BLACK &&
	          render_color(&r, &v, COLOR_INDEX(196), 1) == RED &&
	          render_color(&r, &v, COLOR_INDEX(232), 1) == 0xff080808u &&
	          render_color(&r, &v, COLOR_INDEX(255), 1) == 0xffeeeeeeu,
	      "the 256 colour cube and grays of xterm");
	CHECK(render_color(&r, &v, COLOR_RGB(1, 2, 3), 1) == 0xff010203u,
	      "24-bit colour as it is");
	start("\x1b]4;1;#00ff00\x07\x1b]11;#123456\x07");
	CHECK(render_color(&r, &v, COLOR_INDEX(1), 1) == 0xff00ff00u &&
	          render_color(&r, &v, COLOR_DEFAULT, 0) == 0xff123456u,
	      "a program sets a colour over the theme");
	start("\x1b[?5h");
	CHECK(render_color(&r, &v, COLOR_DEFAULT, 1) == BLACK,
	      "reverse video swaps the default colours");
}

static void test_cells(void) {
	start("\x1b[31;44mA");
	render_row(&r, cv, &v, 0);
	CHECK(pix(0, 0, 1, 5) == RED && pix(0, 0, 0, 0) == BLUE,
	      "a letter in its colour on its background");
	CHECK(pix(1, 0, 0, 0) == BLACK, "the rest of the row, the background");

	start("\x1b[7mA");
	render_row(&r, cv, &v, 0);
	CHECK(pix(0, 0, 1, 5) == BLACK && pix(0, 0, 0, 0) == WHITE, "inverse");

	start("\x1b[8mA");
	render_row(&r, cv, &v, 0);
	CHECK(pix(0, 0, 1, 5) == BLACK, "hidden");

	start("\x1b[4;31m ");
	render_row(&r, cv, &v, 0);
	CHECK(pix(0, 0, 0, 13) == RED && pix(0, 0, 7, 13) == RED &&
	          pix(0, 0, 0, 12) == BLACK,
	      "an underline under the baseline, across the cell");

	start("\x1b[4:2m ");
	render_row(&r, cv, &v, 0);
	CHECK(pix(0, 0, 3, 13) == WHITE && pix(0, 0, 3, 14) == BLACK &&
	          pix(0, 0, 3, 15) == WHITE,
	      "a double underline");
}

static void test_cursor(void) {
	start("A\x1b[1;1H\x1b[2 q");
	render_row(&r, cv, &v, 0);
	render_cursor(&r, cv, &v, 1, 1);
	CHECK(pix(0, 0, 0, 0) == WHITE && pix(0, 0, 1, 5) == BLACK,
	      "a block cursor, with its letter in the background colour");

	start("A\x1b[1;1H\x1b[2 q");
	render_row(&r, cv, &v, 0);
	render_cursor(&r, cv, &v, 0, 1);
	CHECK(pix(0, 0, 0, 0) == WHITE && pix(0, 0, 3, 8) == BLACK &&
	          pix(0, 0, 1, 5) == WHITE,
	      "without focus, its outline");

	start("A");
	render_row(&r, cv, &v, 0);
	render_cursor(&r, cv, &v, 1, 1);
	CHECK(pix(1, 0, 0, 5) == WHITE && pix(1, 0, 1, 5) == BLACK,
	      "by default, a line");

	start("A");
	render_row(&r, cv, &v, 0);
	render_cursor(&r, cv, &v, 1, 0);
	CHECK(pix(1, 0, 0, 5) == BLACK, "and it blinks off");
	render_cursor(&r, cv, &v, 0, 0);
	CHECK(pix(1, 0, 0, 5) == WHITE, "but not without focus");

	start("\x1b[?25l");
	render_cursor(&r, cv, &v, 1, 1);
	CHECK(pix(0, 0, 0, 5) == 0, "a hidden cursor is not painted");
}

int main(void) {
	setup();
	if (render_init(&r, 16) < 0) {
		printf("not ok - no font\n");
		return 1;
	}
	struct tdraw_theme *t = tdraw_theme_open();
	const char *err;
	if (t && tdraw_theme_load(t, &err) >= 0)
		render_theme(&r, t);
	tdraw_theme_free(t);
	test_layout();
	test_colors();
	test_cells();
	test_cursor();
	render_free(&r);
	scr_free(&s);
	tdraw_canvas_free(cv);
	return test_summary();
}
