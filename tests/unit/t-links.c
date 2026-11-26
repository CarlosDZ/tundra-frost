#include "history.c"
#include "links.c"
#include "render.c"
#include "screen.c"
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

static int bare(int pos, int from, int to, const char *want) {
	int f, t, id;
	char url[LINK_MAX];
	int k = link_at(&v, pos, &f, &t, &id, url);
	int ok = k == LINK_BARE && f == from && t == to && !strcmp(url, want);
	if (!ok && k == LINK_BARE)
		printf("# got %d..%d %s\n", f, t, url);
	return ok;
}

static int none(int pos) {
	int f, t, id;
	char url[LINK_MAX];
	return link_at(&v, pos, &f, &t, &id, url) == LINK_NONE;
}

int main(void) {
	start(40, 2, "ver https://tundra.example/a?b=1.");
	CHECK(bare(10, 4, 32, "https://tundra.example/a?b=1"),
	      "an address written in the line, without the full stop after it");
	CHECK(none(2), "the words around it are not");
	start(40, 2, "(https://x.example/a_(b)) y");
	CHECK(bare(5, 1, 24, "https://x.example/a_(b)"),
	      "its own brackets stay, the one around it goes");
	start(40, 2, "<http://x.example/p>, mailto:a@b.example");
	CHECK(bare(3, 1, 19, "http://x.example/p"), "it ends where < > close it");
	CHECK(bare(25, 22, 40, "mailto:a@b.example"), "mailto has no slashes");
	start(40, 2, "xhttp://x.example ftp:/nada javascript:x");
	CHECK(none(4) && none(20) && none(30),
	      "not glued to a word, not without its slashes, not other schemes");
	start(10, 3, "a https://w.example/xyz");
	CHECK(bare(13, 2, 23, "https://w.example/xyz"),
	      "an address that wraps is found from any of its rows");
	start(20, 1, "\x1b]8;;https://t.example/o\x1b\\enlace\x1b]8;;\x1b\\ y");
	int f, t, id;
	char url[LINK_MAX];
	CHECK(link_at(&v, 3, &f, &t, &id, url) == LINK_OSC8 && f == 0 && t == 6 &&
	          id == 1,
	      "an OSC 8 link: its cells and its number");
	CHECK(none(8), "and the text after it is not");
	CHECK(link_allowed("https://a") && link_allowed("HTTP://a") &&
	          link_allowed("file:///etc") && link_allowed("mailto:x"),
	      "http, https, file and mailto are opened");
	CHECK(!link_allowed("javascript:alert(1)") && !link_allowed("ssh://h") &&
	          !link_allowed("data:text/html,x"),
	      "other schemes are not");
	scr_free(&s);
	return test_summary();
}
