#include "select.h"

#include <stdlib.h>
#include <string.h>

static const struct cell *at(const struct view *v, int i) {
	return v->cells + (size_t)v->map[i / v->cols] * v->cols + i % v->cols;
}

static int wrapped(const struct view *v, int y) {
	return v->lines && (v->lines[y] & LINE_WRAPPED);
}

static int in_word(const struct view *v, int i) {
	const struct cell *c = at(v, i);
	if (c->attr & ATTR_WIDE_TAIL)
		return 1;
	return c->cp && c->cp != ' ' && !strchr(" \t\"'`()[]{}<>|;,", (int)c->cp);
}

static int joined(const struct view *v, int i, int j) {
	return i / v->cols == j / v->cols || wrapped(v, (i < j ? i : j) / v->cols);
}

void select_span(const struct view *v, int a, int b, int mode, int *from,
                 int *to) {
	int last = v->cols * v->rows - 1;
	int lo = a < b ? a : b, hi = a < b ? b : a;
	lo = lo < 0 ? 0 : lo > last ? last : lo;
	hi = hi < 0 ? 0 : hi > last ? last : hi;
	if (mode == SELECT_LINES) {
		int y0 = lo / v->cols, y1 = hi / v->cols;
		while (y0 > 0 && wrapped(v, y0 - 1))
			y0--;
		while (y1 < v->rows - 1 && wrapped(v, y1))
			y1++;
		*from = y0 * v->cols, *to = (y1 + 1) * v->cols;
		return;
	}
	if (mode == SELECT_WORDS) {
		if (in_word(v, lo))
			while (lo > 0 && in_word(v, lo - 1) && joined(v, lo - 1, lo))
				lo--;
		if (in_word(v, hi))
			while (hi < last && in_word(v, hi + 1) && joined(v, hi, hi + 1))
				hi++;
	}
	if ((at(v, lo)->attr & ATTR_WIDE_TAIL) && lo > 0)
		lo--;
	if ((at(v, hi)->attr & ATTR_WIDE) && hi < last)
		hi++;
	*from = lo, *to = hi + 1;
}

static size_t text_utf8(char *p, uint32_t cp) {
	if (cp < 0x80) {
		p[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		p[0] = (char)(0xC0 | cp >> 6);
		p[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		p[0] = (char)(0xE0 | cp >> 12);
		p[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		p[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	p[0] = (char)(0xF0 | cp >> 18);
	p[1] = (char)(0x80 | (cp >> 12 & 0x3F));
	p[2] = (char)(0x80 | (cp >> 6 & 0x3F));
	p[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

char *select_text(const struct view *v, int from, int to, size_t *len) {
	*len = 0;
	if (to <= from)
		return NULL;
	char *out = malloc((size_t)(to - from) * 12 + (size_t)v->rows + 1);
	if (!out)
		return NULL;
	size_t n = 0;
	for (int y = from / v->cols; y <= (to - 1) / v->cols; y++) {
		int x0 = y == from / v->cols ? from % v->cols : 0;
		int x1 = y == (to - 1) / v->cols ? (to - 1) % v->cols + 1 : v->cols;
		int goes_on = wrapped(v, y) && (y + 1) * v->cols < to;
		size_t keep = n;
		for (int x = x0; x < x1; x++) {
			const struct cell *c = at(v, y * v->cols + x);
			if (c->attr & ATTR_WIDE_TAIL)
				continue;
			n += text_utf8(out + n, c->cp ? c->cp : ' ');
			for (int i = 0; i < CELL_COMB && c->comb[i]; i++)
				n += text_utf8(out + n, c->comb[i]);
			if (c->cp && c->cp != ' ')
				keep = n;
		}
		if (!goes_on) {
			n = keep;
			if (y < (to - 1) / v->cols)
				out[n++] = '\n';
		}
	}
	*len = n;
	return out;
}
