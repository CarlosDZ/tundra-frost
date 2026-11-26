#include "test.h"
#include "tmuxcmd.c"

#include <stdio.h>

static int parses(const char *line, struct tmux_cmd *c) {
	static char buf[512];
	static char *argv[32];
	int argc = 0;
	snprintf(buf, sizeof(buf), "%s", line);
	for (char *t = strtok(buf, " "); t && argc < 32; t = strtok(NULL, " "))
		argv[argc++] = t;
	return tmux_parse(argc, argv, c) == 0;
}

static const char *vars(void *ctx, const char *name) {
	(void)ctx;
	if (!strcmp(name, "pane_id"))
		return "%5";
	if (!strcmp(name, "window_id"))
		return "@2";
	if (!strcmp(name, "pane_title"))
		return "hola-1";
	return NULL;
}

static char pieces[256];

static void piece(void *ctx, const char *text, size_t len,
                  const struct tmux_style *st) {
	(void)ctx;
	char one[64];
	snprintf(one, sizeof(one), "[%u%s]%.*s", st->fg, st->bold ? "b" : "",
	         (int)len, text);
	strncat(pieces, one, sizeof(pieces) - strlen(pieces) - 1);
}

int main(void) {
	struct tmux_cmd c;
	CHECK(
	    parses("split-window -d -t %4 -h -l 70% -P -F #{pane_id} -- cat", &c) &&
	        !strcmp(c.name, "split-window") && tmux_flag(&c, 'd') &&
	        tmux_flag(&c, 'h') && !tmux_flag(&c, 'v') &&
	        !strcmp(tmux_value(&c, 't'), "%4") &&
	        !strcmp(tmux_value(&c, 'l'), "70%") &&
	        !strcmp(tmux_value(&c, 'F'), "#{pane_id}") && c.argc == 1 &&
	        !strcmp(c.argv[0], "cat"),
	    "split-window as Claude Code writes it");
	CHECK(parses("splitw -dh -t%4", &c) && !strcmp(c.name, "split-window") &&
	          tmux_flag(&c, 'd') && tmux_flag(&c, 'h') &&
	          !strcmp(tmux_value(&c, 't'), "%4"),
	      "an alias, joined options and a value glued to its letter");
	CHECK(parses("set-option -p -t %5 pane-border-style fg=blue", &c) &&
	          tmux_flag(&c, 'p') && c.argc == 2 &&
	          !strcmp(c.argv[0], "pane-border-style") &&
	          !strcmp(c.argv[1], "fg=blue"),
	      "set-option: the option and its value after the options");
	CHECK(parses("respawn-pane -k -t %5 -- cd /x && claude", &c) &&
	          tmux_flag(&c, 'k') && c.argc == 4,
	      "respawn-pane: the command after --");
	CHECK(!parses("split-window -Q", &c) && !parses("frobnicate", &c) &&
	          !parses("kill-pane -t", &c),
	      "an unknown order or option, or an option without its value, fails");

	char out[128];
	tmux_format("#{pane_id} #{window_id}", vars, NULL, out, sizeof(out));
	CHECK(!strcmp(out, "%5 @2"), "#{name} is its value");
	tmux_format("#{nothing}x ## #[fg=red]y", vars, NULL, out, sizeof(out));
	CHECK(!strcmp(out, "x # y"),
	      "an unknown name is empty, ## a #, and a style is left out");

	struct tmux_style base = {COLOR_DEFAULT, 0}, st = base;
	tmux_style("fg=blue,bold", &base, &st);
	CHECK(st.fg == COLOR_INDEX(4) && st.bold, "fg=blue,bold");
	tmux_style("fg=colour208", &base, &st);
	CHECK(st.fg == COLOR_INDEX(208), "a colour of the 256");
	tmux_style("fg=#ff8000", &base, &st);
	CHECK(st.fg == COLOR_RGB(255, 128, 0), "and one of 24 bits");
	tmux_style("default", &base, &st);
	CHECK(st.fg == COLOR_DEFAULT && !st.bold, "default goes back");

	tmux_format_styled("#[fg=blue,bold] #{pane_title} #[default]", &base, vars,
	                   NULL, piece, NULL);
	char want[128];
	snprintf(want, sizeof(want), "[%ub] [%ub]hola-1[%ub] ", COLOR_INDEX(4),
	         COLOR_INDEX(4), COLOR_INDEX(4));
	CHECK(!strcmp(pieces, want),
	      "a border format comes in pieces with their style");
	return test_summary();
}
