#include "test.h"
#include "vt.c"

#include <stdlib.h>
#include <string.h>

#define MAX_EV 64

struct rec {
	enum vt_kind kind;
	uint32_t cp;
	char str[256];
	size_t len;
	struct vt_seq seq;
};

static struct rec ev[MAX_EV];
static int nev;
static struct vt vt;

static void sink(void *ctx, const struct vt_event *e) {
	(void)ctx;
	if (nev == MAX_EV)
		return;
	struct rec *r = &ev[nev++];
	memset(r, 0, sizeof(*r));
	r->kind = e->kind;
	r->cp = e->cp;
	r->len = e->len;
	if (e->str)
		memcpy(r->str, e->str, e->len < 255 ? e->len : 255);
	if (e->seq)
		r->seq = *e->seq;
}

static void feed(const char *s) {
	nev = 0;
	vt_init(&vt);
	vt_feed(&vt, (const uint8_t *)s, strlen(s), sink, NULL);
}

static void feed_bytes(const char *s) {
	nev = 0;
	vt_init(&vt);
	for (size_t i = 0; s[i]; i++)
		vt_feed(&vt, (const uint8_t *)s + i, 1, sink, NULL);
}

static int is_print(int i, uint32_t cp) {
	return i < nev && ev[i].kind == VT_PRINT && ev[i].cp == cp;
}

static int is_csi(int i, char final) {
	return i < nev && ev[i].kind == VT_CSI && ev[i].seq.final == final;
}

static void test_text(void) {
	feed("hola mundo");
	CHECK(nev == 1 && ev[0].kind == VT_PRINT_RUN && ev[0].len == 10 &&
	          memcmp(ev[0].str, "hola mundo", 10) == 0,
	      "ASCII comes as one run");

	feed("\xc3\xb1\xe6\x97\xa5\xf0\x9f\x99\x82");
	CHECK(nev == 3 && is_print(0, 0xF1) && is_print(1, 0x65E5) &&
	          is_print(2, 0x1F642),
	      "two, three and four byte UTF-8");

	feed_bytes("\xe6\x97\xa5");
	CHECK(nev == 1 && is_print(0, 0x65E5), "UTF-8 split across calls");

	feed("\xc3"
	     "A");
	CHECK(nev == 2 && is_print(0, 0xFFFD) && is_print(1, 'A'),
	      "a cut sequence is U+FFFD and the next byte stays");

	feed("\xc0\x80");
	CHECK(nev == 2 && is_print(0, 0xFFFD) && is_print(1, 0xFFFD),
	      "an overlong NUL is two U+FFFD");

	feed("\xed\xa0\x80");
	CHECK(nev == 1 && is_print(0, 0xFFFD), "a surrogate is U+FFFD");

	feed("\xc2\x9b"
	     "1m");
	CHECK(nev == 1 && ev[0].kind == VT_PRINT_RUN,
	      "a C1 CSI in UTF-8 is dropped and opens nothing");

	feed("a\x7f"
	     "b");
	CHECK(nev == 2 && ev[0].kind == VT_PRINT_RUN && ev[1].kind == VT_PRINT_RUN,
	      "DEL is ignored");

	feed("\r\n");
	CHECK(nev == 2 && ev[0].kind == VT_EXECUTE && ev[0].cp == '\r' &&
	          ev[1].cp == '\n',
	      "C0 controls execute");
}

static void test_csi(void) {
	feed("\x1b[1;2H");
	CHECK(nev == 1 && is_csi(0, 'H') && ev[0].seq.nparams == 2 &&
	          vt_param(&ev[0].seq, 0, 1) == 1 &&
	          vt_param(&ev[0].seq, 1, 1) == 2,
	      "CSI with two params");

	feed("\x1b[;5H");
	CHECK(is_csi(0, 'H') && vt_param(&ev[0].seq, 0, 1) == 1 &&
	          vt_param(&ev[0].seq, 1, 1) == 5,
	      "an empty param takes the default");

	feed("\x1b[H");
	CHECK(is_csi(0, 'H') && ev[0].seq.nparams == 0, "no params");

	feed("\x1b[?1049h");
	CHECK(is_csi(0, 'h') && ev[0].seq.priv == '?' &&
	          vt_param(&ev[0].seq, 0, 0) == 1049,
	      "private marker");

	feed("\x1b[4:3m");
	CHECK(is_csi(0, 'm') && ev[0].seq.nparams == 2 && ev[0].seq.param[1] == 3 &&
	          ev[0].seq.sub == 2,
	      "sub-param of a curly underline");

	feed("\x1b[58:2::255:0:0m");
	CHECK(is_csi(0, 'm') && ev[0].seq.nparams == 6 && ev[0].seq.sub == 0x3E &&
	          (ev[0].seq.empty >> 2 & 1) && ev[0].seq.param[3] == 255,
	      "underline colour with an empty colour space");

	feed("\x1b[2 q");
	CHECK(is_csi(0, 'q') && strcmp(ev[0].seq.inter, " ") == 0 &&
	          vt_param(&ev[0].seq, 0, 0) == 2,
	      "intermediate byte");

	feed("\x1b[>q");
	CHECK(is_csi(0, 'q') && ev[0].seq.priv == '>', "XTVERSION");

	feed("\x1b[99999999A");
	CHECK(is_csi(0, 'A') && ev[0].seq.param[0] == VT_PARAM_MAX,
	      "a huge param is clamped");

	char many[200] = "\x1b[";
	for (int i = 0; i < 40; i++)
		strcat(many, "1;");
	strcat(many, "m");
	feed(many);
	CHECK(nev == 1 && is_csi(0, 'm') && ev[0].seq.nparams == VT_MAX_PARAMS,
	      "params past the limit are dropped");

	feed("\x1b[ 1q");
	CHECK(nev == 0, "a param after an intermediate is ignored");

	feed("\x1b[1?h");
	CHECK(nev == 0, "a private marker after a param is ignored");

	feed("\x1b[1\n2H");
	CHECK(nev == 2 && ev[0].kind == VT_EXECUTE && is_csi(1, 'H') &&
	          vt_param(&ev[1].seq, 0, 0) == 12,
	      "a C0 inside a CSI executes and the CSI goes on");

	feed("\x1b[1\x18"
	     "A");
	CHECK(nev == 2 && ev[0].kind == VT_EXECUTE && ev[0].cp == 0x18 &&
	          ev[1].kind == VT_PRINT_RUN,
	      "CAN cancels a CSI");

	feed_bytes("\x1b[38;2;10;20;30m");
	CHECK(nev == 1 && is_csi(0, 'm') && ev[0].seq.nparams == 5 &&
	          ev[0].seq.param[4] == 30,
	      "CSI split across calls");

	feed("ab\x1b[mcd");
	CHECK(nev == 3 && ev[0].kind == VT_PRINT_RUN && is_csi(1, 'm') &&
	          ev[2].kind == VT_PRINT_RUN && ev[2].str[0] == 'c',
	      "runs stop at a sequence and start again after it");
}

static void test_esc(void) {
	feed("\x1b"
	     "7");
	CHECK(nev == 1 && ev[0].kind == VT_ESC && ev[0].seq.final == '7', "ESC 7");

	feed("\x1b(0");
	CHECK(nev == 1 && ev[0].kind == VT_ESC && ev[0].seq.final == '0' &&
	          strcmp(ev[0].seq.inter, "(") == 0,
	      "charset designation");
}

static void test_strings(void) {
	feed("\x1b]0;t\xc3\xadtulo\x07");
	CHECK(nev == 1 && ev[0].kind == VT_OSC && ev[0].len == 9 &&
	          memcmp(ev[0].str, "0;t\xc3\xadtulo", 9) == 0,
	      "OSC ended by BEL, with UTF-8 inside");

	feed("\x1b]2;x\x1b\\");
	CHECK(nev == 2 && ev[0].kind == VT_OSC && ev[0].len == 3 &&
	          ev[1].kind == VT_ESC && ev[1].seq.final == '\\',
	      "OSC ended by ST");

	feed_bytes("\x1b]8;;https://x\x1b\\");
	CHECK(nev == 2 && ev[0].kind == VT_OSC && ev[0].len == 12,
	      "OSC split across calls");

	size_t n = VT_STR_MAX + 16;
	char *big = malloc(n + 8);
	memcpy(big, "\x1b]52;", 5);
	memset(big + 5, 'A', n);
	memcpy(big + 5 + n, "\x07", 2);
	feed(big);
	free(big);
	CHECK(nev == 0, "an OSC past the limit is dropped");

	feed("\x1bP$qm\x1b\\");
	CHECK(nev == 2 && ev[0].kind == VT_DCS && ev[0].seq.final == 'q' &&
	          strcmp(ev[0].seq.inter, "$") == 0 && ev[0].len == 1 &&
	          ev[0].str[0] == 'm',
	      "DECRQSS");

	feed("\x1bP+q544e\x1b\\");
	CHECK(ev[0].kind == VT_DCS && strcmp(ev[0].seq.inter, "+") == 0 &&
	          ev[0].len == 4,
	      "XTGETTCAP");

	feed("\x1b_Gabc\x1b\\");
	CHECK(nev == 2 && ev[0].kind == VT_APC && ev[0].len == 4, "APC");

	feed("\x1bXabc\x1b\\");
	CHECK(nev == 1 && ev[0].kind == VT_ESC, "SOS is swallowed");

	feed("\x1b]0;abc\x18x");
	CHECK(nev == 2 && ev[0].kind == VT_EXECUTE && ev[1].kind == VT_PRINT_RUN,
	      "CAN drops an unfinished OSC");
}

static int fuzz_ok;

static void fuzz_sink(void *ctx, const struct vt_event *e) {
	(void)ctx;
	if (e->len > VT_STR_MAX || (e->seq && e->seq->nparams > VT_MAX_PARAMS))
		fuzz_ok = 0;
}

static void test_fuzz(void) {
	static uint8_t buf[1 << 16];
	uint32_t x = 2463534242u;
	fuzz_ok = 1;
	vt_init(&vt);
	for (int round = 0; round < 64; round++) {
		for (size_t i = 0; i < sizeof(buf); i++) {
			x ^= x << 13, x ^= x >> 17, x ^= x << 5;
			uint8_t b = (uint8_t)x;
			buf[i] = (x >> 8) % 4 == 0
			             ? (uint8_t)"\x1b[;:?P]_\x07\xc3\xe6"[b % 12]
			             : b;
		}
		vt_feed(&vt, buf, sizeof(buf), fuzz_sink, NULL);
	}
	CHECK(fuzz_ok, "4 MiB of random bytes keep every event within its limits");
}

int main(void) {
	test_fuzz();
	test_text();
	test_csi();
	test_esc();
	test_strings();
	return test_summary();
}
