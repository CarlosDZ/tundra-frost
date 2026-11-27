#include "config.c"
#include "test.h"

static struct config c;
static char err[CONFIG_ERR_MAX];

static int parses(const char *text) {
	return config_parse(text, strlen(text), &c, err) == 0;
}

int main(void) {
	config_default(&c);
	CHECK(c.size == 24 && c.history == 32 && c.cursor == CURSOR_BAR && c.blink,
	      "the defaults of ADR-042: 24, 32 MB, a line, blinking");

	CHECK(parses("size = 18\nhistory = 64\ncursor = block\nblink = no\n") &&
	          c.size == 18 && c.history == 64 && c.cursor == CURSOR_BLOCK &&
	          !c.blink,
	      "every key takes its value");

	CHECK(parses("cursor = underline\n") && c.cursor == CURSOR_UNDERLINE &&
	          c.size == 18 && !c.blink,
	      "a second file changes only what it says");

	struct config before = c;
	CHECK(!parses("size = 18\ncolour = red\n") &&
	          !memcmp(&before, &c, sizeof(c)) && strstr(err, "line 2"),
	      "an unknown key fails the whole file, on its line");
	CHECK(!parses("size = 200\n") && !parses("history = 0\n") &&
	          !parses("cursor = bar\n") && !parses("blink = maybe\n") &&
	          !parses("size = 12px\n") && !memcmp(&before, &c, sizeof(c)),
	      "a value out of range or not known fails, and changes nothing");
	CHECK(!parses("window {\n\tsize = 12\n}\n"), "blocks are not for frost");
	return test_summary();
}
