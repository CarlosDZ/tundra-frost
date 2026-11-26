#include "history.c"
#include "render.c"
#include "screen.c"
#include "select.c"
#include "test.h"
#include "vt.c"
#include "width.c"

static struct scr s;
static struct view v;
static uint8_t lines[16];

static void start(int cols, int rows, const char *in) {
	scr_free(&s);
	scr_init(&s, cols, rows);
	scr_feed(&s, (const uint8_t *)in, strlen(in));
	struct grid *g = &s.grid[s.alt];
	for (int y = 0; y < rows; y++)
		lines[y] = g->lines[g->map[y]];
	v = (struct view){0};
	v.cols = cols, v.rows = rows, v.cells = g->cells, v.map = g->map;
	v.lines = lines;
}

static int pos(int x, int y) { return y * v.cols + x; }

static int span(int a, int b, int mode, int from, int to) {
	int f, t;
	select_span(&v, a, b, mode, &f, &t);
	return f == from && t == to;
}

static int text(int from, int to, const char *want) {
	size_t n;
	char *t = select_text(&v, from, to, &n);
	int ok = n == strlen(want) && (!n || !memcmp(t, want, n));
	if (!ok)
		printf("# got '%.*s'\n", (int)n, t ? t : "");
	free(t);
	return ok;
}

int main(void) {
	start(10, 3, "ls -l /etc\r\nhola   \r\nfin");
	CHECK(span(pos(3, 0), pos(5, 0), SELECT_CELLS, 3, 6),
	      "cells, from one to the other");
	CHECK(span(pos(5, 0), pos(3, 0), SELECT_CELLS, 3, 6), "either way");
	CHECK(span(pos(7, 0), pos(7, 0), SELECT_WORDS, pos(6, 0), pos(10, 0)),
	      "two clicks: the word under the pointer");
	CHECK(span(pos(2, 0), pos(2, 0), SELECT_WORDS, 2, 3),
	      "on a space, the space alone");
	CHECK(span(pos(4, 1), pos(1, 2), SELECT_LINES, pos(0, 1), pos(0, 3)),
	      "three clicks: whole lines");
	CHECK(text(pos(0, 0), pos(0, 2), "ls -l /etc\nhola"),
	      "the text has a break per line and no spaces at the end");
	CHECK(text(pos(3, 0), pos(5, 0), "-l"), "a part of a line");

	start(5, 3, "abcdefgh\r\nxy");
	CHECK(span(pos(1, 0), pos(1, 0), SELECT_WORDS, 0, pos(3, 1)),
	      "a word goes on through a row that wraps");
	CHECK(span(pos(0, 1), pos(0, 1), SELECT_LINES, 0, pos(0, 2)),
	      "a line is the rows it wrapped into");
	CHECK(text(0, pos(0, 3), "abcdefgh\nxy"),
	      "a row that wraps joins the next with no break");

	start(6, 1, "a日本b");
	CHECK(span(2, 2, SELECT_CELLS, 1, 3),
	      "half of a two-cell letter takes the whole letter");
	CHECK(text(0, 6, "a日本b"), "two-cell letters are copied once");

	start(6, 1, "e\xcc\x81 x");
	CHECK(text(0, 3, "e\xcc\x81 x"), "accents go with their letter");
	scr_free(&s);
	return test_summary();
}
