#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tundra/conf.h>

static struct tconf parsed;

void config_default(struct config *c) {
	*c = (struct config){24, 32, CURSOR_BAR, 1};
}

static int bounded(const char *v, int lo, int hi, int *out) {
	char *end;
	long n = strtol(v, &end, 10);
	if (end == v || *end || n < lo || n > hi)
		return -1;
	*out = (int)n;
	return 0;
}

static int fail(char *err, int line, const char *what, const char *arg) {
	snprintf(err, CONFIG_ERR_MAX, "line %d: %s %s", line, what, arg);
	return -1;
}

int config_parse(const char *text, size_t len, struct config *c, char *err) {
	struct config next = *c;
	if (tconf_parse(text, len, &parsed) < 0) {
		snprintf(err, CONFIG_ERR_MAX, "line %d: %s", parsed.err_line,
		         parsed.err);
		return -1;
	}
	if (parsed.nblocks > 0)
		return fail(err, parsed.blocks[0].line, "no blocks here, found",
		            parsed.blocks[0].name);
	for (int i = 0; i < parsed.n; i++) {
		const struct tconf_item *it = &parsed.items[i];
		const char *v = it->value;
		int bad = 0;
		if (!strcmp(it->key, "size"))
			bad = bounded(v, 6, 128, &next.size);
		else if (!strcmp(it->key, "history"))
			bad = bounded(v, 1, 1024, &next.history);
		else if (!strcmp(it->key, "cursor"))
			bad = !strcmp(v, "line")    ? (next.cursor = CURSOR_BAR, 0)
			      : !strcmp(v, "block") ? (next.cursor = CURSOR_BLOCK, 0)
			      : !strcmp(v, "underline")
			          ? (next.cursor = CURSOR_UNDERLINE, 0)
			          : -1;
		else if (!strcmp(it->key, "blink"))
			bad = !strcmp(v, "yes")  ? (next.blink = 1, 0)
			      : !strcmp(v, "no") ? (next.blink = 0, 0)
			                         : -1;
		else
			return fail(err, it->line, "unknown key", it->key);
		if (bad)
			return fail(err, it->line, "bad value for", it->key);
	}
	*c = next;
	return 0;
}
