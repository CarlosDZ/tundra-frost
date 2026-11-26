#include "paste.c"
#include "test.h"

static int filters(const char *in, int bracketed, const char *want) {
	char out[256];
	size_t n = paste_filter(in, strlen(in), bracketed, out);
	return n == strlen(want) && !memcmp(out, want, n);
}

static int decodes(const char *in, const char *want) {
	char out[64];
	long n = base64_decode(in, strlen(in), out);
	return want ? n == (long)strlen(want) && !memcmp(out, want, (size_t)n)
	            : n == -1;
}

int main(void) {
	CHECK(filters("ls -l", 0, "ls -l"), "plain text goes as it is");
	CHECK(filters("a\nb\r\nc\rd", 0, "a\rb\rc\rd"),
	      "each line break, of any kind, is one Enter");
	CHECK(filters("a\tb", 0, "a\tb"), "a tab stays");
	CHECK(filters("rm\x1b[A -rf\x07\x08 ~", 0, "rm[A -rf ~"),
	      "control characters go: no ESC, bell or backspace");
	CHECK(filters("a\xc2\x9b"
	              "1;2H\x7f"
	              "b",
	              0, "a1;2Hb"),
	      "and neither C1 controls nor DEL");
	CHECK(filters("ñ é 日本", 0, "ñ é 日本"), "letters of any script stay");
	CHECK(filters("a\xff\xc3"
	              "b",
	              0, "ab"),
	      "broken UTF-8 goes");
	CHECK(filters("ls", 1, "\x1b[200~ls\x1b[201~"),
	      "a protected paste is wrapped");
	CHECK(filters("x\x1b[201~; rm -rf ~\n", 1,
	              "\x1b[200~x[201~; rm -rf ~\r\x1b[201~"),
	      "a pasted ESC [ 201 ~ does not end the protected paste");

	CHECK(decodes("aG9sYQ==", "hola"), "base64 with padding");
	CHECK(decodes("aG9sYSE", "hola!"), "and without");
	CHECK(decodes("", ""), "empty is empty");
	CHECK(decodes("aG9s*YQ", NULL), "a letter that is not base64 is refused");
	return test_summary();
}
