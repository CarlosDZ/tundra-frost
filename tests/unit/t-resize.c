#include "history.c"
#include "screen.c"
#include "test.h"
#include "vt.c"
#include "width.c"

#include "lens.h"

static struct scr s;
static struct hist h;

static void on_scrollout(void *ctx, const struct cell *cells, int cols,
                         uint8_t flags) {
	(void)ctx;
	hist_push(&h, cells, cols, flags);
}
static void on_seal(void *ctx) { (void)ctx, hist_seal(&h); }
static void on_clear(void *ctx) { (void)ctx, hist_clear(&h); }

static void start(int cols, int rows) {
	scr_free(&s);
	hist_free(&h);
	scr_init(&s, cols, rows);
	hist_init(&h, 1 << 20);
	s.hooks.scrollout = on_scrollout;
	s.hooks.seal = on_seal;
	s.hooks.clear_history = on_clear;
}

static void feed(const char *str) {
	scr_feed(&s, (const uint8_t *)str, strlen(str));
}

static void put_cp(char **p, uint32_t cp) { *p += put_utf8((uint8_t *)*p, cp); }

static const char *snapshot(void) {
	static char buf[1 << 16];
	static struct cell line[4096];
	char *p = buf;
	for (size_t i = 0; i < hist_count(&h); i++) {
		uint8_t fl;
		int n = hist_get(&h, i, line, 4096, &fl);
		*p++ = 'H';
		for (int k = 0; k < n && k < 4096; k++)
			if (!(line[k].attr & ATTR_WIDE_TAIL))
				put_cp(&p, line[k].cp ? line[k].cp : '.');
		*p++ = '\n';
	}
	for (int y = 0; y < s.rows; y++) {
		*p++ = (scr_line(&s, y) & LINE_WRAPPED) ? 'W' : '|';
		for (int x = 0; x < s.cols; x++) {
			struct cell *c = scr_cell(&s, x, y);
			if (!(c->attr & ATTR_WIDE_TAIL))
				put_cp(&p, c->cp ? c->cp : '.');
		}
		*p++ = '\n';
	}
	p += sprintf(p, "cursor %d,%d%s", s.x, s.y, s.pending ? " pending" : "");
	return buf;
}

static void test_round_trip(void) {
	start(20, 5);
	feed("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\nbb\r\n"
	     "ccccccccccccccccccccccccccccccccccccccccccccc");
	char before[1 << 16];
	snprintf(before, sizeof(before), "%s", snapshot());
	CHECK(hist_count(&h) == 1 && h.open, "the top row left, its line open");

	scr_resize(&s, 7, 5, &h);
	CHECK(s.cols == 7 && !(scr_line(&s, 4) & LINE_WRAPPED) && s.y == 4 &&
	          s.x == 3,
	      "narrower: the last line fills the screen, the cursor after it");
	CHECK(hist_count(&h) == 3 && h.open, "and the rest went to the history");

	scr_resize(&s, 20, 5, &h);
	CHECK(strcmp(snapshot(), before) == 0,
	      "wider again: history, rows and cursor as they were");

	scr_resize(&s, 3, 2, &h);
	scr_resize(&s, 50, 9, &h);
	scr_resize(&s, 20, 5, &h);
	CHECK(strcmp(snapshot(), before) == 0, "and after any sizes in between");
}

static void test_cursor(void) {
	start(20, 4);
	feed("hello world\x1b[7G");
	scr_resize(&s, 5, 4, &h);
	CHECK(s.x == 1 && s.y == 1, "the cursor keeps its place in the line");
	scr_resize(&s, 20, 4, &h);
	CHECK(s.x == 6 && s.y == 0, "and comes back");

	start(10, 4);
	feed("0123456789");
	CHECK(s.pending, "a full row leaves the wrap pending");
	scr_resize(&s, 5, 4, &h);
	CHECK(s.y == 1 && s.x == 4 && s.pending,
	      "after the end of a full row it stays pending");
	feed("x");
	CHECK(scr_cell(&s, 0, 2)->cp == 'x', "and the next letter wraps");

	start(10, 4);
	feed("ab\r\n\r\n\x1b[5C");
	scr_resize(&s, 4, 4, &h);
	CHECK(s.y == 3 && s.x == 1,
	      "a cursor past the text of its row keeps its place in the line");
}

static void test_wide(void) {
	start(20, 4);
	feed("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8"
	     "\xaa\x9e");
	char before[1 << 16];
	snprintf(before, sizeof(before), "%s", snapshot());
	scr_resize(&s, 5, 4, &h);
	CHECK(scr_cell(&s, 0, 0)->cp == 0x65E5 && scr_cell(&s, 4, 0)->cp == 0 &&
	          scr_cell(&s, 0, 1)->cp == 0x8A9E,
	      "two-cell letters do not split across rows");
	scr_resize(&s, 20, 4, &h);
	CHECK(strcmp(snapshot(), before) == 0, "and join again without gaps");
}

static void test_height(void) {
	start(10, 5);
	feed("1\r\n2\r\n3\r\n4\r\n5");
	scr_resize(&s, 10, 3, &h);
	CHECK(scr_cell(&s, 0, 0)->cp == '3' && hist_count(&h) == 2 && s.y == 2,
	      "lower: the top rows go to the history");
	scr_resize(&s, 10, 5, &h);
	CHECK(scr_cell(&s, 0, 0)->cp == '1' && hist_count(&h) == 0 && s.y == 4,
	      "taller: they come back");
}

static void test_alt(void) {
	start(10, 4);
	feed("normal\x1b[?1049h\x1b[H0123456789\x1b[2;1Hxy");
	scr_resize(&s, 4, 3, &h);
	CHECK(s.alt && scr_cell(&s, 0, 0)->cp == '0' && s.x == 2 && s.y == 1,
	      "the alternate grid is only cut");
	feed("\x1b[?1049l");
	CHECK(!s.alt && scr_cell(&s, 0, 0)->cp == 'n' &&
	          scr_cell(&s, 0, 1)->cp == 'a' && s.x == 2 && s.y == 1,
	      "and the normal one was split again under it");
}

static void test_seal(void) {
	start(10, 2);
	feed("0123456789abcd\r\nx\x1b[2J\x1b[H");
	CHECK(hist_count(&h) == 1 && !h.open,
	      "clearing the screen ends the line that left");
	scr_resize(&s, 20, 2, &h);
	CHECK(scr_cell(&s, 0, 0)->cp == '0' && scr_cell(&s, 10, 0)->cp == 0,
	      "so it comes back alone, not joined to the cleared row");
}

static void test_fuzz(void) {
	static uint8_t buf[4096];
	uint32_t x = 1013904223u;
	start(17, 7);
	int ok = 1;
	for (int round = 0; round < 400 && ok; round++) {
		for (size_t i = 0; i < sizeof(buf); i++) {
			x ^= x << 13, x ^= x >> 17, x ^= x << 5;
			uint8_t b = (uint8_t)x;
			buf[i] =
			    (x >> 8) % 3 == 0
			        ? (uint8_t)"\x1b[;?0123456789mHJKLr@\r\n\xe6\x97\xa5"[b %
			                                                              27]
			    : (x >> 8) % 3 == 1 ? (uint8_t)('a' + b % 26)
			                        : b;
		}
		scr_feed(&s, buf, sizeof(buf));
		s.reply_len = 0;
		x ^= x << 13, x ^= x >> 17, x ^= x << 5;
		scr_resize(&s, 1 + (int)(x % 40), 1 + (int)((x >> 8) % 12), &h);
		ok = s.x >= 0 && s.x < s.cols && s.y >= 0 && s.y < s.rows &&
		     hist_bytes(&h) <= h.limit && lens_hold(&s);
		for (int yy = 0; yy < s.rows && ok; yy++)
			for (int xx = 0; xx < s.cols; xx++) {
				struct cell *c = scr_cell(&s, xx, yy);
				if ((c->attr & ATTR_WIDE) &&
				    (xx + 1 >= s.cols ||
				     !(scr_cell(&s, xx + 1, yy)->attr & ATTR_WIDE_TAIL)))
					ok = 0;
			}
	}
	CHECK(ok, "random bytes and random sizes keep the cursor, the letters and "
	          "the limit");
}

int main(void) {
	test_round_trip();
	test_cursor();
	test_wide();
	test_height();
	test_alt();
	test_seal();
	test_fuzz();
	scr_free(&s);
	hist_free(&h);
	return test_summary();
}
