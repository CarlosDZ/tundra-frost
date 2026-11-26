#ifndef FROST_LINE_H
#define FROST_LINE_H

#include <stdint.h>

#include "screen.h"

#define LINE_INPUT_MAX 256
#define LINE_LABEL_MAX 20

enum line_cmd {
	CMD_NONE,
	CMD_GOTO,
	CMD_NEW,
	CMD_LS,
	CMD_SIZE,
	CMD_ATTACH,
	CMD_DETACH,
	CMD_BAD,
};

struct line_command {
	enum line_cmd cmd;
	int number;
	char arg[64];
};

void line_parse(const char *text, struct line_command *out);

struct line_tab {
	const char *label;
	int current, marked;
};

struct line_colors {
	uint32_t bg, text, dim, alert;
};

void line_cells(struct cell *row, int cols, const struct line_tab *tabs, int n,
                int open, const char *input, const char *message, int error,
                const struct line_colors *c, int *cursor_x);

#endif
