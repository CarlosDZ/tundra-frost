#include "line.c"
#include "test.h"
#include "width.c"

static int parses(const char *in, enum line_cmd cmd, int number,
                  const char *arg) {
	struct line_command c;
	line_parse(in, &c);
	return c.cmd == cmd && c.number == number && !strcmp(c.arg, arg);
}

static const struct line_colors colors = {
    COLOR_RGB(1, 1, 1), COLOR_RGB(2, 2, 2), COLOR_RGB(3, 3, 3),
    COLOR_RGB(4, 4, 4)};

static const char *letters(const struct cell *row, int cols) {
	static char out[512];
	int n = 0, end = 0;
	for (int x = 0; x < cols; x++) {
		if (row[x].attr & ATTR_WIDE_TAIL)
			continue;
		uint32_t cp = row[x].cp ? row[x].cp : ' ';
		out[n++] = (char)(cp < 0x80 ? cp : '?');
		if (cp != ' ')
			end = n;
	}
	out[end] = 0;
	return out;
}

int main(void) {
	CHECK(parses("2", CMD_GOTO, 2, ""), "a number goes to that tab");
	CHECK(parses("  goto 3 ", CMD_GOTO, 3, ""), "and so does goto");
	CHECK(parses("new", CMD_NEW, 0, ""), "new");
	CHECK(parses("ls", CMD_LS, 0, ""), "ls");
	CHECK(parses("size 28", CMD_SIZE, 28, ""), "size and a number");
	CHECK(parses("attach 3", CMD_ATTACH, 3, "3"), "attach by number");
	CHECK(parses("attach build", CMD_ATTACH, -1, "build"), "or by name");
	CHECK(parses("detach", CMD_DETACH, 0, ""), "detach");
	CHECK(parses("detach build", CMD_DETACH, 0, "build"), "detach with a name");
	CHECK(parses("", CMD_NONE, 0, "") && parses("   ", CMD_NONE, 0, ""),
	      "nothing is nothing");
	CHECK(parses("goto", CMD_BAD, 0, "") && parses("size x", CMD_BAD, 0, "") &&
	          parses("0", CMD_BAD, 0, "") && parses("rm -rf", CMD_BAD, 0, "") &&
	          parses("attach a b", CMD_BAD, 0, ""),
	      "the rest is a mistake");

	struct cell row[40];
	int cx;
	struct line_tab tabs[] = {{"shell", 0, 0}, {"nvim", 1, 0}, {"build", 0, 1}};
	line_cells(row, 40, tabs, 3, 0, "", "", 0, &colors, &cx);
	CHECK(!strcmp(letters(row, 40), " 1 shell  2 nvim*  3 build!") && cx < 0,
	      "closed: each tab with its number, the current one with *, a marked "
	      "one with !");
	CHECK(row[1].fg == colors.dim && row[10].fg == colors.text &&
	          row[19].fg == colors.alert && row[0].bg == colors.bg,
	      "the current tab in the text colour, the rest dim, a marked one red");
	line_cells(row, 40, tabs, 3, 1, "goto 2", "", 0, &colors, &cx);
	CHECK(!strcmp(letters(row, 40), ":goto 2   1 shell  2 nvim*  3 build!") ||
	          strstr(letters(row, 40), ":goto 2") == letters(row, 40),
	      "open: the prompt and what is typed");
	CHECK(cx == 7, "the cursor after what is typed");
	CHECK(row[39].cp == '!', "and the tabs to the right");
	line_cells(row, 40, tabs, 3, 1, "", "no tab 9", 1, &colors, &cx);
	CHECK(!strcmp(letters(row, 40), ":  no tab 9") && row[3].fg == colors.alert,
	      "a mistake shows instead of the tabs, in red");
	struct line_tab longer[] = {{"a very long title of a program", 1, 0}};
	line_cells(row, 40, longer, 1, 0, "", "", 0, &colors, &cx);
	CHECK(!strcmp(letters(row, 40), " 1 a very long title of*"),
	      "a long label is cut");
	return test_summary();
}
