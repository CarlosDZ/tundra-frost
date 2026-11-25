#include "keys.c"
#include "test.h"

#define S MOD_SHIFT
#define A MOD_ALT
#define C MOD_CTRL
#define CAPS MOD_CAPS_LOCK
#define NUM MOD_NUM_LOCK
#define DIS KEYS_DISAMBIGUATE
#define EV KEYS_EVENT_TYPES
#define ALT KEYS_ALTERNATES
#define ALL KEYS_ALL
#define TXT KEYS_TEXT
#define P KEY_PRESS
#define R KEY_REPEAT
#define U KEY_RELEASE

struct row {
	unsigned flags;
	int app;
	uint32_t code, shifted, base;
	unsigned mods;
	int type;
	const char *text, *want;
	size_t len;
};

static const struct row rows[] = {
    {0, 0, 'a', 'A', 0, 0, P, "a", "a", 0},
    {0, 0, 'a', 'A', 0, S, P, "A", "A", 0},
    {0, 0, 'a', 'A', 0, C, P, "a", "\x01", 0},
    {0, 0, 'a', 'A', 0, A, P, "a",
     "\x1b"
     "a",
     0},
    {0, 0, 'a', 'A', 0, C | A, P, "a", "\x1b\x01", 0},
    {0, 0, 'a', 'A', 0, A | S, P, "A",
     "\x1b"
     "A",
     0},
    {0, 0, 'a', 'A', 0, C | S, P, "A", "\x1b[97;6u", 0},
    {0, 0, 'a', 'A', 0, MOD_SUPER, P, "a", "\x1b[97;9u", 0},
    {0, 0, 'a', 'A', 0, CAPS, P, "A", "A", 0},
    {0, 0, 'a', 'A', 0, 0, R, "a", "a", 0},
    {0, 0, 'a', 'A', 0, 0, U, "a", "", 0},
    {0, 0, 'i', 'I', 0, C, P, "i", "\t", 0},
    {0, 0, '3', '#', 0, C, P, "3", "\x1b", 0},
    {0, 0, ';', ':', 0, C, P, ";", ";", 0},
    {0, 0, '[', '{', 0, C, P, "[", "\x1b", 0},
    {0, 0, ' ', 0, 0, C, P, " ", "", 1},
    {0, 0, ' ', 0, 0, C | S, P, " ", "", 1},
    {0, 0, ' ', 0, 0, A, P, " ", "\x1b ", 0},
    {0, 0, 0xf1, 0xd1, ';', 0, P, "ñ", "ñ", 0},
    {0, 0, 0xf1, 0xd1, ';', A, P, "ñ", "\x1bñ", 0},
    {0, 0, 0x441, 0x421, 'c', C, P, "с", "\x03", 0},
    {0, 0, '2', '"', 0, 0, P, "@", "@", 0},
    {0, 0, 0, 0, 0, 0, P, "é", "é", 0},
    {0, 0, 0, 0, 0, A, P, "", "", 0},
    {0, 0, FKEY_ENTER, 0, 0, 0, P, "\r", "\r", 0},
    {0, 0, FKEY_ENTER, 0, 0, A, P, "\r", "\x1b\r", 0},
    {0, 0, FKEY_ENTER, 0, 0, S, P, "\r", "\r", 0},
    {0, 0, FKEY_ESCAPE, 0, 0, 0, P, "\x1b", "\x1b", 0},
    {0, 0, FKEY_ESCAPE, 0, 0, A, P, "\x1b", "\x1b\x1b", 0},
    {0, 0, FKEY_BACKSPACE, 0, 0, 0, P, "\b", "\x7f", 0},
    {0, 0, FKEY_BACKSPACE, 0, 0, C, P, "\b", "\b", 0},
    {0, 0, FKEY_BACKSPACE, 0, 0, A | C, P, "\b", "\x1b\b", 0},
    {0, 0, FKEY_TAB, 0, 0, 0, P, "\t", "\t", 0},
    {0, 0, FKEY_TAB, 0, 0, S, P, "\t", "\x1b[Z", 0},
    {0, 0, FKEY_TAB, 0, 0, A | S, P, "\t", "\x1b\x1b[Z", 0},
    {0, 0, FKEY_TAB, 0, 0, C, P, "\t", "\t", 0},
    {0, 0, FKEY_ENTER, 0, 0, MOD_SUPER, P, "\r", "\x1b[13;9u", 0},
    {0, 0, FKEY_UP, 0, 0, 0, P, "", "\x1b[A", 0},
    {0, 1, FKEY_UP, 0, 0, 0, P, "", "\x1bOA", 0},
    {0, 0, FKEY_UP, 0, 0, C, P, "", "\x1b[1;5A", 0},
    {0, 1, FKEY_UP, 0, 0, C, P, "", "\x1b[1;5A", 0},
    {0, 0, FKEY_HOME, 0, 0, 0, P, "", "\x1b[H", 0},
    {0, 1, FKEY_END, 0, 0, 0, P, "", "\x1bOF", 0},
    {0, 0, FKEY_F(1), 0, 0, 0, P, "", "\x1bOP", 0},
    {0, 0, FKEY_F(1), 0, 0, S, P, "", "\x1b[1;2P", 0},
    {0, 0, FKEY_F(3), 0, 0, 0, P, "", "\x1bOR", 0},
    {0, 0, FKEY_F(3), 0, 0, C, P, "", "\x1b[1;5R", 0},
    {0, 0, FKEY_F(5), 0, 0, 0, P, "", "\x1b[15~", 0},
    {0, 0, FKEY_F(5), 0, 0, S, P, "", "\x1b[15;2~", 0},
    {0, 0, FKEY_F(12), 0, 0, 0, P, "", "\x1b[24~", 0},
    {0, 0, FKEY_DELETE, 0, 0, 0, P, "", "\x1b[3~", 0},
    {0, 0, FKEY_PAGE_UP, 0, 0, A, P, "", "\x1b[5;3~", 0},
    {0, 0, FKEY_MENU, 0, 0, 0, P, "", "\x1b[29~", 0},
    {0, 0, FKEY_F(13), 0, 0, 0, P, "", "\x1b[57376u", 0},
    {0, 0, FKEY_UP, 0, 0, NUM, P, "", "\x1b[A", 0},
    {0, 0, FKEY_LEFT_SHIFT, 0, 0, S, P, "", "", 0},
    {0, 0, FKEY_CAPS_LOCK, 0, 0, CAPS, P, "", "", 0},
    {0, 0, FKEY_KP_0 + 1, 0, 0, NUM, P, "1", "1", 0},
    {0, 0, FKEY_KP_END, 0, 0, 0, P, "", "\x1b[F", 0},
    {0, 0, FKEY_KP_ENTER, 0, 0, 0, P, "\r", "\r", 0},
    {0, 0, FKEY_KP_BEGIN, 0, 0, 0, P, "", "\x1b[E", 0},
    {0, 0, FKEY_KP_ADD, 0, 0, NUM, P, "+", "+", 0},

    {DIS, 0, 'a', 'A', 0, 0, P, "a", "a", 0},
    {DIS, 0, 'a', 'A', 0, S, P, "A", "A", 0},
    {DIS, 0, FKEY_ESCAPE, 0, 0, 0, P, "\x1b", "\x1b[27u", 0},
    {DIS, 0, 'a', 'A', 0, A, P, "a", "\x1b[97;3u", 0},
    {DIS, 0, 'a', 'A', 0, A | S, P, "A", "\x1b[97;4u", 0},
    {DIS, 0, 'a', 'A', 0, C, P, "a", "\x1b[97;5u", 0},
    {DIS, 0, 'a', 'A', 0, C | S, P, "A", "\x1b[97;6u", 0},
    {DIS, 0, 'a', 'A', 0, C | CAPS, P, "A", "\x1b[97;5u", 0},
    {DIS, 0, 'i', 'I', 0, C, P, "i", "\x1b[105;5u", 0},
    {DIS, 0, '[', '{', 0, C, P, "[", "\x1b[91;5u", 0},
    {DIS, 0, ' ', 0, 0, C, P, " ", "\x1b[32;5u", 0},
    {DIS, 0, FKEY_ENTER, 0, 0, 0, P, "\r", "\r", 0},
    {DIS, 0, FKEY_ENTER, 0, 0, S, P, "\r", "\x1b[13;2u", 0},
    {DIS, 0, FKEY_TAB, 0, 0, 0, P, "\t", "\t", 0},
    {DIS, 0, FKEY_TAB, 0, 0, S, P, "\t", "\x1b[9;2u", 0},
    {DIS, 0, FKEY_BACKSPACE, 0, 0, 0, P, "\b", "\x7f", 0},
    {DIS, 0, FKEY_BACKSPACE, 0, 0, C, P, "\b", "\x1b[127;5u", 0},
    {DIS, 0, FKEY_UP, 0, 0, 0, P, "", "\x1b[A", 0},
    {DIS, 1, FKEY_UP, 0, 0, 0, P, "", "\x1b[A", 0},
    {DIS, 0, FKEY_UP, 0, 0, C, P, "", "\x1b[1;5A", 0},
    {DIS, 0, FKEY_UP, 0, 0, NUM, P, "", "\x1b[1;129A", 0},
    {DIS, 0, FKEY_F(1), 0, 0, 0, P, "", "\x1b[P", 0},
    {DIS, 0, FKEY_F(3), 0, 0, 0, P, "", "\x1b[13~", 0},
    {DIS, 0, FKEY_F(5), 0, 0, A, P, "", "\x1b[15;3~", 0},
    {DIS, 0, FKEY_MENU, 0, 0, 0, P, "", "\x1b[57363u", 0},
    {DIS, 0, FKEY_KP_0 + 1, 0, 0, NUM, P, "1", "1", 0},
    {DIS, 0, FKEY_KP_0 + 1, 0, 0, C | NUM, P, "1", "\x1b[57400;5u", 0},
    {DIS, 0, FKEY_KP_END, 0, 0, 0, P, "", "\x1b[57424u", 0},
    {DIS, 0, FKEY_KP_ENTER, 0, 0, 0, P, "\r", "\x1b[57414u", 0},
    {DIS, 0, FKEY_LEFT_SHIFT, 0, 0, S, P, "", "", 0},
    {DIS, 0, 'a', 'A', 0, C, U, "a", "", 0},
    {DIS, 0, 'a', 'A', 0, C, R, "a", "\x1b[97;5u", 0},
    {DIS, 0, 0, 0, 0, 0, P, "é", "é", 0},

    {DIS | EV, 0, 'a', 'A', 0, C, U, "a", "\x1b[97;5:3u", 0},
    {DIS | EV, 0, 'a', 'A', 0, C, R, "a", "\x1b[97;5:2u", 0},
    {DIS | EV, 0, 'a', 'A', 0, 0, R, "a", "a", 0},
    {DIS | EV, 0, 'a', 'A', 0, 0, U, "a", "", 0},
    {DIS | EV, 0, FKEY_UP, 0, 0, 0, U, "", "\x1b[1;1:3A", 0},
    {DIS | EV, 0, FKEY_ESCAPE, 0, 0, 0, U, "\x1b", "\x1b[27;1:3u", 0},
    {DIS | EV, 0, FKEY_F(5), 0, 0, 0, R, "", "\x1b[15;1:2~", 0},
    {DIS | EV, 0, FKEY_ENTER, 0, 0, 0, U, "\r", "", 0},
    {DIS | EV, 0, FKEY_ENTER, 0, 0, S, U, "\r", "", 0},

    {DIS | ALT, 0, 'a', 'A', 0, C | S, P, "A", "\x1b[97:65;6u", 0},
    {DIS | ALT, 0, 'a', 'A', 0, C, P, "a", "\x1b[97;5u", 0},
    {DIS | ALT, 0, 0x441, 0x421, 'c', C, P, "с", "\x1b[1089::99;5u", 0},
    {DIS | ALT, 0, 0x441, 0x421, 'c', C | S, P, "С", "\x1b[1089:1057:99;6u", 0},

    {DIS | ALL, 0, 'a', 'A', 0, 0, P, "a", "\x1b[97u", 0},
    {DIS | ALL, 0, 'a', 'A', 0, S, P, "A", "\x1b[97;2u", 0},
    {DIS | ALL, 0, 'a', 'A', 0, CAPS, P, "A", "\x1b[97;65u", 0},
    {DIS | ALL, 0, FKEY_ENTER, 0, 0, 0, P, "\r", "\x1b[13u", 0},
    {DIS | ALL, 0, FKEY_TAB, 0, 0, 0, P, "\t", "\x1b[9u", 0},
    {DIS | ALL, 0, FKEY_BACKSPACE, 0, 0, 0, P, "\b", "\x1b[127u", 0},
    {DIS | ALL, 0, FKEY_LEFT_SHIFT, 0, 0, S, P, "", "\x1b[57441;2u", 0},
    {DIS | ALL, 0, FKEY_KP_0 + 1, 0, 0, NUM, P, "1", "\x1b[57400;129u", 0},
    {DIS | ALL | EV, 0, FKEY_LEFT_SHIFT, 0, 0, 0, U, "", "\x1b[57441;1:3u", 0},
    {DIS | ALL | EV, 0, 'a', 'A', 0, 0, U, "a", "\x1b[97;1:3u", 0},
    {DIS | ALL | EV, 0, FKEY_ENTER, 0, 0, 0, U, "\r", "\x1b[13;1:3u", 0},

    {DIS | ALL | TXT, 0, 'a', 'A', 0, 0, P, "a", "\x1b[97;;97u", 0},
    {DIS | ALL | TXT, 0, 'a', 'A', 0, S, P, "A", "\x1b[97;2;65u", 0},
    {DIS | ALL | TXT, 0, 'a', 'A', 0, C, P, "a", "\x1b[97;5u", 0},
    {DIS | ALL | TXT, 0, FKEY_ENTER, 0, 0, 0, P, "\r", "\x1b[13u", 0},
    {DIS | ALL | TXT, 0, 0, 0, 0, 0, P, "å", "\x1b[0;;229u", 0},
    {DIS | ALL | TXT, 0, 0xf1, 0xd1, ';', 0, P, "ñ", "\x1b[241;;241u", 0},
    {DIS | ALL | TXT | EV, 0, 'a', 'A', 0, 0, U, "a", "\x1b[97;1:3u", 0},
    {DIS | ALL | TXT, 0, 'e', 'E', 0, 0, P, "e\xcc\x81", "\x1b[101;;101:769u",
     0},
};

static void show(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == 0x1b)
			printf("\\e");
		else if (c < 0x20 || c == 0x7f)
			printf("\\x%02x", c);
		else
			putchar(c);
	}
}

int main(void) {
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		const struct row *r = &rows[i];
		struct key k = {r->code, r->shifted, r->base,
		                r->mods, r->type,    r->text};
		struct key_modes m = {r->flags, r->app, 0};
		char out[KEY_OUT_MAX];
		size_t n = key_encode(&k, &m, out);
		size_t want = r->len ? r->len : strlen(r->want);
		int ok = n == want && !memcmp(out, r->want, n);
		char desc[160];
		snprintf(desc, sizeof(desc),
		         "flags %u, code %u, mods %u, type %d gives the table's bytes",
		         r->flags, r->code, r->mods, r->type);
		CHECK(ok, desc);
		if (!ok) {
			printf("# got  ");
			show(out, n);
			printf("\n# want ");
			show(r->want, want);
			printf("\n");
		}
	}

	struct key k = {FKEY_ENTER, 0, 0, 0, KEY_PRESS, "\r"};
	struct key_modes m = {0, 0, 1};
	char out[KEY_OUT_MAX];
	size_t n = key_encode(&k, &m, out);
	CHECK(n == 2 && !memcmp(out, "\r\n", 2), "in newline mode, Enter is CR LF");

	char big[600];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	struct key t = {0, 0, 0, 0, KEY_PRESS, big};
	m = (struct key_modes){KEYS_DISAMBIGUATE | KEYS_ALL | KEYS_TEXT, 0, 0};
	n = key_encode(&t, &m, out);
	CHECK(n <= KEY_OUT_MAX, "a long text does not write past the buffer");
	return test_summary();
}
