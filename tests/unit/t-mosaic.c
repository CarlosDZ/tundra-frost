#include "mosaic.c"
#include "test.h"

static struct box b[8];

static int is(int i, int x, int y, int cols, int rows, int title_y) {
	int ok = b[i].x == x && b[i].y == y && b[i].cols == cols &&
	         b[i].rows == rows && b[i].title_y == title_y;
	if (!ok)
		printf("# pane %d: %d,%d %dx%d title %d\n", i, b[i].x, b[i].y,
		       b[i].cols, b[i].rows, b[i].title_y);
	return ok;
}

int main(void) {
	mosaic_layout(80, 24, 1, LAYOUT_MAIN_VERTICAL, 30, 1, b);
	CHECK(is(0, 0, 0, 80, 24, -1), "one pane takes it all, with no title");

	mosaic_layout(100, 24, 3, LAYOUT_MAIN_VERTICAL, 30, 1, b);
	CHECK(is(0, 0, 1, 30, 23, 0), "main-vertical: the leader at 30% on the "
	                              "left, under its title");
	CHECK(is(1, 31, 1, 69, 11, 0) && is(2, 31, 13, 69, 11, 12),
	      "the others stacked on the right, past a separator, each under its "
	      "title");

	mosaic_layout(100, 24, 3, LAYOUT_MAIN_VERTICAL, 30, 0, b);
	CHECK(is(0, 0, 0, 30, 24, -1) && is(1, 31, 0, 69, 12, -1) &&
	          is(2, 31, 13, 69, 11, -1),
	      "without titles, a separator row between the stacked ones");

	mosaic_layout(81, 24, 4, LAYOUT_TILED, 0, 1, b);
	CHECK(is(0, 0, 1, 40, 11, 0) && is(1, 41, 1, 40, 11, 0) &&
	          is(2, 0, 13, 40, 11, 12) && is(3, 41, 13, 40, 11, 12),
	      "tiled: four in two by two");
	mosaic_layout(81, 24, 3, LAYOUT_TILED, 0, 0, b);
	CHECK(is(0, 0, 0, 40, 12, -1) && is(1, 41, 0, 40, 12, -1) &&
	          is(2, 0, 13, 81, 11, -1),
	      "three: the last row takes the whole width");

	mosaic_layout(80, 24, 2, LAYOUT_EVEN_HORIZONTAL, 0, 0, b);
	CHECK(is(0, 0, 0, 40, 24, -1) && is(1, 41, 0, 39, 24, -1),
	      "even-horizontal: side by side");
	mosaic_layout(80, 24, 2, LAYOUT_EVEN_VERTICAL, 0, 0, b);
	CHECK(is(0, 0, 0, 80, 12, -1) && is(1, 0, 13, 80, 11, -1),
	      "even-vertical: one over the other");

	mosaic_layout(5, 3, 4, LAYOUT_MAIN_VERTICAL, 30, 1, b);
	int ok = 1;
	for (int i = 0; i < 4; i++)
		ok &= b[i].cols >= 1 && b[i].rows >= 1;
	CHECK(ok, "too small a window still gives each pane a cell");
	return test_summary();
}
