#include "history.h"
#include "width.h"

#include <stdlib.h>
#include <string.h>

#define STYLE_MARK 0xFF
#define STYLE_LEN 16
#define LINE_MARK 0xFE
#define EMPTY_MARK 0x00
#define CELL_BYTES_MAX (1 + STYLE_LEN + 4 * (1 + CELL_COMB))

static const struct hist_style plain;

int hist_init(struct hist *h, size_t limit) {
	memset(h, 0, sizeof(*h));
	h->limit = limit;
	size_t most = limit / (64 + sizeof(struct hist_line));
	if (most < 4)
		return -1;
	h->lmax = 1;
	while (h->lmax * 2 <= most)
		h->lmax *= 2;
	h->cap = limit - h->lmax * sizeof(struct hist_line);
	return 0;
}

void hist_free(struct hist *h) {
	free(h->buf);
	free(h->lines);
	free(h->scratch);
	memset(h, 0, sizeof(*h));
}

void hist_clear(struct hist *h) {
	h->first = h->n = 0;
	h->wr = 0;
	h->open = 0;
}

void hist_seal(struct hist *h) { h->open = 0; }

size_t hist_count(const struct hist *h) { return h->n; }

size_t hist_bytes(const struct hist *h) {
	return (h->buf ? h->cap : 0) + h->lcap * sizeof(struct hist_line);
}

static struct hist_line *line_at(const struct hist *h, size_t i) {
	return &h->lines[(h->first + i) & (h->lcap - 1)];
}

static void drop_oldest(struct hist *h) {
	h->first = (h->first + 1) & (h->lcap - 1);
	if (--h->n == 0) {
		h->wr = 0;
		h->open = 0;
	}
}

static long reserve(struct hist *h, size_t len, size_t keep) {
	if (len > h->cap)
		return -1;
	for (;;) {
		if (h->n == 0) {
			h->wr = 0;
			return 0;
		}
		size_t rd = line_at(h, 0)->off;
		if (h->wr > rd) {
			if (h->cap - h->wr >= len)
				return (long)h->wr;
			if (rd >= len)
				return 0;
		} else if (rd - h->wr >= len) {
			return (long)h->wr;
		}
		if (h->n <= keep)
			return -1;
		drop_oldest(h);
	}
}

static int grow_index(struct hist *h) {
	if (h->n < h->lcap)
		return 0;
	if (h->lcap >= h->lmax) {
		drop_oldest(h);
		return 0;
	}
	size_t cap = h->lcap ? h->lcap * 2 : 64;
	if (cap > h->lmax)
		cap = h->lmax;
	if (cap <= h->lcap)
		return -1;
	struct hist_line *l = malloc(cap * sizeof(*l));
	if (!l)
		return -1;
	for (size_t i = 0; i < h->n; i++)
		l[i] = *line_at(h, i);
	free(h->lines);
	h->lines = l;
	h->lcap = cap;
	h->first = 0;
	return 0;
}

static size_t put_utf8(uint8_t *p, uint32_t cp) {
	if (cp < 0x80) {
		p[0] = (uint8_t)cp;
		return 1;
	}
	if (cp < 0x800) {
		p[0] = (uint8_t)(0xC0 | cp >> 6);
		p[1] = (uint8_t)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		p[0] = (uint8_t)(0xE0 | cp >> 12);
		p[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
		p[2] = (uint8_t)(0x80 | (cp & 0x3F));
		return 3;
	}
	p[0] = (uint8_t)(0xF0 | cp >> 18);
	p[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
	p[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
	p[3] = (uint8_t)(0x80 | (cp & 0x3F));
	return 4;
}

static size_t get_utf8(const uint8_t *p, size_t n, uint32_t *cp) {
	int need = p[0] >= 0xF0 ? 3 : p[0] >= 0xE0 ? 2 : p[0] >= 0xC0 ? 1 : 0;
	if ((size_t)need >= n) {
		*cp = 0xFFFD;
		return n;
	}
	uint32_t v = need == 3   ? p[0] & 0x07u
	             : need == 2 ? p[0] & 0x0Fu
	             : need == 1 ? p[0] & 0x1Fu
	                         : p[0];
	for (int i = 1; i <= need; i++)
		v = v << 6 | (p[i] & 0x3Fu);
	*cp = v;
	return (size_t)need + 1;
}

static void style_of(const struct cell *c, struct hist_style *s) {
	s->fg = c->fg, s->bg = c->bg, s->ul = c->ul, s->link = c->link;
	s->attr = c->attr & ~ATTR_LAYOUT;
}

static int same_style(const struct hist_style *a, const struct hist_style *b) {
	return a->fg == b->fg && a->bg == b->bg && a->ul == b->ul &&
	       a->attr == b->attr && a->link == b->link;
}

static void put_style(uint8_t *p, const struct hist_style *s) {
	memcpy(p, &s->fg, 4);
	memcpy(p + 4, &s->bg, 4);
	memcpy(p + 8, &s->ul, 4);
	memcpy(p + 12, &s->attr, 2);
	memcpy(p + 14, &s->link, 2);
}

static void get_style(const uint8_t *p, struct hist_style *s) {
	memcpy(&s->fg, p, 4);
	memcpy(&s->bg, p + 4, 4);
	memcpy(&s->ul, p + 8, 4);
	memcpy(&s->attr, p + 12, 2);
	memcpy(&s->link, p + 14, 2);
}

static size_t encode(const struct cell *cells, int n, struct hist_style *st,
                     uint8_t *out) {
	size_t k = 0;
	for (int x = 0; x < n; x++) {
		const struct cell *c = &cells[x];
		if (c->attr & (ATTR_WIDE_TAIL | ATTR_PAD))
			continue;
		struct hist_style s;
		style_of(c, &s);
		if (!same_style(&s, st)) {
			out[k++] = STYLE_MARK;
			put_style(out + k, &s);
			k += STYLE_LEN;
			*st = s;
		}
		if (!c->cp) {
			out[k++] = EMPTY_MARK;
			continue;
		}
		k += put_utf8(out + k, c->cp);
		for (int i = 0; i < CELL_COMB && c->comb[i]; i++)
			k += put_utf8(out + k, c->comb[i]);
	}
	return k;
}

static int empty_plain(const struct cell *c) {
	struct hist_style s;
	style_of(c, &s);
	return !c->cp && same_style(&s, &plain);
}

static int extend(struct hist *h, size_t len) {
	for (;;) {
		struct hist_line *l = line_at(h, h->n - 1);
		size_t end = l->off + l->len, rd = line_at(h, 0)->off;
		int ahead = h->n == 1 || end > rd;
		if ((ahead ? h->cap - end : rd - end) >= len)
			return 0;
		if (ahead)
			break;
		drop_oldest(h);
	}
	struct hist_line *l = line_at(h, h->n - 1);
	size_t whole = l->len + len;
	while (h->n > 1 && line_at(h, 0)->off < whole)
		drop_oldest(h);
	l = line_at(h, h->n - 1);
	if (whole > h->cap || (h->n == 1 ? l->off : line_at(h, 0)->off) < whole)
		return -1;
	memcpy(h->buf, h->buf + l->off, l->len);
	l->off = 0;
	h->wr = l->len;
	return 0;
}

int hist_push(struct hist *h, const struct cell *cells, int cols,
              uint8_t flags) {
	int n = cols, wrapped = flags & LINE_WRAPPED;
	if (!wrapped)
		while (n > 0 && empty_plain(&cells[n - 1]))
			n--;
	int pads = 0;
	for (int x = 0; x < n; x++)
		pads += (cells[x].attr & ATTR_PAD) != 0;
	size_t need = 1 + (size_t)n * CELL_BYTES_MAX;
	if (need > h->scratch_size) {
		uint8_t *p = realloc(h->scratch, need);
		if (!p)
			return -1;
		h->scratch = p;
		h->scratch_size = need;
	}
	if (!h->buf && !(h->buf = malloc(h->cap)))
		return -1;

	if (h->open && h->n) {
		struct hist_style st = h->open_style;
		size_t len = encode(cells, n, &st, h->scratch);
		struct hist_line *l = line_at(h, h->n - 1);
		if (l->len + len <= HIST_LINE_MAX && extend(h, len) == 0) {
			l = line_at(h, h->n - 1);
			memcpy(h->buf + l->off + l->len, h->scratch, len);
			l->len += (uint32_t)len;
			l->cells += (uint32_t)(n - pads);
			l->rows_cols = 0;
			h->wr = l->off + l->len;
			h->open = wrapped;
			h->open_style = st;
			return 0;
		}
	}

	struct hist_style st = plain;
	h->scratch[0] = LINE_MARK;
	size_t len = 1 + encode(cells, n, &st, h->scratch + 1);
	if (grow_index(h) < 0)
		return -1;
	long at = reserve(h, len, 0);
	if (at < 0)
		return -1;
	memcpy(h->buf + at, h->scratch, len);
	struct hist_line *l = line_at(h, h->n);
	l->off = (uint32_t)at;
	l->len = (uint32_t)len;
	l->cells = (uint32_t)(n - pads);
	l->rows = l->rows_cols = 0;
	l->flags = flags & LINE_PROMPT;
	h->n++;
	h->wr = (size_t)at + len;
	h->open = wrapped;
	h->open_style = st;
	return 0;
}

static int decode(const struct hist *h, const struct hist_line *l,
                  struct cell *out, int max) {
	const uint8_t *p = h->buf + l->off;
	size_t i = 0;
	int n = 0;
	struct hist_style s = plain;
	while (i < l->len) {
		if (p[i] == STYLE_MARK) {
			if (l->len - i < 1 + STYLE_LEN)
				break;
			get_style(p + i + 1, &s);
			i += 1 + STYLE_LEN;
			continue;
		}
		if (p[i] == LINE_MARK) {
			i++;
			continue;
		}
		uint32_t cp = 0;
		if (p[i] == EMPTY_MARK)
			i++;
		else
			i += get_utf8(p + i, l->len - i, &cp);
		int w = cp ? width_of(cp) : 1;
		if (w == 0) {
			if (n > 0 && n <= max) {
				struct cell *c = &out[n - 1];
				if ((c->attr & ATTR_WIDE_TAIL) && n > 1)
					c--;
				for (int k = 0; k < CELL_COMB; k++)
					if (!c->comb[k]) {
						c->comb[k] = cp;
						break;
					}
			}
			continue;
		}
		for (int k = 0; k < w; k++, n++) {
			if (n >= max)
				continue;
			struct cell *c = &out[n];
			memset(c, 0, sizeof(*c));
			c->cp = k ? 0 : cp;
			c->fg = s.fg, c->bg = s.bg, c->ul = s.ul, c->link = s.link;
			c->attr = s.attr;
			if (w == 2)
				c->attr |= k ? ATTR_WIDE_TAIL : ATTR_WIDE;
		}
	}
	return n;
}

int hist_get(const struct hist *h, size_t i, struct cell *out, int max,
             uint8_t *flags) {
	if (i >= h->n)
		return -1;
	const struct hist_line *l = line_at(h, i);
	if (flags)
		*flags = l->flags;
	return decode(h, l, out, max);
}

int hist_pop(struct hist *h, struct cell *out, int max, uint8_t *flags,
             int *open) {
	if (h->n == 0)
		return -1;
	const struct hist_line *l = line_at(h, h->n - 1);
	int n = decode(h, l, out, max);
	if (flags)
		*flags = l->flags;
	if (open)
		*open = h->open;
	h->wr = l->off;
	h->n--;
	h->open = 0;
	if (h->n == 0)
		h->wr = 0;
	return n;
}

int split_rows(const struct cell *c, int n, int cols, int *starts, int max) {
	int rows = 1, w = 0;
	if (max > 0)
		starts[0] = 0;
	for (int i = 0; i < n;) {
		int cw = (c[i].attr & ATTR_WIDE) ? 2 : 1;
		if (w + cw > cols && w > 0) {
			if (rows < max)
				starts[rows] = i;
			rows++;
			w = 0;
		}
		w += cw;
		i += cw;
	}
	return rows;
}

int hist_rows(struct hist *h, size_t i, int cols) {
	if (i >= h->n || cols <= 0)
		return 0;
	struct hist_line *l = line_at(h, i);
	if (l->rows_cols == cols)
		return l->rows;
	const uint8_t *p = h->buf + l->off;
	int rows = 1, w = 0;
	for (size_t k = 0; k < l->len;) {
		if (p[k] == STYLE_MARK) {
			k += 1 + STYLE_LEN;
			continue;
		}
		if (p[k] == LINE_MARK) {
			k++;
			continue;
		}
		uint32_t cp = 0;
		if (p[k] == EMPTY_MARK)
			k++;
		else
			k += get_utf8(p + k, l->len - k, &cp);
		int cw = cp ? width_of(cp) : 1;
		if (cw == 0)
			continue;
		if (w + cw > cols && w > 0)
			rows++, w = 0;
		w += cw;
	}
	if (rows <= 0xFFFF && cols <= 0xFFFF) {
		l->rows = (uint16_t)rows;
		l->rows_cols = (uint16_t)cols;
	}
	return rows;
}
