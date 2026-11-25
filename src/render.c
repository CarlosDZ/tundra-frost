#include "render.h"

#include <stdio.h>
#include <string.h>

#define PAL_FG 256
#define PAL_BG 257
#define PAL_CURSOR 258

static const uint32_t base16[16] = {
    0x1f2529, 0xff1712, 0x65c55b, 0xd2a408, 0x74acfe, 0xe57fdd,
    0x11c2ca, 0x889196, 0x3e494f, 0xff3e2f, 0x89dc80, 0xecbd3a,
    0xa0c6ff, 0xfa9ff2, 0x10dee7, 0xe2e6e9,
};
#define DEFAULT_BG 0x141a1e
#define DEFAULT_TEXT 0xccd2d6

static uint32_t opaque(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFFFFFF); }

static uint32_t theme_pick(const struct tdraw_theme *t, const char *own,
                           const char *common, uint32_t fallback) {
	if (!t)
		return opaque(fallback);
	uint32_t c = tdraw_theme_color(t, common, opaque(fallback));
	return own ? tdraw_theme_color(t, own, c) : c;
}

void render_theme(struct render *r, const struct tdraw_theme *t) {
	static const uint8_t level[6] = {0, 95, 135, 175, 215, 255};
	char name[16];
	for (int i = 0; i < 16; i++) {
		snprintf(name, sizeof(name), "color%d", i);
		r->theme[i] = theme_pick(t, NULL, name, base16[i]);
	}
	for (int i = 16; i < 232; i++) {
		int k = i - 16;
		r->theme[i] = opaque((uint32_t)level[k / 36] << 16 |
		                     (uint32_t)level[k / 6 % 6] << 8 | level[k % 6]);
	}
	for (int i = 232; i < 256; i++) {
		uint32_t g = (uint32_t)(8 + 10 * (i - 232));
		r->theme[i] = opaque(g << 16 | g << 8 | g);
	}
	r->theme[PAL_FG] = theme_pick(t, "frost.text", "text", DEFAULT_TEXT);
	r->theme[PAL_BG] =
	    theme_pick(t, "frost.background", "background", DEFAULT_BG);
	r->theme[PAL_CURSOR] = theme_pick(t, "frost.cursor", "text", DEFAULT_TEXT);
}

int render_init(struct render *r, int size) {
	memset(r, 0, sizeof(*r));
	const char *err;
	r->font = tdraw_font_open("mono", size, &err);
	if (!r->font) {
		fprintf(stderr, "frost: no mono font: %s\n", err);
		return -1;
	}
	r->bold = tdraw_font_open("mono.bold", size, &err);
	struct tdraw_font_metrics m;
	tdraw_font_metrics(r->font, &m);
	if (m.cell <= 0) {
		fprintf(stderr, "frost: the first mono font is not monospaced\n");
		render_free(r);
		return -1;
	}
	if (r->bold) {
		struct tdraw_font_metrics b;
		tdraw_font_metrics(r->bold, &b);
		if (b.cell != m.cell || b.height != m.height) {
			tdraw_font_free(r->bold);
			r->bold = NULL;
		}
	}
	r->cw = m.cell, r->ch = m.height, r->ascent = m.ascent;
	render_theme(r, NULL);
	return 0;
}

void render_free(struct render *r) {
	tdraw_font_free(r->font);
	tdraw_font_free(r->bold);
	memset(r, 0, sizeof(*r));
}

void render_fit(struct render *r, int width, int height, int *cols, int *rows) {
	*cols = width / r->cw > 0 ? width / r->cw : 1;
	*rows = height / r->ch > 0 ? height / r->ch : 1;
	r->width = width, r->height = height;
	r->ox = (width - *cols * r->cw) / 2;
	r->oy = (height - *rows * r->ch) / 2;
	if (r->ox < 0)
		r->ox = 0;
	if (r->oy < 0)
		r->oy = 0;
}

static uint32_t slot(const struct render *r, const struct view *v, int i) {
	return v->palette_set && v->palette_set[i] ? opaque(v->palette[i])
	                                           : r->theme[i];
}

uint32_t render_color(const struct render *r, const struct view *v,
                      uint32_t color, int fg) {
	switch (COLOR_KIND(color)) {
	case 2:
		return opaque(color);
	case 1:
		return slot(r, v, (int)(color & 0xFF));
	}
	int s = fg ? PAL_FG : PAL_BG;
	if (v->reverse)
		s = fg ? PAL_BG : PAL_FG;
	return slot(r, v, s);
}

static uint32_t mix(uint32_t a, uint32_t b) {
	uint32_t out = 0xFF000000u;
	for (int sh = 0; sh < 24; sh += 8)
		out |= ((a >> sh & 0xFF) + (b >> sh & 0xFF)) / 2 << sh;
	return out;
}

static void colors(const struct render *r, const struct view *v,
                   const struct cell *c, uint32_t *fg, uint32_t *bg) {
	*fg = render_color(r, v, c->fg, 1);
	*bg = render_color(r, v, c->bg, 0);
	if (c->attr & ATTR_INVERSE) {
		uint32_t t = *fg;
		*fg = *bg;
		*bg = t;
	}
	if (c->attr & ATTR_DIM)
		*fg = mix(*fg, *bg);
	if (c->attr & ATTR_HIDDEN)
		*fg = *bg;
}

static size_t utf8(char *p, uint32_t cp) {
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

void render_margins(struct render *r, struct tdraw_canvas *c,
                    const struct view *v) {
	uint32_t bg = render_color(r, v, COLOR_DEFAULT, 0);
	int gw = v->cols * r->cw, gh = v->rows * r->ch;
	tdraw_fill(c, 0, 0, r->width, r->oy, bg);
	tdraw_fill(c, 0, r->oy + gh, r->width, r->height - r->oy - gh, bg);
	tdraw_fill(c, 0, r->oy, r->ox, gh, bg);
	tdraw_fill(c, r->ox + gw, r->oy, r->width - r->ox - gw, gh, bg);
}

static struct tdraw_font *font_of(const struct render *r,
                                  const struct cell *c) {
	return (c->attr & ATTR_BOLD) && r->bold ? r->bold : r->font;
}

static void glyph(struct render *r, struct tdraw_canvas *c,
                  const struct cell *cl, int px, int py, uint32_t color) {
	char b[4];
	struct tdraw_font *f = font_of(r, cl);
	tdraw_text(c, f, px, py, b, utf8(b, cl->cp), color);
	for (int i = 0; i < CELL_COMB && cl->comb[i]; i++)
		tdraw_text(c, f, px, py, b, utf8(b, cl->comb[i]), color);
}

static void decorate(struct render *r, struct tdraw_canvas *c,
                     const struct cell *cl, int px, int py, uint32_t fg,
                     const struct view *v) {
	int t = r->ch / 16 > 0 ? r->ch / 16 : 1;
	int w = (cl->attr & ATTR_WIDE) ? 2 * r->cw : r->cw;
	unsigned ul = (cl->attr & ATTR_UL_MASK) >> ATTR_UL_SHIFT;
	if (ul) {
		uint32_t col = cl->ul ? render_color(r, v, cl->ul, 1) : fg;
		int uy = py + r->ascent + t;
		if (uy + 3 * t > py + r->ch)
			uy = py + r->ch - 3 * t;
		switch (ul) {
		case UL_DOUBLE:
			tdraw_fill(c, px, uy, w, t, col);
			tdraw_fill(c, px, uy + 2 * t, w, t, col);
			break;
		case UL_CURLY: {
			float xy[2 * 5];
			for (int i = 0; i < 5; i++) {
				xy[2 * i] = (float)px + (float)w * (float)i / 4.0f;
				xy[2 * i + 1] =
				    (float)uy + (i % 2 ? 2.0f * (float)t : 0.0f) + 0.5f;
			}
			struct tdraw_pen pen = {col, (float)t, 1};
			tdraw_polyline(c, xy, 5, &pen);
			break;
		}
		case UL_DOTTED:
			for (int x = 0; x < w; x += 2 * t)
				tdraw_fill(c, px + x, uy, t, t, col);
			break;
		case UL_DASHED:
			for (int x = 0; x < w; x += r->cw)
				tdraw_fill(c, px + x, uy, (r->cw + 1) / 2, t, col);
			break;
		default:
			tdraw_fill(c, px, uy, w, t, col);
		}
	}
	if (cl->attr & ATTR_STRIKE)
		tdraw_fill(c, px, py + r->ascent - r->ascent / 3, w, t, fg);
	if (cl->attr & ATTR_OVERLINE)
		tdraw_fill(c, px, py, w, t, fg);
}

void render_row(struct render *r, struct tdraw_canvas *c, const struct view *v,
                int y) {
	const struct cell *row = v->cells + (size_t)v->map[y] * v->cols;
	int py = r->oy + y * r->ch;
	uint32_t fg, bg, fg2, bg2;

	for (int x = 0; x < v->cols;) {
		colors(r, v, &row[x], &fg, &bg);
		int k = x + 1;
		while (k < v->cols) {
			colors(r, v, &row[k], &fg2, &bg2);
			if (bg2 != bg)
				break;
			k++;
		}
		tdraw_fill(c, r->ox + x * r->cw, py, (k - x) * r->cw, r->ch, bg);
		x = k;
	}

	char buf[1024];
	for (int x = 0; x < v->cols;) {
		const struct cell *cl = &row[x];
		if (!cl->cp || (cl->attr & (ATTR_WIDE_TAIL | ATTR_HIDDEN))) {
			x++;
			continue;
		}
		colors(r, v, cl, &fg, &bg);
		if ((cl->attr & ATTR_WIDE) || cl->comb[0]) {
			glyph(r, c, cl, r->ox + x * r->cw, py, fg);
			x += (cl->attr & ATTR_WIDE) ? 2 : 1;
			continue;
		}
		struct tdraw_font *f = font_of(r, cl);
		int start = x;
		size_t n = 0;
		while (x < v->cols && n + 4 <= sizeof(buf)) {
			const struct cell *d = &row[x];
			if (!d->cp || d->comb[0] ||
			    (d->attr & (ATTR_WIDE | ATTR_WIDE_TAIL | ATTR_HIDDEN)) ||
			    font_of(r, d) != f)
				break;
			colors(r, v, d, &fg2, &bg2);
			if (fg2 != fg)
				break;
			n += utf8(buf + n, d->cp);
			x++;
		}
		tdraw_text(c, f, r->ox + start * r->cw, py, buf, n, fg);
	}

	for (int x = 0; x < v->cols; x++) {
		const struct cell *cl = &row[x];
		if (!(cl->attr & (ATTR_UL_MASK | ATTR_STRIKE | ATTR_OVERLINE)) ||
		    (cl->attr & ATTR_WIDE_TAIL))
			continue;
		colors(r, v, cl, &fg, &bg);
		decorate(r, c, cl, r->ox + x * r->cw, py, fg, v);
	}
}

void render_cursor(struct render *r, struct tdraw_canvas *c,
                   const struct view *v, int focused, int lit) {
	if (!v->cursor_visible || (v->cursor_blink && focused && !lit))
		return;
	int x = v->x, y = v->y;
	if (x < 0 || x >= v->cols || y < 0 || y >= v->rows)
		return;
	const struct cell *row = v->cells + (size_t)v->map[y] * v->cols;
	if ((row[x].attr & ATTR_WIDE_TAIL) && x > 0)
		x--;
	const struct cell *cl = &row[x];
	int w = (cl->attr & ATTR_WIDE) ? 2 * r->cw : r->cw;
	int px = r->ox + x * r->cw, py = r->oy + y * r->ch;
	int t = r->ch / 16 > 0 ? r->ch / 16 : 1;
	uint32_t color = slot(r, v, PAL_CURSOR);
	int shape =
	    v->cursor_shape == CURSOR_DEFAULT ? CURSOR_BAR : v->cursor_shape;

	switch (shape) {
	case CURSOR_BLOCK:
		if (!focused) {
			struct tdraw_pen pen = {color, 1.0f, 0};
			tdraw_rect(c, (float)px + 0.5f, (float)py + 0.5f, (float)w - 1.0f,
			           (float)r->ch - 1.0f, 0.0f, &pen);
			break;
		}
		tdraw_fill(c, px, py, w, r->ch, color);
		if (cl->cp && !(cl->attr & ATTR_HIDDEN)) {
			uint32_t fg, bg;
			colors(r, v, cl, &fg, &bg);
			glyph(r, c, cl, px, py, bg);
		}
		break;
	case CURSOR_UNDERLINE:
		tdraw_fill(c, px, py + r->ch - 2 * t, w, 2 * t, color);
		break;
	default:
		tdraw_fill(c, px, py, r->cw / 8 > 0 ? r->cw / 8 : 1, r->ch, color);
	}
}
