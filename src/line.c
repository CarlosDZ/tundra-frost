#include "line.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "width.h"

static int number(const char *s) {
	char *end;
	long v = strtol(s, &end, 10);
	return *s && !*end && v > 0 && v < 100000 ? (int)v : -1;
}

void line_parse(const char *text, struct line_command *out) {
	char word[16] = "", rest[64] = "";
	*out = (struct line_command){CMD_NONE, 0, ""};
	while (isspace((unsigned char)*text))
		text++;
	if (!*text)
		return;
	int k = 0;
	while (text[k] && !isspace((unsigned char)text[k]) &&
	       k < (int)sizeof(word) - 1) {
		word[k] = text[k];
		k++;
	}
	if (text[k] && !isspace((unsigned char)text[k])) {
		out->cmd = CMD_BAD;
		return;
	}
	const char *r = text + k;
	while (isspace((unsigned char)*r))
		r++;
	snprintf(rest, sizeof(rest), "%s", r);
	for (int i = (int)strlen(rest) - 1;
	     i >= 0 && isspace((unsigned char)rest[i]); i--)
		rest[i] = 0;

	out->cmd = CMD_BAD;
	if (number(word) > 0 && !*rest)
		out->cmd = CMD_GOTO, out->number = number(word);
	else if (!strcmp(word, "goto") && number(rest) > 0)
		out->cmd = CMD_GOTO, out->number = number(rest);
	else if (!strcmp(word, "new") && !*rest)
		out->cmd = CMD_NEW;
	else if (!strcmp(word, "ls") && !*rest)
		out->cmd = CMD_LS;
	else if (!strcmp(word, "size") && number(rest) > 0)
		out->cmd = CMD_SIZE, out->number = number(rest);
	else if (!strcmp(word, "attach") && *rest && !strchr(rest, ' ')) {
		out->cmd = CMD_ATTACH, out->number = number(rest);
		snprintf(out->arg, sizeof(out->arg), "%s", rest);
	} else if (!strcmp(word, "detach") && !strchr(rest, ' ')) {
		out->cmd = CMD_DETACH;
		snprintf(out->arg, sizeof(out->arg), "%s", rest);
	}
}

static int put(struct cell *row, int cols, int x, const char **s, uint32_t fg,
               uint32_t bg, int max) {
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t cp = *p;
	int len = *p < 0x80 ? 1 : *p >> 5 == 6 ? 2 : *p >> 4 == 14 ? 3 : 4;
	if (len > 1) {
		cp = *p & (0x7F >> len);
		for (int i = 1; i < len; i++) {
			if ((p[i] & 0xC0) != 0x80) {
				len = i;
				cp = 0xFFFD;
				break;
			}
			cp = cp << 6 | (p[i] & 0x3F);
		}
	}
	*s += len;
	int w = width_of(cp);
	if (w <= 0 || cp < 0x20)
		return x;
	if (x + w > max || x + w > cols)
		return -1;
	if (!row)
		return x + w;
	row[x] = (struct cell){cp, {0, 0}, fg, bg, 0, 0, 0};
	if (w == 2) {
		row[x].attr = ATTR_WIDE;
		row[x + 1] = (struct cell){0, {0, 0}, fg, bg, 0, ATTR_WIDE_TAIL, 0};
	}
	return x + w;
}

static int text(struct cell *row, int cols, int x, const char *s, uint32_t fg,
                uint32_t bg, int max) {
	while (*s && x >= 0) {
		int nx = put(row, cols, x, &s, fg, bg, max);
		if (nx < 0)
			return x;
		x = nx;
	}
	return x;
}

static int draw_tabs(struct cell *row, int cols, int x,
                     const struct line_tab *tabs, int n,
                     const struct line_colors *c) {
	for (int i = 0; i < n && x < cols; i++) {
		uint32_t fg = tabs[i].marked    ? c->alert
		              : tabs[i].current ? c->text
		                                : c->dim;
		char num[16];
		snprintf(num, sizeof(num), "%d ", i + 1);
		x = text(row, cols, x, num, fg, c->bg, cols);
		int start = x;
		x = text(row, cols, x, tabs[i].label, fg, c->bg,
		         start + LINE_LABEL_MAX);
		if (tabs[i].current)
			x = text(row, cols, x, "*", fg, c->bg, cols);
		if (tabs[i].marked)
			x = text(row, cols, x, "!", fg, c->bg, cols);
		x += 2;
	}
	return x;
}

void line_cells(struct cell *row, int cols, const struct line_tab *tabs, int n,
                int open, const char *input, const char *message, int error,
                const struct line_colors *c, int *cursor_x) {
	for (int x = 0; x < cols; x++)
		row[x] = (struct cell){' ', {0, 0}, c->text, c->bg, 0, 0, 0};
	*cursor_x = -1;
	int x = 1;
	if (open) {
		x = text(row, cols, 0, ":", c->dim, c->bg, cols);
		x = text(row, cols, x, input, c->text, c->bg, cols);
		*cursor_x = x < cols ? x : cols - 1;
		x += 2;
	}
	if (message && *message) {
		text(row, cols, x, message, error ? c->alert : c->dim, c->bg, cols);
		return;
	}
	if (open) {
		int need = draw_tabs(NULL, 1 << 20, 0, tabs, n, c) - 2;
		if (cols - need > x)
			x = cols - need;
	}
	draw_tabs(row, cols, x, tabs, n, c);
}
