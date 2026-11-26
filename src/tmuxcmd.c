#include "tmuxcmd.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "screen.h"

struct spec {
	const char *name, *alias, *flags, *values;
};

static const struct spec specs[] = {
    {"display-message", "display", "aIlNpv", "cdt"},
    {"has-session", "has", "", "t"},
    {"kill-pane", "killp", "a", "t"},
    {"kill-server", "kill-server", "", ""},
    {"kill-session", "kill-ses", "aC", "t"},
    {"kill-window", "killw", "a", "t"},
    {"list-panes", "lsp", "as", "Fft"},
    {"list-sessions", "ls", "", "Ff"},
    {"list-windows", "lsw", "a", "Fft"},
    {"new-session", "new", "AdDEPX", "ceFfnstxy"},
    {"new-window", "neww", "abdkPS", "ceFnt"},
    {"resize-pane", "resizep", "DLMRTUZ", "txy"},
    {"respawn-pane", "respawnp", "k", "cet"},
    {"select-layout", "selectl", "Enop", "t"},
    {"select-pane", "selectp", "DdeLlMmRUZ", "Tt"},
    {"select-window", "selectw", "lnpT", "t"},
    {"set-option", "set", "aFgopqsuUw", "t"},
    {"set-window-option", "setw", "aFgoqu", "t"},
    {"show-options", "show", "AgHpqsvw", "t"},
    {"split-window", "splitw", "bdfhIvPZ", "ceFlt"},
};

static int slot(char l) {
	return l >= 'a' && l <= 'z'   ? l - 'a'
	       : l >= 'A' && l <= 'Z' ? l - 'A' + 26
	                              : -1;
}

int tmux_flag(const struct tmux_cmd *c, char letter) {
	int i = slot(letter);
	return i >= 0 && (c->flags >> i & 1);
}

const char *tmux_value(const struct tmux_cmd *c, char letter) {
	int i = slot(letter);
	return i >= 0 ? c->value[i] : NULL;
}

int tmux_parse(int argc, char **argv, struct tmux_cmd *c) {
	memset(c, 0, sizeof(*c));
	if (argc < 1)
		return -1;
	const struct spec *sp = NULL;
	for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]) && !sp; i++)
		if (!strcmp(argv[0], specs[i].name) || !strcmp(argv[0], specs[i].alias))
			sp = &specs[i];
	if (!sp)
		return -1;
	c->name = sp->name;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			int s = slot(*p);
			if (s < 0)
				return -1;
			if (strchr(sp->values, *p)) {
				const char *v = p[1] ? p + 1 : i + 1 < argc ? argv[++i] : NULL;
				if (!v)
					return -1;
				c->value[s] = v;
				c->flags |= (uint64_t)1 << s;
				break;
			}
			if (!strchr(sp->flags, *p))
				return -1;
			c->flags |= (uint64_t)1 << s;
		}
	}
	c->argc = argc - i;
	c->argv = argv + i;
	return 0;
}

static void walk(const char *fmt, tmux_var_fn var, void *ctx,
                 void (*text)(void *w, const char *s, size_t n),
                 void (*style)(void *w, const char *s, size_t n), void *w) {
	const char *p = fmt;
	while (*p) {
		if (p[0] == '#' && p[1] == '#') {
			text(w, "#", 1);
			p += 2;
		} else if (p[0] == '#' && (p[1] == '{' || p[1] == '[')) {
			char close = p[1] == '{' ? '}' : ']';
			const char *end = strchr(p + 2, close);
			if (!end) {
				text(w, p, strlen(p));
				return;
			}
			char name[128];
			size_t n = (size_t)(end - (p + 2));
			if (n >= sizeof(name))
				n = sizeof(name) - 1;
			memcpy(name, p + 2, n);
			name[n] = 0;
			if (close == '}') {
				const char *v = var ? var(ctx, name) : NULL;
				if (v)
					text(w, v, strlen(v));
			} else if (style) {
				style(w, name, n);
			}
			p = end + 1;
		} else {
			const char *q = p;
			while (*q && *q != '#')
				q++;
			if (q == p)
				q++;
			text(w, p, (size_t)(q - p));
			p = q;
		}
	}
}

struct flat {
	char *out;
	size_t max, n;
};

static void flat_text(void *w, const char *s, size_t n) {
	struct flat *f = w;
	for (size_t i = 0; i < n && f->n + 1 < f->max; i++)
		f->out[f->n++] = s[i];
}

size_t tmux_format(const char *fmt, tmux_var_fn var, void *ctx, char *out,
                   size_t max) {
	struct flat f = {out, max, 0};
	if (!max)
		return 0;
	walk(fmt, var, ctx, flat_text, NULL, &f);
	out[f.n] = 0;
	return f.n;
}

static uint32_t colour(const char *s, size_t n) {
	static const char *const names[8] = {"black", "red",     "green", "yellow",
	                                     "blue",  "magenta", "cyan",  "white"};
	char w[32];
	if (n >= sizeof(w))
		return COLOR_DEFAULT;
	memcpy(w, s, n);
	w[n] = 0;
	int bright = !strncasecmp(w, "bright", 6);
	for (int i = 0; i < 8; i++)
		if (!strcasecmp(w + (bright ? 6 : 0), names[i]))
			return COLOR_INDEX(i + (bright ? 8 : 0));
	if (!strncasecmp(w, "colour", 6) || !strncasecmp(w, "color", 5)) {
		int k = atoi(w + (w[5] == 'r' ? 6 : 5));
		return k >= 0 && k < 256 ? COLOR_INDEX(k) : COLOR_DEFAULT;
	}
	if (w[0] == '#' && n == 7) {
		unsigned long v = strtoul(w + 1, NULL, 16);
		return COLOR_RGB(v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF);
	}
	return COLOR_DEFAULT;
}

void tmux_style(const char *spec, const struct tmux_style *base,
                struct tmux_style *st) {
	const char *p = spec;
	while (*p) {
		const char *end = p;
		while (*end && *end != ',' && *end != ' ')
			end++;
		size_t n = (size_t)(end - p);
		if (n == 7 && !strncmp(p, "default", 7))
			*st = *base;
		else if (n == 4 && !strncmp(p, "bold", 4))
			st->bold = 1;
		else if (n == 6 && !strncmp(p, "nobold", 6))
			st->bold = 0;
		else if (n > 3 && !strncmp(p, "fg=", 3))
			st->fg = !strncmp(p + 3, "default", n - 3) && n == 10
			             ? base->fg
			             : colour(p + 3, n - 3);
		p = *end ? end + 1 : end;
	}
}

struct styled {
	struct tmux_style st;
	const struct tmux_style *base;
	tmux_piece_fn piece;
	void *ctx;
};

static void styled_text(void *w, const char *s, size_t n) {
	struct styled *x = w;
	x->piece(x->ctx, s, n, &x->st);
}

static void styled_style(void *w, const char *s, size_t n) {
	struct styled *x = w;
	char spec[128];
	if (n >= sizeof(spec))
		n = sizeof(spec) - 1;
	memcpy(spec, s, n);
	spec[n] = 0;
	tmux_style(spec, x->base, &x->st);
}

void tmux_format_styled(const char *fmt, const struct tmux_style *base,
                        tmux_var_fn var, void *vctx, tmux_piece_fn piece,
                        void *pctx) {
	struct styled x = {*base, base, piece, pctx};
	walk(fmt, var, vctx, styled_text, styled_style, &x);
}
