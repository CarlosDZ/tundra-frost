#include "screen.h"
#include "history.h"
#include "width.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DA1_REPLY "\x1b[?62;22c"
#define DA2_REPLY "\x1b[>41;400;0c"

#define PAL_FG 256
#define PAL_BG 257
#define PAL_CURSOR 258

static const uint16_t dec_graphics[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

static struct grid *cur(struct scr *s) { return &s->grid[s->alt]; }

struct cell *scr_cell(struct scr *s, int x, int y) {
	struct grid *g = cur(s);
	return &g->cells[(size_t)g->map[y] * s->cols + x];
}

static uint8_t *line(struct scr *s, int y) {
	struct grid *g = cur(s);
	return &g->lines[g->map[y]];
}

uint8_t scr_line(struct scr *s, int y) { return *line(s, y); }

static struct cell *row(struct scr *s, int y) { return scr_cell(s, 0, y); }

static uint16_t *len_of(struct scr *s, int y) {
	struct grid *g = cur(s);
	return &g->len[g->map[y]];
}

static void touch(struct scr *s, int y, int x1) {
	uint16_t *n = len_of(s, y);
	if (*n < x1)
		*n = (uint16_t)x1;
}

static void dirty(struct scr *s, int y0, int y1) {
	for (int y = y0; y <= y1; y++)
		s->dirty[y] = 1;
}

static void reply(struct scr *s, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	size_t room = REPLY_MAX - s->reply_len;
	int n = vsnprintf((char *)s->reply + s->reply_len, room, fmt, ap);
	va_end(ap);
	if (n > 0 && (size_t)n < room)
		s->reply_len += (size_t)n;
}

static struct cell blank(const struct scr *s) {
	struct cell c = {0};
	c.bg = s->pen.bg;
	return c;
}

static int empty_plain_cell(const struct cell *c) {
	return !c->cp && !c->fg && !c->bg && !c->ul && !c->link &&
	       !(c->attr & ~ATTR_LAYOUT);
}

static void unwide(struct scr *s, int x, int y) {
	struct cell *c = scr_cell(s, x, y);
	if ((c->attr & ATTR_WIDE_TAIL) && x > 0)
		c[-1] = blank(s);
	if ((c->attr & ATTR_WIDE) && x + 1 < s->cols)
		c[1] = blank(s);
}

static void fill(struct scr *s, int y, int x0, int x1) {
	if (x0 < 0)
		x0 = 0;
	if (x1 > s->cols)
		x1 = s->cols;
	if (x0 >= x1)
		return;
	unwide(s, x0, y);
	unwide(s, x1 - 1, y);
	struct cell b = blank(s), *c = row(s, y);
	uint16_t *n = len_of(s, y);
	if (b.bg == COLOR_DEFAULT) {
		int end = x1 < *n ? x1 : *n;
		if (x0 < end)
			memset(c + x0, 0, (size_t)(end - x0) * sizeof(*c));
		if (x0 < *n && x1 >= *n)
			*n = (uint16_t)x0;
	} else {
		for (int x = x0; x < x1; x++)
			c[x] = b;
		touch(s, y, x1);
	}
	s->dirty[y] = 1;
}

static void seal(struct scr *s) {
	if (!s->alt && s->hooks.seal)
		s->hooks.seal(s->ctx);
}

static void clear_rows(struct scr *s, int y0, int y1) {
	if (y0 == 0)
		seal(s);
	for (int y = y0; y <= y1; y++) {
		fill(s, y, 0, s->cols);
		*line(s, y) = 0;
	}
}

static void reverse(uint16_t *a, int i, int j) {
	for (; i < j; i++, j--) {
		uint16_t t = a[i];
		a[i] = a[j];
		a[j] = t;
	}
}

static void rotate(struct scr *s, int top, int bot, int n, int up) {
	uint16_t *map = cur(s)->map;
	int k = up ? top + n : bot - n + 1;
	reverse(map, top, k - 1);
	reverse(map, k, bot);
	reverse(map, top, bot);
}

static void scroll_up(struct scr *s, int top, int bot, int n, int history) {
	int h = bot - top + 1;
	if (n > h)
		n = h;
	if (n <= 0)
		return;
	if (history && !s->alt && top == 0 && s->hooks.scrollout)
		for (int i = 0; i < n; i++) {
			uint8_t fl = *line(s, i);
			int k = fl & LINE_WRAPPED ? s->cols : *len_of(s, i);
			s->hooks.scrollout(s->ctx, row(s, i), k, fl);
		}
	rotate(s, top, bot, n, 1);
	clear_rows(s, bot - n + 1, bot);
	dirty(s, top, bot);
}

static void scroll_down(struct scr *s, int top, int bot, int n) {
	int h = bot - top + 1;
	if (n > h)
		n = h;
	if (n <= 0)
		return;
	if (top == 0)
		seal(s);
	rotate(s, top, bot, n, 0);
	clear_rows(s, top, top + n - 1);
	dirty(s, top, bot);
}

static void linefeed(struct scr *s) {
	if (s->y == s->bot)
		scroll_up(s, s->top, s->bot, 1, 1);
	else if (s->y < s->rows - 1)
		s->y++;
}

static void reverse_index(struct scr *s) {
	if (s->y == s->top)
		scroll_down(s, s->top, s->bot, 1);
	else if (s->y > 0)
		s->y--;
}

static void move_to(struct scr *s, int x, int y) {
	int lo = s->origin ? s->top : 0, hi = s->origin ? s->bot : s->rows - 1;
	s->x = x < 0 ? 0 : x >= s->cols ? s->cols - 1 : x;
	s->y = y < lo ? lo : y > hi ? hi : y;
	s->pending = 0;
}

static void move_origin(struct scr *s, int x, int y) {
	move_to(s, x, s->origin ? y + s->top : y);
}

static void wrap(struct scr *s) {
	*line(s, s->y) |= LINE_WRAPPED;
	s->x = 0;
	s->pending = 0;
	linefeed(s);
}

static void put_cell(struct scr *s, struct cell *c, uint32_t cp) {
	c->cp = cp;
	c->comb[0] = c->comb[1] = 0;
	c->fg = s->pen.fg;
	c->bg = s->pen.bg;
	c->ul = s->pen.ul;
	c->attr = s->pen.attr;
	c->link = s->pen.link;
}

static void insert_cells(struct scr *s, int n) {
	if (n > s->cols - s->x)
		n = s->cols - s->x;
	if (n <= 0)
		return;
	struct cell *c = row(s, s->y), b = blank(s);
	uint16_t *len = len_of(s, s->y);
	if (c[s->x].attr & ATTR_WIDE_TAIL) {
		if (s->x > 0)
			c[s->x - 1] = b;
		c[s->x] = b;
	}
	memmove(c + s->x + n, c + s->x,
	        (size_t)(s->cols - s->x - n) * sizeof(struct cell));
	for (int x = s->x; x < s->x + n; x++)
		c[x] = b;
	if (c[s->cols - 1].attr & ATTR_WIDE)
		c[s->cols - 1] = b;
	if (*len > s->x)
		*len = (uint16_t)(*len + n < s->cols ? *len + n : s->cols);
	if (b.bg != COLOR_DEFAULT)
		touch(s, s->y, s->x + n);
	s->dirty[s->y] = 1;
}

static void delete_cells(struct scr *s, int n) {
	if (n > s->cols - s->x)
		n = s->cols - s->x;
	if (n <= 0)
		return;
	struct cell *c = row(s, s->y);
	unwide(s, s->x, s->y);
	unwide(s, s->x + n - 1, s->y);
	memmove(c + s->x, c + s->x + n,
	        (size_t)(s->cols - s->x - n) * sizeof(struct cell));
	struct cell b = blank(s);
	for (int x = s->cols - n; x < s->cols; x++)
		c[x] = b;
	if (c[s->x].attr & ATTR_WIDE_TAIL)
		c[s->x] = b;
	uint16_t *len = len_of(s, s->y);
	if (b.bg != COLOR_DEFAULT)
		*len = (uint16_t)s->cols;
	else if (*len > s->x)
		*len = (uint16_t)(*len - n > s->x ? *len - n : s->x);
	s->dirty[s->y] = 1;
}

static void combine(struct scr *s, uint32_t cp) {
	int x = s->pending ? s->x : s->x - 1;
	if (x < 0)
		return;
	struct cell *c = scr_cell(s, x, s->y);
	if ((c->attr & ATTR_WIDE_TAIL) && x > 0)
		c--;
	if (!c->cp)
		return;
	for (int i = 0; i < CELL_COMB; i++) {
		if (!c->comb[i]) {
			c->comb[i] = cp;
			s->dirty[s->y] = 1;
			return;
		}
	}
}

static void print(struct scr *s, uint32_t cp) {
	if (s->g[s->gl] == '0' && cp >= 0x5F && cp <= 0x7E)
		cp = dec_graphics[cp - 0x5F];
	int w = width_of(cp);
	if (w == 0) {
		combine(s, cp);
		return;
	}
	if (s->pending && s->autowrap)
		wrap(s);
	if (w == 2 && s->x == s->cols - 1) {
		if (!s->autowrap || s->cols < 2)
			return;
		fill(s, s->y, s->x, s->cols);
		scr_cell(s, s->x, s->y)->attr |= ATTR_PAD;
		touch(s, s->y, s->cols);
		wrap(s);
	}
	if (s->insert)
		insert_cells(s, w);
	unwide(s, s->x, s->y);
	if (w == 2)
		unwide(s, s->x + 1, s->y);
	struct cell *c = scr_cell(s, s->x, s->y);
	put_cell(s, c, cp);
	if (w == 2) {
		c->attr |= ATTR_WIDE;
		put_cell(s, c + 1, 0);
		c[1].attr |= ATTR_WIDE_TAIL;
	}
	touch(s, s->y, s->x + w);
	s->dirty[s->y] = 1;
	s->last = cp;
	if (s->x + w >= s->cols) {
		s->x = s->cols - 1;
		s->pending = 1;
	} else {
		s->x += w;
	}
}

static void print_run(struct scr *s, const uint8_t *p, size_t n) {
	if (s->g[s->gl] == '0' || s->insert) {
		for (size_t i = 0; i < n; i++)
			print(s, p[i]);
		return;
	}
	while (n) {
		if (s->pending) {
			if (!s->autowrap) {
				print(s, p[n - 1]);
				return;
			}
			wrap(s);
		}
		size_t k = (size_t)(s->cols - s->x);
		if (k > n)
			k = n;
		unwide(s, s->x, s->y);
		unwide(s, s->x + (int)k - 1, s->y);
		struct cell *c = scr_cell(s, s->x, s->y);
		for (size_t i = 0; i < k; i++)
			put_cell(s, c + i, p[i]);
		touch(s, s->y, s->x + (int)k);
		s->dirty[s->y] = 1;
		s->last = p[k - 1];
		p += k;
		n -= k;
		if (s->x + (int)k >= s->cols) {
			s->x = s->cols - 1;
			s->pending = 1;
		} else {
			s->x += (int)k;
		}
	}
}

static void tab(struct scr *s, int n) {
	while (n-- > 0) {
		int x = s->x + 1;
		while (x < s->cols - 1 && !s->tabs[x])
			x++;
		s->x = x < s->cols ? x : s->cols - 1;
	}
	s->pending = 0;
}

static void back_tab(struct scr *s, int n) {
	while (n-- > 0 && s->x > 0) {
		int x = s->x - 1;
		while (x > 0 && !s->tabs[x])
			x--;
		s->x = x;
	}
	s->pending = 0;
}

static void reset_tabs(struct scr *s) {
	for (int x = 0; x < s->cols; x++)
		s->tabs[x] = x % 8 == 0 && x;
}

static void save_cursor(struct scr *s) {
	struct saved *v = &cur(s)->saved;
	v->x = s->x, v->y = s->y, v->pending = s->pending, v->origin = s->origin;
	v->pen = s->pen;
	memcpy(v->g, s->g, 4);
	v->gl = s->gl;
}

static void restore_cursor(struct scr *s) {
	struct saved *v = &cur(s)->saved;
	s->origin = v->origin;
	s->pen = v->pen;
	memcpy(s->g, v->g, 4);
	s->gl = v->gl;
	move_to(s, v->x, v->y);
	s->pending = v->pending;
}

static void use_grid(struct scr *s, int alt) {
	if (s->alt == alt)
		return;
	s->alt = alt;
	dirty(s, 0, s->rows - 1);
}

static void pen_reset(struct scr *s) {
	uint16_t link = s->pen.link;
	memset(&s->pen, 0, sizeof(s->pen));
	s->pen.link = link;
}

static void soft_reset(struct scr *s) {
	s->autowrap = 1, s->origin = 0, s->insert = 0, s->newline = 0;
	s->app_cursor = 0, s->app_keypad = 0, s->cursor_visible = 1;
	s->top = 0, s->bot = s->rows - 1;
	memcpy(s->g, "BBBB", 4);
	s->gl = 0;
	pen_reset(s);
	s->pen.link = 0;
	s->grid[0].saved = (struct saved){0, 0, 0, 0, {0}, {'B', 'B', 'B', 'B'}, 0};
	s->grid[1].saved = s->grid[0].saved;
}

static void full_reset(struct scr *s) {
	soft_reset(s);
	use_grid(s, 0);
	s->grid[0].nkeys = s->grid[1].nkeys = 0;
	for (int a = 1; a >= 0; a--) {
		s->alt = a;
		clear_rows(s, 0, s->rows - 1);
	}
	s->x = s->y = s->pending = 0;
	s->reverse = 0, s->bracketed_paste = 0, s->focus_events = 0;
	s->alt_scroll = 1, s->sync = 0, s->mouse = 0, s->mouse_sgr = 0;
	s->mouse_pixels = 0;
	s->cursor_shape = CURSOR_DEFAULT, s->cursor_blink = 1;
	memset(s->palette_set, 0, sizeof(s->palette_set));
	reset_tabs(s);
	dirty(s, 0, s->rows - 1);
}

static size_t block_size(int cols, int rows) {
	size_t n = (size_t)cols * rows;
	return 2 * n * sizeof(struct cell) + 4 * (size_t)rows * sizeof(uint16_t) +
	       2 * (size_t)rows;
}

static void block_layout(void *block, int cols, int rows, struct cell *cells[2],
                         uint16_t *maps[2], uint16_t *lens[2],
                         uint8_t *lines[2]) {
	size_t n = (size_t)cols * rows;
	cells[0] = block;
	cells[1] = cells[0] + n;
	maps[0] = (uint16_t *)(cells[1] + n);
	maps[1] = maps[0] + rows;
	lens[0] = maps[1] + rows;
	lens[1] = lens[0] + rows;
	lines[0] = (uint8_t *)(lens[1] + rows);
	lines[1] = lines[0] + rows;
}

static void measure(const struct cell *cells, uint16_t *len, int cols,
                    int rows) {
	static const struct cell zero;
	for (int r = 0; r < rows; r++) {
		const struct cell *c = cells + (size_t)r * cols;
		int n = cols;
		while (n > 0 && !memcmp(&c[n - 1], &zero, sizeof(zero)))
			n--;
		len[r] = (uint16_t)n;
	}
}

int scr_init(struct scr *s, int cols, int rows) {
	memset(s, 0, sizeof(*s));
	if (cols < 1 || rows < 1 || cols > 65535 || rows > 65535)
		return -1;
	s->cols = cols, s->rows = rows;
	s->block = calloc(1, block_size(cols, rows));
	s->tabs = calloc((size_t)cols, 1);
	s->dirty = calloc((size_t)rows, 1);
	if (!s->block || !s->tabs || !s->dirty) {
		scr_free(s);
		return -1;
	}
	struct cell *cells[2];
	uint16_t *maps[2], *lens[2];
	uint8_t *lines[2];
	block_layout(s->block, cols, rows, cells, maps, lens, lines);
	for (int i = 0; i < 2; i++) {
		s->grid[i].cells = cells[i];
		s->grid[i].map = maps[i];
		s->grid[i].lines = lines[i];
		s->grid[i].len = lens[i];
		for (int y = 0; y < rows; y++)
			maps[i][y] = (uint16_t)y;
	}
	memset(s->light, 0, sizeof(s->light));
	s->light[PAL_FG] = 1;
	vt_init(&s->vt);
	full_reset(s);
	return 0;
}

void scr_free(struct scr *s) {
	free(s->block);
	free(s->tabs);
	free(s->dirty);
	for (int i = 0; i < s->nlinks; i++)
		free(s->links[i]);
	memset(s, 0, sizeof(*s));
}

static int ext_color(const struct vt_seq *q, int i, uint32_t *out) {
	int n = q->nparams;
	if (i + 1 < n && (q->sub >> (i + 1) & 1)) {
		unsigned v[6] = {0};
		int k = 0;
		for (int j = i + 1; j < n && (q->sub >> j & 1); j++)
			if (k < 6)
				v[k++] = q->param[j];
		int used = 0;
		for (int j = i + 1; j < n && (q->sub >> j & 1); j++)
			used++;
		if (v[0] == 5 && k >= 2)
			*out = COLOR_INDEX(v[1] & 0xFF);
		else if (v[0] == 2 && k >= 5)
			*out = COLOR_RGB(v[2] & 0xFF, v[3] & 0xFF, v[4] & 0xFF);
		else if (v[0] == 2 && k == 4)
			*out = COLOR_RGB(v[1] & 0xFF, v[2] & 0xFF, v[3] & 0xFF);
		return used;
	}
	unsigned kind = vt_param(q, i + 1, 0);
	if (kind == 5 && i + 2 < n) {
		*out = COLOR_INDEX(vt_param(q, i + 2, 0) & 0xFF);
		return 2;
	}
	if (kind == 2 && i + 4 < n) {
		*out = COLOR_RGB(vt_param(q, i + 2, 0) & 0xFF,
		                 vt_param(q, i + 3, 0) & 0xFF,
		                 vt_param(q, i + 4, 0) & 0xFF);
		return 4;
	}
	return i + 1 < n ? 1 : 0;
}

static void set_ul(struct pen *p, unsigned style) {
	p->attr =
	    (uint16_t)((p->attr & ~ATTR_UL_MASK) |
	               (style > UL_DASHED ? UL_SINGLE : style) << ATTR_UL_SHIFT);
}

static void sgr(struct scr *s, const struct vt_seq *q) {
	struct pen *p = &s->pen;
	if (q->nparams == 0) {
		pen_reset(s);
		return;
	}
	for (int i = 0; i < q->nparams; i++) {
		unsigned v = vt_param(q, i, 0);
		int skip_subs = 1;
		switch (v) {
		case 0:
			pen_reset(s);
			break;
		case 1:
			p->attr |= ATTR_BOLD;
			break;
		case 2:
			p->attr |= ATTR_DIM;
			break;
		case 3:
			p->attr |= ATTR_ITALIC;
			break;
		case 4:
			if (i + 1 < q->nparams && (q->sub >> (i + 1) & 1))
				set_ul(p, q->param[++i]);
			else
				set_ul(p, UL_SINGLE);
			break;
		case 5:
		case 6:
			p->attr |= ATTR_BLINK;
			break;
		case 7:
			p->attr |= ATTR_INVERSE;
			break;
		case 8:
			p->attr |= ATTR_HIDDEN;
			break;
		case 9:
			p->attr |= ATTR_STRIKE;
			break;
		case 21:
			set_ul(p, UL_DOUBLE);
			break;
		case 22:
			p->attr &= ~(ATTR_BOLD | ATTR_DIM);
			break;
		case 23:
			p->attr &= ~ATTR_ITALIC;
			break;
		case 24:
			set_ul(p, UL_NONE);
			break;
		case 25:
			p->attr &= ~ATTR_BLINK;
			break;
		case 27:
			p->attr &= ~ATTR_INVERSE;
			break;
		case 28:
			p->attr &= ~ATTR_HIDDEN;
			break;
		case 29:
			p->attr &= ~ATTR_STRIKE;
			break;
		case 38:
			i += ext_color(q, i, &p->fg), skip_subs = 0;
			break;
		case 39:
			p->fg = COLOR_DEFAULT;
			break;
		case 48:
			i += ext_color(q, i, &p->bg), skip_subs = 0;
			break;
		case 49:
			p->bg = COLOR_DEFAULT;
			break;
		case 53:
			p->attr |= ATTR_OVERLINE;
			break;
		case 55:
			p->attr &= ~ATTR_OVERLINE;
			break;
		case 58:
			i += ext_color(q, i, &p->ul), skip_subs = 0;
			break;
		case 59:
			p->ul = COLOR_DEFAULT;
			break;
		default:
			if (v >= 30 && v <= 37)
				p->fg = COLOR_INDEX(v - 30);
			else if (v >= 40 && v <= 47)
				p->bg = COLOR_INDEX(v - 40);
			else if (v >= 90 && v <= 97)
				p->fg = COLOR_INDEX(v - 90 + 8);
			else if (v >= 100 && v <= 107)
				p->bg = COLOR_INDEX(v - 100 + 8);
		}
		if (skip_subs)
			while (i + 1 < q->nparams && (q->sub >> (i + 1) & 1))
				i++;
	}
}

static int mode_state(struct scr *s, int dec, unsigned m) {
	if (!dec) {
		if (m == 4)
			return s->insert ? 1 : 2;
		if (m == 20)
			return s->newline ? 1 : 2;
		return 0;
	}
	switch (m) {
	case 1:
		return s->app_cursor ? 1 : 2;
	case 5:
		return s->reverse ? 1 : 2;
	case 6:
		return s->origin ? 1 : 2;
	case 7:
		return s->autowrap ? 1 : 2;
	case 12:
		return s->cursor_blink ? 1 : 2;
	case 25:
		return s->cursor_visible ? 1 : 2;
	case 66:
		return s->app_keypad ? 1 : 2;
	case 47:
	case 1047:
	case 1049:
		return s->alt ? 1 : 2;
	case 1000:
	case 1002:
	case 1003:
		return s->mouse == (int)m ? 1 : 2;
	case 1004:
		return s->focus_events ? 1 : 2;
	case 1006:
		return s->mouse_sgr ? 1 : 2;
	case 1007:
		return s->alt_scroll ? 1 : 2;
	case 1016:
		return s->mouse_pixels ? 1 : 2;
	case 2004:
		return s->bracketed_paste ? 1 : 2;
	case 2026:
		return s->sync ? 1 : 2;
	}
	return 0;
}

static void set_modes(struct scr *s, const struct vt_seq *q, int on) {
	int dec = q->priv == '?';
	if (q->priv && !dec)
		return;
	for (int i = 0; i < q->nparams; i++) {
		unsigned m = vt_param(q, i, 0);
		if (!dec) {
			if (m == 4)
				s->insert = on;
			else if (m == 20)
				s->newline = on;
			continue;
		}
		switch (m) {
		case 1:
			s->app_cursor = on;
			break;
		case 5:
			s->reverse = on;
			dirty(s, 0, s->rows - 1);
			break;
		case 6:
			s->origin = on;
			move_origin(s, 0, 0);
			break;
		case 7:
			s->autowrap = on;
			break;
		case 12:
			s->cursor_blink = on;
			break;
		case 25:
			s->cursor_visible = on;
			break;
		case 66:
			s->app_keypad = on;
			break;
		case 47:
		case 1047:
			if (!on && m == 1047 && s->alt)
				clear_rows(s, 0, s->rows - 1);
			use_grid(s, on);
			break;
		case 1048:
			if (on)
				save_cursor(s);
			else
				restore_cursor(s);
			break;
		case 1049:
			if (on) {
				save_cursor(s);
				use_grid(s, 1);
				clear_rows(s, 0, s->rows - 1);
			} else {
				use_grid(s, 0);
				restore_cursor(s);
			}
			break;
		case 1000:
		case 1002:
		case 1003:
			s->mouse = on ? (int)m : MOUSE_OFF;
			break;
		case 1004:
			s->focus_events = on;
			break;
		case 1006:
			s->mouse_sgr = on;
			break;
		case 1007:
			s->alt_scroll = on;
			break;
		case 1016:
			s->mouse_pixels = on;
			break;
		case 2004:
			s->bracketed_paste = on;
			break;
		case 2026:
			s->sync = on;
			break;
		}
	}
}

static void keys_csi(struct scr *s, const struct vt_seq *q) {
	struct grid *g = cur(s);
	int flags = g->nkeys ? g->keys[g->nkeys - 1] : 0;
	switch (q->priv) {
	case '?':
		reply(s, "\x1b[?%du", flags);
		break;
	case '>':
		if (g->nkeys == KEY_STACK) {
			memmove(g->keys, g->keys + 1, (KEY_STACK - 1) * sizeof(int));
			g->nkeys--;
		}
		g->keys[g->nkeys++] = (int)vt_param(q, 0, 0) & 31;
		break;
	case '<': {
		int n = (int)vt_param(q, 0, 1);
		g->nkeys = n >= g->nkeys ? 0 : g->nkeys - n;
		break;
	}
	case '=': {
		int f = (int)vt_param(q, 0, 0) & 31, mode = (int)vt_param(q, 1, 1);
		if (mode == 2)
			f |= flags;
		else if (mode == 3)
			f = flags & ~f;
		if (!g->nkeys)
			g->nkeys = 1;
		g->keys[g->nkeys - 1] = f;
		break;
	}
	}
}

static void sgr_report(struct scr *s, char *buf, size_t size) {
	const struct pen *p = &s->pen;
	size_t n = (size_t)snprintf(buf, size, "0");
	static const struct {
		uint16_t bit;
		const char *code;
	} attrs[] = {{ATTR_BOLD, "1"},   {ATTR_DIM, "2"},      {ATTR_ITALIC, "3"},
	             {ATTR_BLINK, "5"},  {ATTR_INVERSE, "7"},  {ATTR_HIDDEN, "8"},
	             {ATTR_STRIKE, "9"}, {ATTR_OVERLINE, "53"}};
	for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
		if (p->attr & attrs[i].bit && n < size)
			n += (size_t)snprintf(buf + n, size - n, ";%s", attrs[i].code);
	unsigned ul = (p->attr & ATTR_UL_MASK) >> ATTR_UL_SHIFT;
	if (ul && n < size)
		n += (size_t)snprintf(buf + n, size - n, ul == 1 ? ";4" : ";4:%u", ul);
	const struct {
		uint32_t c;
		int base;
	} cols[] = {{p->fg, 38}, {p->bg, 48}, {p->ul, 58}};
	for (int i = 0; i < 3 && n < size; i++) {
		uint32_t c = cols[i].c;
		if (COLOR_KIND(c) == 1)
			n += (size_t)snprintf(buf + n, size - n, ";%d:5:%u", cols[i].base,
			                      c & 0xFF);
		else if (COLOR_KIND(c) == 2)
			n += (size_t)snprintf(buf + n, size - n, ";%d:2::%u:%u:%u",
			                      cols[i].base, c >> 16 & 0xFF, c >> 8 & 0xFF,
			                      c & 0xFF);
	}
}

static void csi(struct scr *s, const struct vt_seq *q) {
	unsigned p0 = vt_param(q, 0, 0), n = p0 ? p0 : 1;
	int inter = q->inter[0];
	if (inter == ' ' && q->final == 'q' && !q->priv) {
		static const enum cursor_shape shapes[] = {
		    CURSOR_DEFAULT,   CURSOR_BLOCK, CURSOR_BLOCK, CURSOR_UNDERLINE,
		    CURSOR_UNDERLINE, CURSOR_BAR,   CURSOR_BAR};
		if (p0 <= 6) {
			s->cursor_shape = shapes[p0];
			s->cursor_blink = p0 == 0 || p0 % 2 == 1;
		}
		return;
	}
	if (inter == '$' && q->final == 'p') {
		int dec = q->priv == '?';
		reply(s, dec ? "\x1b[?%u;%d$y" : "\x1b[%u;%d$y", p0,
		      mode_state(s, dec, p0));
		return;
	}
	if (inter == '!' && q->final == 'p') {
		soft_reset(s);
		return;
	}
	if (inter)
		return;
	if (q->final == 'u' && q->priv) {
		keys_csi(s, q);
		return;
	}
	if (q->priv == '?') {
		if (q->final == 'h' || q->final == 'l')
			set_modes(s, q, q->final == 'h');
		else if (q->final == 'n' && p0 == 6)
			reply(s, "\x1b[?%d;%dR", s->y - (s->origin ? s->top : 0) + 1,
			      s->x + 1);
		else if (q->final == 'n' && p0 == 996)
			reply(s, "\x1b[?997;%dn", s->light[PAL_BG] ? 2 : 1);
		return;
	}
	if (q->priv == '>') {
		if (q->final == 'c')
			reply(s, DA2_REPLY);
		return;
	}
	if (q->priv)
		return;

	switch (q->final) {
	case '@':
		insert_cells(s, (int)n);
		break;
	case 'A': {
		int lo = s->y >= s->top ? s->top : 0;
		int y = s->y - (int)n;
		s->y = y < lo ? lo : y;
		s->pending = 0;
		break;
	}
	case 'B':
	case 'e': {
		int hi = s->y <= s->bot ? s->bot : s->rows - 1;
		int y = s->y + (int)n;
		s->y = y > hi ? hi : y;
		s->pending = 0;
		break;
	}
	case 'C':
	case 'a':
		move_to(s, s->x + (int)n, s->y);
		break;
	case 'D':
		move_to(s, s->x - (int)n, s->y);
		break;
	case 'E':
		s->x = 0;
		csi(s, &(struct vt_seq){.final = 'B', .nparams = 1, .param = {n}});
		break;
	case 'F':
		s->x = 0;
		csi(s, &(struct vt_seq){.final = 'A', .nparams = 1, .param = {n}});
		break;
	case 'G':
	case '`':
		move_to(s, (int)n - 1, s->y);
		break;
	case 'H':
	case 'f':
		move_origin(s, (int)vt_param(q, 1, 1) - 1, (int)(p0 ? p0 : 1) - 1);
		break;
	case 'I':
		tab(s, (int)n);
		break;
	case 'J':
		if (p0 == 0) {
			fill(s, s->y, s->x, s->cols);
			if (s->y + 1 < s->rows)
				clear_rows(s, s->y + 1, s->rows - 1);
		} else if (p0 == 1) {
			if (s->y > 0)
				clear_rows(s, 0, s->y - 1);
			fill(s, s->y, 0, s->x + 1);
		} else if (p0 == 2) {
			clear_rows(s, 0, s->rows - 1);
		} else if (p0 == 3 && s->hooks.clear_history) {
			s->hooks.clear_history(s->ctx);
		}
		break;
	case 'K':
		if (s->y == 0 && (p0 == 2 || (p0 != 1 && s->x == 0)))
			seal(s);
		if (p0 == 0)
			fill(s, s->y, s->x, s->cols);
		else if (p0 == 1)
			fill(s, s->y, 0, s->x + 1);
		else if (p0 == 2)
			fill(s, s->y, 0, s->cols);
		if (p0 != 1)
			*line(s, s->y) &= ~LINE_WRAPPED;
		break;
	case 'L':
		if (s->y >= s->top && s->y <= s->bot) {
			scroll_down(s, s->y, s->bot, (int)n);
			s->x = 0, s->pending = 0;
		}
		break;
	case 'M':
		if (s->y >= s->top && s->y <= s->bot) {
			scroll_up(s, s->y, s->bot, (int)n, 0);
			s->x = 0, s->pending = 0;
		}
		break;
	case 'P':
		delete_cells(s, (int)n);
		break;
	case 'S':
		scroll_up(s, s->top, s->bot, (int)n, 1);
		break;
	case 'T':
		scroll_down(s, s->top, s->bot, (int)n);
		break;
	case 'X':
		fill(s, s->y, s->x, s->x + (int)n), s->pending = 0;
		break;
	case 'Z':
		back_tab(s, (int)n);
		break;
	case 'b':
		if (s->last)
			for (unsigned i = 0; i < n && i < 65535; i++)
				print(s, s->last);
		break;
	case 'c':
		if (p0 == 0)
			reply(s, DA1_REPLY);
		break;
	case 'd':
		move_origin(s, s->x, (int)n - 1);
		break;
	case 'g':
		if (p0 == 0)
			s->tabs[s->x] = 0;
		else if (p0 == 3)
			memset(s->tabs, 0, (size_t)s->cols);
		break;
	case 'h':
	case 'l':
		set_modes(s, q, q->final == 'h');
		break;
	case 'm':
		sgr(s, q);
		break;
	case 'n':
		if (p0 == 5)
			reply(s, "\x1b[0n");
		else if (p0 == 6)
			reply(s, "\x1b[%d;%dR", s->y - (s->origin ? s->top : 0) + 1,
			      s->x + 1);
		break;
	case 'r': {
		int top = (int)vt_param(q, 0, 1), bot = (int)vt_param(q, 1, s->rows);
		top = top ? top : 1;
		bot = bot && bot <= s->rows ? bot : s->rows;
		if (top < bot) {
			s->top = top - 1, s->bot = bot - 1;
			move_origin(s, 0, 0);
		}
		break;
	}
	case 's':
		save_cursor(s);
		break;
	case 'u':
		restore_cursor(s);
		break;
	case 't':
		if (p0 == 14)
			reply(s, "\x1b[4;%d;%dt", s->rows * s->cell_h, s->cols * s->cell_w);
		else if (p0 == 16)
			reply(s, "\x1b[6;%d;%dt", s->cell_h, s->cell_w);
		else if (p0 == 18)
			reply(s, "\x1b[8;%d;%dt", s->rows, s->cols);
		break;
	}
}

static void esc(struct scr *s, const struct vt_seq *q) {
	char i = q->inter[0];
	if (i == '(' || i == ')' || i == '*' || i == '+') {
		s->g[i == '('   ? 0
		     : i == ')' ? 1
		     : i == '*' ? 2
		                : 3] = q->final == '0' ? '0' : 'B';
		return;
	}
	if (i == '#') {
		if (q->final == '8') {
			for (int y = 0; y < s->rows; y++)
				for (int x = 0; x < s->cols; x++) {
					struct cell *c = scr_cell(s, x, y);
					*c = (struct cell){0};
					c->cp = 'E';
				}
			for (int y = 0; y < s->rows; y++)
				touch(s, y, s->cols);
			s->top = 0, s->bot = s->rows - 1;
			move_to(s, 0, 0);
			dirty(s, 0, s->rows - 1);
		}
		return;
	}
	if (i)
		return;
	switch (q->final) {
	case '7':
		save_cursor(s);
		break;
	case '8':
		restore_cursor(s);
		break;
	case 'D':
		linefeed(s), s->pending = 0;
		break;
	case 'E':
		linefeed(s);
		s->x = 0, s->pending = 0;
		break;
	case 'H':
		s->tabs[s->x] = 1;
		break;
	case 'M':
		reverse_index(s), s->pending = 0;
		break;
	case 'c':
		full_reset(s);
		break;
	case '=':
		s->app_keypad = 1;
		break;
	case '>':
		s->app_keypad = 0;
		break;
	}
}

static void execute(struct scr *s, uint32_t c) {
	switch (c) {
	case 0x07:
		if (s->hooks.bell)
			s->hooks.bell(s->ctx);
		break;
	case 0x08:
		if (s->x > 0)
			s->x--;
		s->pending = 0;
		break;
	case 0x09:
		tab(s, 1);
		break;
	case 0x0A:
	case 0x0B:
	case 0x0C:
		linefeed(s);
		if (s->newline)
			s->x = 0;
		s->pending = 0;
		break;
	case 0x0D:
		s->x = 0;
		s->pending = 0;
		break;
	case 0x0E:
		s->gl = 1;
		break;
	case 0x0F:
		s->gl = 0;
		break;
	}
}

static int hexval(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_color(const char *p, size_t len, uint32_t *out) {
	unsigned v[3] = {0};
	if (len == 7 && p[0] == '#') {
		for (int i = 0; i < 6; i++) {
			int h = hexval(p[1 + i]);
			if (h < 0)
				return -1;
			v[i / 2] = v[i / 2] * 16 + (unsigned)h;
		}
		*out = COLOR_RGB(v[0], v[1], v[2]);
		return 0;
	}
	if (len < 4 || memcmp(p, "rgb:", 4) != 0)
		return -1;
	size_t i = 4;
	for (int k = 0; k < 3; k++) {
		int digits = 0;
		unsigned x = 0;
		while (i < len && p[i] != '/') {
			int h = hexval(p[i++]);
			if (h < 0 || ++digits > 4)
				return -1;
			x = x * 16 + (unsigned)h;
		}
		if (!digits || (k < 2 && (i >= len || p[i] != '/')))
			return -1;
		i++;
		v[k] = digits == 1 ? x * 17 : x >> (4 * (digits - 2));
	}
	*out = COLOR_RGB(v[0], v[1], v[2]);
	return 0;
}

static void answer_color(struct scr *s, const char *prefix, int slot) {
	const char *v = s->light[slot] ? "ffff/ffff/ffff" : "0000/0000/0000";
	reply(s, "\x1b]%s;rgb:%s\x1b\\", prefix, v);
}

static void osc_palette(struct scr *s, const char *p, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned idx = 0;
		int digits = 0;
		while (i < len && p[i] >= '0' && p[i] <= '9')
			idx = idx * 10 + (unsigned)(p[i++] - '0'), digits++;
		if (!digits || i >= len || p[i] != ';' || idx > 255)
			return;
		size_t start = ++i;
		while (i < len && p[i] != ';')
			i++;
		if (i - start == 1 && p[start] == '?') {
			char pre[8];
			snprintf(pre, sizeof(pre), "4;%u", idx);
			answer_color(s, pre, (int)idx);
		} else if (parse_color(p + start, i - start, &s->palette[idx]) == 0) {
			s->palette_set[idx] = 1;
			dirty(s, 0, s->rows - 1);
		}
		i++;
	}
}

static int link_id(struct scr *s, const char *uri, size_t len) {
	for (int i = 0; i < s->nlinks; i++)
		if (strlen(s->links[i]) == len && memcmp(s->links[i], uri, len) == 0)
			return i + 1;
	if (s->nlinks == LINKS_MAX)
		return 0;
	char *copy = malloc(len + 1);
	if (!copy)
		return 0;
	memcpy(copy, uri, len);
	copy[len] = 0;
	s->links[s->nlinks++] = copy;
	return s->nlinks;
}

static void osc(struct scr *s, const uint8_t *str, size_t len) {
	unsigned num = 0;
	size_t i = 0;
	while (i < len && str[i] >= '0' && str[i] <= '9')
		num = num * 10 + (unsigned)(str[i++] - '0');
	if (i == 0)
		return;
	const char *p = (const char *)str + i + (i < len && str[i] == ';');
	size_t n = len - (size_t)(p - (const char *)str);
	switch (num) {
	case 0:
	case 2: {
		size_t k = 0;
		for (size_t j = 0; j < n && k < TITLE_MAX - 1; j++)
			if ((uint8_t)p[j] >= 0x20)
				s->title[k++] = p[j];
		s->title[k] = 0;
		if (s->hooks.title)
			s->hooks.title(s->ctx, s->title);
		break;
	}
	case 4:
		osc_palette(s, p, n);
		break;
	case 7:
		if (s->hooks.cwd)
			s->hooks.cwd(s->ctx, p, n);
		break;
	case 8: {
		const char *uri = memchr(p, ';', n);
		if (!uri)
			break;
		uri++;
		size_t ulen = n - (size_t)(uri - p);
		s->pen.link = ulen ? (uint16_t)link_id(s, uri, ulen) : 0;
		break;
	}
	case 9:
		if (!(n >= 2 && p[0] == '4' && p[1] == ';') && s->hooks.notify)
			s->hooks.notify(s->ctx, p, n);
		break;
	case 10:
	case 11:
	case 12: {
		int slot = PAL_FG + (int)(num - 10);
		if (n == 1 && p[0] == '?') {
			char pre[4];
			snprintf(pre, sizeof(pre), "%u", num);
			answer_color(s, pre, slot);
		} else if (parse_color(p, n, &s->palette[slot]) == 0) {
			s->palette_set[slot] = 1;
			dirty(s, 0, s->rows - 1);
		}
		break;
	}
	case 52: {
		const char *data = memchr(p, ';', n);
		if (!data)
			break;
		data++;
		size_t dlen = n - (size_t)(data - p);
		if (dlen && !(dlen == 1 && data[0] == '?') && s->hooks.clipboard)
			s->hooks.clipboard(s->ctx, data, dlen);
		break;
	}
	case 99:
	case 777:
		if (s->hooks.notify)
			s->hooks.notify(s->ctx, p, n);
		break;
	case 104:
		if (n == 0)
			memset(s->palette_set, 0, 256);
		else
			for (size_t j = 0; j < n;) {
				unsigned idx = 0;
				while (j < n && p[j] >= '0' && p[j] <= '9')
					idx = idx * 10 + (unsigned)(p[j++] - '0');
				if (idx < 256)
					s->palette_set[idx] = 0;
				j++;
			}
		dirty(s, 0, s->rows - 1);
		break;
	case 110:
	case 111:
	case 112:
		s->palette_set[PAL_FG + num - 110] = 0;
		dirty(s, 0, s->rows - 1);
		break;
	case 133:
		if (n >= 1 && p[0] == 'A')
			*line(s, s->y) |= LINE_PROMPT;
		break;
	}
}

static void dcs(struct scr *s, const struct vt_seq *q, const uint8_t *str,
                size_t len) {
	if (strcmp(q->inter, "$") == 0 && q->final == 'q') {
		char buf[160];
		if (len == 1 && str[0] == 'm') {
			sgr_report(s, buf, sizeof(buf));
			reply(s, "\x1bP1$r%sm\x1b\\", buf);
		} else if (len == 1 && str[0] == 'r') {
			reply(s, "\x1bP1$r%d;%dr\x1b\\", s->top + 1, s->bot + 1);
		} else if (len == 2 && str[0] == ' ' && str[1] == 'q') {
			int ps = s->cursor_shape == CURSOR_UNDERLINE ? 3
			         : s->cursor_shape == CURSOR_BAR     ? 5
			                                             : 1;
			reply(s, "\x1bP1$r%d q\x1b\\", ps + !s->cursor_blink);
		} else {
			reply(s, "\x1bP0$r\x1b\\");
		}
		return;
	}
	if (strcmp(q->inter, "+") == 0 && q->final == 'q')
		reply(s, "\x1bP0+r\x1b\\");
}

struct lline {
	size_t start;
	int n;
	uint8_t flags;
};

struct prow {
	size_t line;
	int start, end;
};

struct reflow {
	struct cell *c;
	size_t n, size;
	struct lline *l;
	size_t nl, lsize;
	struct prow *r;
	size_t nr, rsize;
};

static int more(void **p, size_t *size, size_t need, size_t elem) {
	if (need <= *size)
		return 0;
	size_t n = *size ? *size : 64;
	while (n < need)
		n *= 2;
	void *q = realloc(*p, n * elem);
	if (!q)
		return -1;
	*p = q;
	*size = n;
	return 0;
}

static struct lline *new_line(struct reflow *f, uint8_t flags) {
	if (more((void **)&f->l, &f->lsize, f->nl + 1, sizeof(*f->l)) < 0)
		return NULL;
	struct lline *l = &f->l[f->nl++];
	l->start = f->n, l->n = 0, l->flags = flags & LINE_PROMPT;
	return l;
}

static int split_line(struct reflow *f, size_t i, int cols, size_t at,
                      size_t *nrows) {
	struct lline *l = &f->l[i];
	const struct cell *c = f->c + l->start;
	int rows = split_rows(c, l->n, cols, NULL, 0);
	int *starts = malloc((size_t)rows * sizeof(int));
	if (!starts ||
	    more((void **)&f->r, &f->rsize, f->nr + (size_t)rows, sizeof(*f->r))) {
		free(starts);
		return -1;
	}
	split_rows(c, l->n, cols, starts, rows);
	memmove(f->r + at + rows, f->r + at, (f->nr - at) * sizeof(*f->r));
	for (int k = 0; k < rows; k++) {
		struct prow *r = &f->r[at + (size_t)k];
		r->line = i;
		r->start = starts[k];
		r->end = k + 1 < rows ? starts[k + 1] : l->n;
	}
	f->nr += (size_t)rows;
	free(starts);
	*nrows = (size_t)rows;
	return 0;
}

static uint8_t row_cells(const struct reflow *f, size_t r, int cols,
                         struct cell *out) {
	const struct prow *p = &f->r[r];
	const struct lline *l = &f->l[p->line];
	int n = p->end - p->start;
	if (n > cols)
		n = cols;
	if (n > 0)
		memcpy(out, f->c + l->start + p->start, (size_t)n * sizeof(*out));
	if (n > 0 && (out[n - 1].attr & ATTR_WIDE) && n == cols)
		out[n - 1] = (struct cell){0};
	memset(out + n, 0, (size_t)(cols - n) * sizeof(*out));
	uint8_t flags = p->start == 0 ? l->flags : 0;
	if (r + 1 < f->nr && f->r[r + 1].line == p->line) {
		flags |= LINE_WRAPPED;
		if (n < cols)
			out[n].attr |= ATTR_PAD;
	}
	return flags;
}

static int pull_line(struct reflow *f, struct hist *h, uint8_t *flags) {
	int n = hist_get(h, hist_count(h) - 1, NULL, 0, NULL);
	if (n < 0 ||
	    more((void **)&f->c, &f->size, f->n + (size_t)n, sizeof(*f->c)) < 0)
		return -1;
	hist_pop(h, f->c + f->n, n, flags, NULL);
	f->n += (size_t)n;
	return n;
}

int scr_resize(struct scr *s, int cols, int rows, struct hist *h) {
	if (cols < 1 || rows < 1 || cols > 65535 || rows > 65535)
		return -1;
	if (cols == s->cols && rows == s->rows)
		return 0;
	struct reflow f = {0};
	void *block = calloc(1, block_size(cols, rows));
	struct cell *cells[2] = {NULL, NULL};
	uint16_t *maps[2] = {NULL, NULL}, *lens[2] = {NULL, NULL};
	uint8_t *lines[2] = {NULL, NULL};
	uint8_t *tabs = calloc((size_t)cols, 1), *dirt = calloc((size_t)rows, 1);
	int ret = -1, alt = s->alt;
	if (!block || !tabs || !dirt)
		goto out;
	block_layout(block, cols, rows, cells, maps, lens, lines);

	struct saved *sv = &s->grid[0].saved;
	int cx = alt ? sv->x : s->x, cy = alt ? sv->y : s->y;
	int cpend = alt ? sv->pending : s->pending;
	s->alt = 0;

	int cont = 0;
	if (h && h->open && hist_count(h)) {
		struct lline *l = new_line(&f, 0);
		uint8_t fl;
		int k = l ? pull_line(&f, h, &fl) : -1;
		if (k < 0)
			goto out;
		l = &f.l[0];
		l->n = k, l->flags = fl & LINE_PROMPT;
		cont = 1;
	}

	int last = cy;
	for (int y = s->rows - 1; y > last; y--) {
		int used = scr_line(s, y) != 0;
		for (int x = 0; x < s->cols && !used; x++)
			used = !empty_plain_cell(scr_cell(s, x, y));
		if (used)
			last = y;
	}
	size_t cur_line = 0;
	int cur_off = 0;
	for (int y = 0; y <= last; y++) {
		uint8_t lf = scr_line(s, y);
		struct lline *l = cont ? &f.l[f.nl - 1] : new_line(&f, lf);
		if (!l)
			goto out;
		int k = s->cols;
		if (!(lf & LINE_WRAPPED))
			while (k > 0 && empty_plain_cell(scr_cell(s, k - 1, y)))
				k--;
		if (y == cy) {
			int at = cx + (cpend ? 1 : 0);
			if (k < at)
				k = at;
			cur_line = f.nl - 1;
			cur_off = l->n + at;
		}
		if (more((void **)&f.c, &f.size, f.n + (size_t)k, sizeof(*f.c)) < 0)
			goto out;
		int kept = 0;
		for (int x = 0; x < k; x++) {
			const struct cell *c = scr_cell(s, x, y);
			if (c->attr & ATTR_PAD) {
				if (y == cy && x < cx)
					cur_off--;
				continue;
			}
			f.c[f.n + (size_t)kept++] = *c;
		}
		f.n += (size_t)kept;
		l->n += kept;
		cont = lf & LINE_WRAPPED;
	}

	size_t got;
	for (size_t i = 0; i < f.nl; i++)
		if (split_line(&f, i, cols, f.nr, &got) < 0)
			goto out;

	size_t crow = 0;
	int ccol = 0, cpending = 0;
	for (size_t r = 0; r < f.nr; r++) {
		if (f.r[r].line != cur_line)
			continue;
		crow = r;
		if (cur_off < f.r[r].end || r + 1 == f.nr ||
		    f.r[r + 1].line != cur_line)
			break;
	}
	ccol = cur_off - f.r[crow].start;
	if (ccol >= cols)
		ccol = cols - 1, cpending = 1;

	while (h && f.nr < (size_t)rows && hist_count(h)) {
		uint8_t fl;
		size_t start = f.n;
		int k = pull_line(&f, h, &fl);
		if (k < 0 || more((void **)&f.l, &f.lsize, f.nl + 1, sizeof(*f.l)))
			goto out;
		f.l[f.nl] = (struct lline){start, k, (uint8_t)(fl & LINE_PROMPT)};
		if (split_line(&f, f.nl++, cols, 0, &got) < 0)
			goto out;
		crow += got;
	}

	size_t first = f.nr > (size_t)rows ? f.nr - (size_t)rows : 0;
	if (crow < first)
		first = crow;
	struct cell *tmp = malloc((size_t)cols * sizeof(*tmp));
	if (!tmp)
		goto out;
	for (size_t r = 0; r < first; r++) {
		uint8_t fl = row_cells(&f, r, cols, tmp);
		if (h)
			hist_push(h, tmp, cols, fl);
	}
	free(tmp);

	for (int y = 0; y < rows; y++) {
		maps[0][y] = maps[1][y] = (uint16_t)y;
		size_t r = first + (size_t)y;
		if (r < f.nr)
			lines[0][y] = row_cells(&f, r, cols, cells[0] + (size_t)y * cols);
	}
	if (first + (size_t)rows < f.nr)
		lines[0][rows - 1] &= ~LINE_WRAPPED;

	for (int y = 0; y < rows && y < s->rows; y++) {
		s->alt = 1;
		int k = cols < s->cols ? cols : s->cols;
		memcpy(cells[1] + (size_t)y * cols, scr_cell(s, 0, y),
		       (size_t)k * sizeof(struct cell));
		if (k > 0 && (cells[1][(size_t)y * cols + k - 1].attr & ATTR_WIDE))
			cells[1][(size_t)y * cols + k - 1] = (struct cell){0};
		s->alt = 0;
	}

	free(s->block);
	s->block = block, block = NULL;
	for (int i = 0; i < 2; i++) {
		measure(cells[i], lens[i], cols, rows);
		s->grid[i].cells = cells[i];
		s->grid[i].lines = lines[i];
		s->grid[i].map = maps[i];
		s->grid[i].len = lens[i];
	}
	free(s->tabs);
	free(s->dirty);
	s->tabs = tabs, tabs = NULL;
	s->dirty = dirt, dirt = NULL;
	s->cols = cols, s->rows = rows;
	s->top = 0, s->bot = rows - 1;
	reset_tabs(s);
	dirty(s, 0, rows - 1);

	int ny = (int)(crow - first);
	if (alt) {
		sv->x = ccol, sv->y = ny, sv->pending = cpending;
		struct saved *av = &s->grid[1].saved;
		av->x = av->x < cols ? av->x : cols - 1;
		av->y = av->y < rows ? av->y : rows - 1;
		s->x = s->x < cols ? s->x : cols - 1;
		s->y = s->y < rows ? s->y : rows - 1;
		s->pending = 0;
	} else {
		s->x = ccol, s->y = ny, s->pending = cpending;
		for (int i = 0; i < 2; i++) {
			struct saved *v = &s->grid[i].saved;
			v->x = v->x < cols ? v->x : cols - 1;
			v->y = v->y < rows ? v->y : rows - 1;
		}
	}
	ret = 0;
out:
	s->alt = alt;
	free(block);
	free(tabs);
	free(dirt);
	free(f.c);
	free(f.l);
	free(f.r);
	return ret;
}

static void dispatch(void *ctx, const struct vt_event *ev) {
	struct scr *s = ctx;
	switch (ev->kind) {
	case VT_PRINT:
		print(s, ev->cp);
		break;
	case VT_PRINT_RUN:
		print_run(s, ev->str, ev->len);
		break;
	case VT_EXECUTE:
		execute(s, ev->cp);
		break;
	case VT_CSI:
		csi(s, ev->seq);
		break;
	case VT_ESC:
		esc(s, ev->seq);
		break;
	case VT_OSC:
		osc(s, ev->str, ev->len);
		break;
	case VT_DCS:
		dcs(s, ev->seq, ev->str, ev->len);
		break;
	case VT_APC:
		break;
	}
}

void scr_feed(struct scr *s, const uint8_t *buf, size_t len) {
	vt_feed(&s->vt, buf, len, dispatch, s);
}
