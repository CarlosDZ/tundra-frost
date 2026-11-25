#include "history.c"
#include "test.h"
#include "width.c"

static struct hist h;
static struct cell row[64], got[256];

static void make_row(const char *text, int cols) {
	memset(row, 0, sizeof(row));
	for (int x = 0; text[x] && x < cols; x++)
		row[x].cp = (uint8_t)text[x];
}

static int is_text(const struct cell *c, int n, const char *text) {
	int k = (int)strlen(text);
	if (n != k)
		return 0;
	for (int i = 0; i < k; i++)
		if (c[i].cp != (uint8_t)text[i])
			return 0;
	return 1;
}

static void test_basic(void) {
	hist_init(&h, 1 << 20);
	make_row("hola", 10);
	hist_push(&h, row, 10, 0);
	make_row("mundo", 10);
	hist_push(&h, row, 10, LINE_PROMPT);
	uint8_t fl = 0;
	int n = hist_get(&h, 1, got, 256, &fl);
	CHECK(hist_count(&h) == 2 && is_text(got, n, "mundo") && fl == LINE_PROMPT,
	      "lines come back without their trailing empty cells");
	n = hist_get(&h, 0, got, 256, NULL);
	CHECK(is_text(got, n, "hola"), "the oldest is line 0");

	make_row("abcdefghij", 10);
	hist_push(&h, row, 10, LINE_WRAPPED);
	make_row("kl", 10);
	hist_push(&h, row, 10, 0);
	n = hist_get(&h, 2, got, 256, NULL);
	CHECK(hist_count(&h) == 3 && is_text(got, n, "abcdefghijkl"),
	      "a wrapped row and the next are one line");
	CHECK(hist_rows(&h, 2, 10) == 2 && hist_rows(&h, 2, 4) == 3 &&
	          hist_rows(&h, 2, 20) == 1,
	      "and it takes the rows of its width");

	n = hist_pop(&h, got, 256, NULL, NULL);
	CHECK(hist_count(&h) == 2 && is_text(got, n, "abcdefghijkl"),
	      "pop takes the newest");
	hist_clear(&h);
	CHECK(hist_count(&h) == 0, "clear");
	hist_free(&h);
}

static void test_cells(void) {
	hist_init(&h, 1 << 20);
	memset(row, 0, sizeof(row));
	row[0].cp = 'a', row[0].fg = COLOR_RGB(1, 2, 3), row[0].attr = ATTR_BOLD;
	row[1].cp = 0x65E5, row[1].attr = ATTR_WIDE | ATTR_ITALIC;
	row[2].attr = ATTR_WIDE_TAIL | ATTR_ITALIC;
	row[3].cp = 'e', row[3].comb[0] = 0x301;
	row[4].bg = COLOR_INDEX(4);
	row[5].cp = 'z', row[5].link = 7, row[5].ul = COLOR_INDEX(9);
	row[5].attr = UL_CURLY << ATTR_UL_SHIFT;
	hist_push(&h, row, 10, 0);
	int n = hist_get(&h, 0, got, 256, NULL);
	int ok = n == 6;
	for (int i = 0; i < 6 && ok; i++)
		ok = memcmp(&got[i], &row[i], sizeof(struct cell)) == 0;
	CHECK(ok, "styles, two-cell letters, accents, links and coloured empty "
	          "cells come back the same");

	memset(row, 0, sizeof(row));
	for (int x = 0; x < 10; x++)
		row[x].cp = 'x', row[x].fg = COLOR_INDEX(1);
	hist_push(&h, row, 10, LINE_WRAPPED);
	for (int x = 0; x < 10; x++)
		row[x].cp = 'y';
	hist_push(&h, row, 10, 0);
	n = hist_get(&h, 1, got, 256, NULL);
	CHECK(n == 20 && got[19].cp == 'y' && got[19].fg == COLOR_INDEX(1) &&
	          h.lines[1].len == 1 + 17 + 20,
	      "the style goes on across rows and is written once");
	hist_free(&h);
}

static void test_limit(void) {
	size_t limit = 64 << 10;
	hist_init(&h, limit);
	char text[40];
	int over = 0;
	for (int i = 0; i < 20000; i++) {
		snprintf(text, sizeof(text), "line %05d of the history", i);
		make_row(text, 40);
		hist_push(&h, row, 40, 0);
		if (hist_bytes(&h) > limit)
			over = 1;
	}
	size_t n = hist_count(&h);
	int k = hist_get(&h, n - 1, got, 256, NULL);
	int first = hist_get(&h, 0, got + 100, 100, NULL);
	CHECK(!over && n > 300 && n < 20000, "a full history stays in its limit");
	CHECK(is_text(got, k, "line 19999 of the history") && first == 25 &&
	          got[105].cp != '0' + 0,
	      "the newest stays and the oldest went");
	hist_free(&h);

	hist_init(&h, 64 << 10);
	make_row("0123456789", 10);
	for (int i = 0; i < 20000; i++)
		hist_push(&h, row, 10, LINE_WRAPPED);
	struct hist_line *l = &h.lines[(h.first + h.n - 1) & (h.lcap - 1)];
	CHECK(hist_count(&h) >= 1 && l->len <= h.cap && hist_bytes(&h) <= 64 << 10,
	      "an endless line is cut where the history ends");
	hist_free(&h);

	hist_init(&h, 1 << 20);
	for (int i = 0; i < 20000; i++)
		hist_push(&h, row, 10, LINE_WRAPPED);
	l = &h.lines[(h.first + h.n - 1) & (h.lcap - 1)];
	CHECK(hist_count(&h) >= 3 && h.lines[h.first].len <= HIST_LINE_MAX &&
	          l->len <= HIST_LINE_MAX,
	      "and at HIST_LINE_MAX when there is room");
	hist_free(&h);

	hist_init(&h, 64 << 10);
	make_row("", 10);
	for (int i = 0; i < 5000; i++)
		hist_push(&h, row, 10, 0);
	CHECK(hist_count(&h) == 5000 || hist_count(&h) == h.lmax,
	      "empty lines are kept");
	hist_free(&h);
}

static void test_seal(void) {
	hist_init(&h, 1 << 20);
	make_row("abcdefghij", 10);
	hist_push(&h, row, 10, LINE_WRAPPED);
	hist_seal(&h);
	make_row("kl", 10);
	hist_push(&h, row, 10, 0);
	CHECK(hist_count(&h) == 2, "a sealed line takes no more rows");
	hist_free(&h);
}

static void test_split(void) {
	struct cell c[8] = {{0}};
	for (int i = 0; i < 8; i++)
		c[i].cp = 'a';
	c[3].cp = 0x65E5, c[3].attr = ATTR_WIDE;
	c[4].attr = ATTR_WIDE_TAIL;
	int st[8];
	int r = split_rows(c, 8, 4, st, 8);
	CHECK(r == 3 && st[1] == 3 && st[2] == 7,
	      "a two-cell letter that does not fit starts the next row");
	CHECK(split_rows(c, 0, 4, st, 8) == 1, "an empty line takes a row");
}

int main(void) {
	test_basic();
	test_cells();
	test_limit();
	test_seal();
	test_split();
	return test_summary();
}
