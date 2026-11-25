#include "history.c"
#include "screen.c"
#include "test.h"
#include "vt.c"
#include "width.c"

static struct scr s;
static char out_lines[8][64];
static int nout, nbell, nclip;
static char clip[64], title[TITLE_MAX];

static void on_scrollout(void *ctx, const struct cell *cells, int cols,
                         uint8_t flags) {
	(void)ctx, (void)flags;
	if (nout < 8) {
		for (int x = 0; x < cols && x < 63; x++)
			out_lines[nout][x] = cells[x].cp ? (char)cells[x].cp : ' ';
		nout++;
	}
}
static void on_bell(void *ctx) { (void)ctx, nbell++; }
static void on_clip(void *ctx, const char *b64, size_t len) {
	(void)ctx;
	nclip++;
	snprintf(clip, sizeof(clip), "%.*s", (int)len, b64);
}
static void on_title(void *ctx, const char *t) {
	(void)ctx;
	snprintf(title, sizeof(title), "%s", t);
}

static void start(int cols, int rows) {
	scr_free(&s);
	scr_init(&s, cols, rows);
	s.hooks.scrollout = on_scrollout;
	s.hooks.bell = on_bell;
	s.hooks.clipboard = on_clip;
	s.hooks.title = on_title;
	nout = nbell = nclip = 0;
	clip[0] = title[0] = 0;
}

static void feed(const char *str) {
	scr_feed(&s, (const uint8_t *)str, strlen(str));
}

static const char *text(int y) {
	static char buf[128];
	int k = 0;
	for (int x = 0; x < s.cols; x++) {
		uint32_t cp = scr_cell(&s, x, y)->cp;
		buf[k++] = cp == 0 ? ' ' : cp < 0x80 ? (char)cp : '#';
	}
	buf[k] = 0;
	return buf;
}

static int replied(const char *want) {
	int ok =
	    s.reply_len == strlen(want) && memcmp(s.reply, want, s.reply_len) == 0;
	s.reply_len = 0;
	return ok;
}

static void test_print(void) {
	start(5, 3);
	feed("abc");
	CHECK(strcmp(text(0), "abc  ") == 0 && s.x == 3, "letters and the cursor");

	feed("defg");
	CHECK(strcmp(text(0), "abcde") == 0 && strcmp(text(1), "fg   ") == 0 &&
	          (scr_line(&s, 0) & LINE_WRAPPED) && s.x == 2 && s.y == 1,
	      "wrapping marks the line");

	start(5, 3);
	feed("abcde");
	CHECK(s.x == 4 && s.pending, "the last column leaves the wrap pending");
	feed("\r");
	CHECK(s.x == 0 && s.y == 0 && !s.pending, "CR cancels the pending wrap");

	start(5, 3);
	feed("\x1b[?7labcdefg");
	CHECK(strcmp(text(0), "abcdg") == 0 && s.y == 0,
	      "without autowrap the last column is overwritten");

	start(5, 3);
	feed("\xe6\x97\xa5");
	CHECK(scr_cell(&s, 0, 0)->cp == 0x65E5 &&
	          (scr_cell(&s, 0, 0)->attr & ATTR_WIDE) &&
	          (scr_cell(&s, 1, 0)->attr & ATTR_WIDE_TAIL) && s.x == 2,
	      "a two-cell letter takes two cells");

	start(5, 3);
	feed("abcd\xe6\x97\xa5");
	CHECK(strcmp(text(0), "abcd ") == 0 && scr_cell(&s, 0, 1)->cp == 0x65E5,
	      "a two-cell letter that does not fit goes to the next line");

	start(5, 3);
	feed("\xe6\x97\xa5\rX");
	CHECK(strcmp(text(0), "X    ") == 0 &&
	          !(scr_cell(&s, 1, 0)->attr & ATTR_WIDE_TAIL),
	      "overwriting half of it wipes the other half");

	start(5, 3);
	feed("e\xcc\x81");
	CHECK(scr_cell(&s, 0, 0)->cp == 'e' &&
	          scr_cell(&s, 0, 0)->comb[0] == 0x301 && s.x == 1,
	      "a combining accent joins the letter before");

	start(5, 3);
	feed("\x1b(0q\x1b(Bq");
	CHECK(scr_cell(&s, 0, 0)->cp == 0x2500 && scr_cell(&s, 1, 0)->cp == 'q',
	      "DEC line drawing");

	start(10, 3);
	feed("a\x1b[3b");
	CHECK(strcmp(text(0), "aaaa      ") == 0, "REP repeats the last letter");

	start(20, 3);
	feed("\tX");
	CHECK(scr_cell(&s, 8, 0)->cp == 'X', "a tab goes to the next stop");
	feed("\r\x1b[3g\x1b[5G\x1bH\r\tY");
	CHECK(scr_cell(&s, 4, 0)->cp == 'Y', "stops are set and cleared");
}

static void test_scroll(void) {
	start(5, 3);
	feed("1\r\n2\r\n3\r\n4");
	CHECK(strcmp(text(0), "2    ") == 0 && strcmp(text(2), "4    ") == 0 &&
	          nout == 1 && out_lines[0][0] == '1',
	      "a line feed at the bottom scrolls and the top line leaves");

	start(5, 4);
	feed("a\r\nb\r\nc\r\nd\x1b[2;3r\x1b[3;1H\nx");
	CHECK(strcmp(text(0), "a    ") == 0 && strcmp(text(1), "c    ") == 0 &&
	          strcmp(text(2), "x    ") == 0 && strcmp(text(3), "d    ") == 0 &&
	          nout == 0,
	      "a scroll region scrolls alone, and its lines do not leave");

	start(5, 3);
	feed("a\r\nb\r\nc\x1b[1;1H\x1bM");
	CHECK(strcmp(text(0), "     ") == 0 && strcmp(text(1), "a    ") == 0,
	      "reverse index at the top scrolls down");

	start(5, 4);
	feed("a\r\nb\r\nc\x1b[2;1H\x1b[L");
	CHECK(strcmp(text(1), "     ") == 0 && strcmp(text(2), "b    ") == 0, "IL");
	feed("\x1b[M");
	CHECK(strcmp(text(1), "b    ") == 0 && nout == 0,
	      "DL, and its lines do not leave");
}

static void test_erase(void) {
	start(5, 3);
	feed("abcde\x1b[1;3H\x1b[K");
	CHECK(strcmp(text(0), "ab   ") == 0, "EL to the end");
	feed("\x1b[44m\x1b[2J");
	CHECK(scr_cell(&s, 0, 0)->bg == COLOR_INDEX(4) &&
	          scr_cell(&s, 4, 2)->bg == COLOR_INDEX(4),
	      "erasing keeps the background of the pen");

	start(5, 3);
	feed("abcde\x1b[1;2H\x1b[2@");
	CHECK(strcmp(text(0), "a  bc") == 0, "ICH");
	feed("\x1b[P");
	CHECK(strcmp(text(0), "a bc ") == 0, "DCH");
	feed("\x1b[1;1H\x1b[3X");
	CHECK(strcmp(text(0), "   c ") == 0, "ECH");
}

static void test_sgr(void) {
	start(5, 3);
	feed("\x1b[1;3;4:3;38;2;10;20;30;48:5:200;58:2::1:2:3mx");
	struct cell *c = scr_cell(&s, 0, 0);
	CHECK((c->attr & ATTR_BOLD) && (c->attr & ATTR_ITALIC) &&
	          (c->attr & ATTR_UL_MASK) >> ATTR_UL_SHIFT == UL_CURLY &&
	          c->fg == COLOR_RGB(10, 20, 30) && c->bg == COLOR_INDEX(200) &&
	          c->ul == COLOR_RGB(1, 2, 3),
	      "styles and colours, with ';' and ':'");
	feed("\x1b[22;24;39my");
	c = scr_cell(&s, 1, 0);
	CHECK(!(c->attr & ATTR_BOLD) && (c->attr & ATTR_ITALIC) &&
	          !(c->attr & ATTR_UL_MASK) && c->fg == COLOR_DEFAULT,
	      "turning styles off one by one");
	feed("\x1b[mz");
	c = scr_cell(&s, 2, 0);
	CHECK(c->attr == 0 && c->bg == COLOR_DEFAULT, "SGR 0 resets");
	feed("\x1b[38:2:5:6:7;97mw");
	CHECK(scr_cell(&s, 3, 0)->fg == COLOR_INDEX(15),
	      "a later colour wins, and ':' without colour space reads");
}

static void test_modes(void) {
	start(5, 3);
	feed("a\x1b[?1049hb");
	CHECK(s.alt && scr_cell(&s, 1, 0)->cp == 'b', "the alternate grid");
	feed("\x1b[?1049l");
	CHECK(!s.alt && strcmp(text(0), "a    ") == 0 && s.x == 1,
	      "leaving it restores the grid and the cursor");

	start(5, 5);
	feed("\x1b[2;4r\x1b[?6h\x1b[1;1Hx");
	CHECK(scr_cell(&s, 0, 1)->cp == 'x', "origin mode counts from the region");
	feed("\x1b[10;1Hy");
	CHECK(scr_cell(&s, 0, 3)->cp == 'y', "and keeps the cursor inside it");

	start(5, 3);
	feed("\x1b[2;3H\x1b"
	     "7\x1b[1;1H\x1b"
	     "8z");
	CHECK(scr_cell(&s, 2, 1)->cp == 'z', "DECSC and DECRC");

	start(5, 3);
	feed("\x1b[6 q");
	CHECK(s.cursor_shape == CURSOR_BAR && !s.cursor_blink, "DECSCUSR");
	feed("\x1bP$q q\x1b\\");
	CHECK(replied("\x1bP1$r6 q\x1b\\"), "DECRQSS of the cursor");

	feed("abc\x1b"
	     "c");
	CHECK(strcmp(text(0), "     ") == 0 && s.x == 0 && s.cursor_visible, "RIS");
}

static void test_replies(void) {
	start(10, 4);
	feed("\x1b[c");
	CHECK(replied(DA1_REPLY), "DA1 answers as xterm");
	feed("\x1b[>c");
	CHECK(replied(DA2_REPLY), "DA2 answers as xterm");
	feed("\x1b[>q");
	CHECK(replied(""), "XTVERSION gets no answer");
	feed("\x1bP+q544e\x1b\\");
	CHECK(replied("\x1bP0+r\x1b\\"), "XTGETTCAP reports nothing");
	feed("\x1b[21t");
	CHECK(replied(""), "the title is never sent back");
	feed("\x1b[18t");
	CHECK(replied("\x1b[8;4;10t"), "size in cells");
	feed("\x1b[3;5H\x1b[6n");
	CHECK(replied("\x1b[3;5R"), "cursor position");
	feed("\x1b[?2026$p");
	CHECK(replied("\x1b[?2026;2$y"), "DECRQM of a mode that is off");
	feed("\x1b[?2026h\x1b[?2026$p");
	CHECK(replied("\x1b[?2026;1$y"), "and on");
	feed("\x1b[?9999$p");
	CHECK(replied("\x1b[?9999;0$y"), "and unknown");
	feed("\x1b[?u");
	CHECK(replied("\x1b[?0u"), "kitty keyboard flags start at 0");
	feed("\x1b[>31u\x1b[?u");
	CHECK(replied("\x1b[?31u"), "a push sets them");
	feed("\x1b[<u\x1b[?u");
	CHECK(replied("\x1b[?0u"), "a pop restores them");
	feed("\x1b]11;?\x07");
	CHECK(replied("\x1b]11;rgb:0000/0000/0000\x1b\\"),
	      "a dark background answers black");
	feed("\x1b]10;?\x07");
	CHECK(replied("\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
	      "its light foreground answers white");
	feed("\x1b[1;38:2::1:2:3m\x1bP$qm\x1b\\");
	CHECK(replied("\x1bP1$r0;1;38:2::1:2:3m\x1b\\"), "DECRQSS of the pen");
}

static void test_osc(void) {
	start(10, 3);
	feed("\x1b]52;c;?\x07");
	CHECK(nclip == 0, "reading the clipboard is refused");
	feed("\x1b]52;c;aGk=\x07");
	CHECK(nclip == 1 && strcmp(clip, "aGk=") == 0, "writing it goes through");

	feed("\x1b]2;nvim\x01 main.c\x07");
	CHECK(strcmp(title, "nvim main.c") == 0, "the title, without controls");

	feed("\x1b]8;;https://a\x1b\\x\x1b]8;;\x1b\\y");
	CHECK(scr_cell(&s, 0, 0)->link == 1 && scr_cell(&s, 1, 0)->link == 0 &&
	          strcmp(s.links[0], "https://a") == 0,
	      "hyperlinks");

	feed("\x07");
	CHECK(nbell == 1, "the bell");

	feed("\x1b]4;1;#ff0000\x07");
	CHECK(s.palette_set[1] && s.palette[1] == COLOR_RGB(255, 0, 0),
	      "a program sets a palette colour");
	feed("\x1b]4;2;rgb:00/80/ff\x07");
	CHECK(s.palette[2] == COLOR_RGB(0, 0x80, 0xFF), "with rgb:");
	feed("\x1b]104\x07");
	CHECK(!s.palette_set[1] && !s.palette_set[2], "and resets them");

	start(10, 3);
	feed("\x1b]133;A\x07$ ls");
	CHECK(scr_line(&s, 0) & LINE_PROMPT, "OSC 133 marks the prompt line");
}

static void test_fuzz(void) {
	static uint8_t buf[1 << 15];
	uint32_t x = 88172645u;
	start(17, 7);
	for (int round = 0; round < 64; round++) {
		for (size_t i = 0; i < sizeof(buf); i++) {
			x ^= x << 13, x ^= x >> 17, x ^= x << 5;
			uint8_t b = (uint8_t)x;
			buf[i] =
			    (x >> 8) % 3 == 0
			        ? (uint8_t)"\x1b[;:?0123456789mHJKLMPr@hl\xe6\xcc"[b % 28]
			        : b;
		}
		scr_feed(&s, buf, sizeof(buf));
		s.reply_len = 0;
	}
	int ok = s.x >= 0 && s.x < s.cols && s.y >= 0 && s.y < s.rows &&
	         s.top <= s.bot && s.bot < s.rows;
	for (int y = 0; y < s.rows; y++)
		for (int xx = 0; xx < s.cols; xx++) {
			struct cell *c = scr_cell(&s, xx, y);
			if ((c->attr & ATTR_WIDE) &&
			    (xx + 1 >= s.cols ||
			     !(scr_cell(&s, xx + 1, y)->attr & ATTR_WIDE_TAIL)))
				ok = 0;
			if ((c->attr & ATTR_WIDE_TAIL) &&
			    (xx == 0 || !(scr_cell(&s, xx - 1, y)->attr & ATTR_WIDE)))
				ok = 0;
		}
	CHECK(ok, "2 MiB of random bytes leave the cursor inside and no half "
	          "letter");
}

int main(void) {
	test_print();
	test_scroll();
	test_erase();
	test_sgr();
	test_modes();
	test_replies();
	test_osc();
	test_fuzz();
	scr_free(&s);
	return test_summary();
}
