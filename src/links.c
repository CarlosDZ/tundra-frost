#include "links.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static const struct cell *at(const struct view *v, int i) {
	return v->cells + (size_t)v->map[i / v->cols] * v->cols + i % v->cols;
}

static int wrapped(const struct view *v, int y) {
	return v->lines && (v->lines[y] & LINE_WRAPPED);
}

static const char *const schemes[] = {"https://", "http://", "ftp://",
                                      "file://", "mailto:"};

int link_allowed(const char *url) {
	for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++)
		if (!strncasecmp(url, schemes[i], strlen(schemes[i])))
			return 1;
	return 0;
}

static int in_url(unsigned cp) {
	return cp >= 0x80 ||
	       (cp > 0x20 && cp < 0x7F && !strchr("\"<>`{}|\\^", (int)cp));
}

static size_t put(char *p, unsigned cp) {
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

static int address(const unsigned *cp, int n, int s, int *end) {
	for (size_t k = 0; k < sizeof(schemes) / sizeof(schemes[0]); k++) {
		int len = (int)strlen(schemes[k]), i = 0;
		while (i < len && s + i < n &&
		       tolower((int)(cp[s + i] < 0x80 ? cp[s + i] : 0)) ==
		           schemes[k][i])
			i++;
		if (i < len || s + len >= n || !in_url(cp[s + len]))
			continue;
		int e = s + len;
		while (e < n && in_url(cp[e]))
			e++;
		for (;;) {
			int open = 0;
			for (int j = s; j < e - 1; j++)
				open += cp[j] == '(' ? 1 : cp[j] == ')' ? -1 : 0;
			if (e > s + len && strchr(".,;:!?'", (int)cp[e - 1]))
				e--;
			else if (e > s + len && cp[e - 1] == ')' && open <= 0)
				e--;
			else if (e > s + len && cp[e - 1] == ']')
				e--;
			else
				break;
		}
		*end = e;
		return 1;
	}
	return 0;
}

int link_at(const struct view *v, int pos, int *from, int *to, int *id,
            char *url) {
	int last = v->cols * v->rows;
	if (pos < 0 || pos >= last)
		return LINK_NONE;
	int y0 = pos / v->cols, y1 = y0;
	while (y0 > 0 && wrapped(v, y0 - 1))
		y0--;
	while (y1 < v->rows - 1 && wrapped(v, y1))
		y1++;
	int lo = y0 * v->cols, hi = (y1 + 1) * v->cols;

	int l = at(v, pos)->link;
	if (l) {
		int a = pos, b = pos + 1;
		while (a > lo && at(v, a - 1)->link == l)
			a--;
		while (b < hi && at(v, b)->link == l)
			b++;
		*from = a, *to = b, *id = l;
		return LINK_OSC8;
	}

	static unsigned cp[1 << 16];
	static int cell[1 << 16];
	int n = 0;
	for (int i = lo; i < hi && n < (int)(sizeof(cp) / sizeof(cp[0])); i++) {
		const struct cell *c = at(v, i);
		if (c->attr & ATTR_WIDE_TAIL)
			continue;
		cp[n] = c->cp ? c->cp : ' ';
		cell[n++] = i;
	}
	for (int s = 0; s < n; s++) {
		int e;
		if ((s > 0 && isalnum((int)(cp[s - 1] < 0x80 ? cp[s - 1] : 'a'))) ||
		    !address(cp, n, s, &e))
			continue;
		int last_cell = cell[e - 1];
		if (pos >= cell[s] &&
		    pos <= last_cell + ((at(v, last_cell)->attr & ATTR_WIDE) ? 1 : 0)) {
			size_t k = 0;
			for (int j = s; j < e && k + 4 < LINK_MAX; j++)
				k += put(url + k, cp[j]);
			url[k] = 0;
			*from = cell[s];
			*to = last_cell + ((at(v, last_cell)->attr & ATTR_WIDE) ? 2 : 1);
			*id = 0;
			return LINK_BARE;
		}
		s = e - 1;
	}
	return LINK_NONE;
}
