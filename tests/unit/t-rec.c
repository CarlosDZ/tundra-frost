#include "history.c"
#include "screen.c"
#include "test.h"
#include "vt.c"
#include "width.c"

#include "dump.h"

#include <stdlib.h>

static char *slurp(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	char *buf = malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf)
		buf[n] = 0, *len = (size_t)n;
	return buf;
}

static char *replay(const char *rec, size_t len, int cols, int rows) {
	static struct scr s;
	scr_init(&s, cols, rows);
	int was_alt = 0;
	for (size_t i = 0; i < len; i++) {
		scr_feed(&s, (const uint8_t *)rec + i, 1);
		s.reply_len = 0;
		if (was_alt && !s.alt) {
			s.alt = 1;
			break;
		}
		if (s.alt)
			was_alt = 1;
	}
	char *out;
	size_t size;
	FILE *f = open_memstream(&out, &size);
	if (was_alt)
		fprintf(f, "-- the alternate grid as it was left\n");
	dump(f, &s);
	fclose(f);
	scr_free(&s);
	return out;
}

static void check(const char *name, int cols, int rows) {
	char path[256], desc[300];
	size_t rlen = 0, wlen = 0;
	snprintf(path, sizeof(path), "tests/rec/%s.out", name);
	char *rec = slurp(path, &rlen);
	snprintf(path, sizeof(path), "tests/rec/%s.txt", name);
	char *want = slurp(path, &wlen);
	char *got = rec ? replay(rec, rlen, cols, rows) : NULL;
	snprintf(desc, sizeof(desc), "%s gives the reviewed screen", name);
	CHECK(rec && want && got && strcmp(got, want) == 0, desc);
	free(rec);
	free(want);
	free(got);
}

int main(void) {
	check("nvim-120x36", 120, 36);
	check("nvim-80x24", 80, 24);
	check("less-120x36", 120, 36);
	check("man-120x36", 120, 36);
	check("htop-120x36", 120, 36);
	check("gitdiff-120x36", 120, 36);
	check("gitstatus-120x36", 120, 36);
	check("ls-120x36", 120, 36);
	check("styles-120x36", 120, 36);
	return test_summary();
}
