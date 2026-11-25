#include "keys.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PLAIN_MODS                                                             \
	(MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_SUPER | MOD_HYPER | MOD_META)
#define LOCKS (MOD_CAPS_LOCK | MOD_NUM_LOCK)
#define OTHER_MODS (MOD_SUPER | MOD_HYPER | MOD_META)

struct out {
	char *p;
	size_t n;
};

static void put(struct out *o, const char *s, size_t len) {
	if (o->n + len <= KEY_OUT_MAX) {
		memcpy(o->p + o->n, s, len);
		o->n += len;
	}
}

static void puts_(struct out *o, const char *s) { put(o, s, strlen(s)); }

static void putf(struct out *o, const char *fmt, ...) {
	char buf[64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0 && (size_t)n < sizeof(buf))
		put(o, buf, (size_t)n);
}

struct form {
	uint32_t code;
	int number;
	char final;
};

static const struct form forms[] = {
    {KEY_INSERT, 2, '~'},    {KEY_DELETE, 3, '~'},   {KEY_PAGE_UP, 5, '~'},
    {KEY_PAGE_DOWN, 6, '~'}, {KEY_UP, 1, 'A'},       {KEY_DOWN, 1, 'B'},
    {KEY_RIGHT, 1, 'C'},     {KEY_LEFT, 1, 'D'},     {KEY_HOME, 1, 'H'},
    {KEY_END, 1, 'F'},       {KEY_KP_BEGIN, 1, 'E'}, {KEY_F(1), 1, 'P'},
    {KEY_F(2), 1, 'Q'},      {KEY_F(3), 13, '~'},    {KEY_F(4), 1, 'S'},
    {KEY_F(5), 15, '~'},     {KEY_F(6), 17, '~'},    {KEY_F(7), 18, '~'},
    {KEY_F(8), 19, '~'},     {KEY_F(9), 20, '~'},    {KEY_F(10), 21, '~'},
    {KEY_F(11), 23, '~'},    {KEY_F(12), 24, '~'},
};

static const struct form *form_of(uint32_t c) {
	for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
		if (forms[i].code == c)
			return &forms[i];
	return NULL;
}

static int is_keypad(uint32_t c) { return c >= KEY_KP_0 && c <= KEY_KP_BEGIN; }

static int is_modifier(uint32_t c) {
	return (c >= KEY_LEFT_SHIFT && c <= KEY_ISO_LEVEL5_SHIFT) ||
	       c == KEY_CAPS_LOCK || c == KEY_SCROLL_LOCK || c == KEY_NUM_LOCK;
}

static uint32_t off_keypad(uint32_t c) {
	switch (c) {
	case KEY_KP_ENTER:
		return KEY_ENTER;
	case KEY_KP_LEFT:
		return KEY_LEFT;
	case KEY_KP_RIGHT:
		return KEY_RIGHT;
	case KEY_KP_UP:
		return KEY_UP;
	case KEY_KP_DOWN:
		return KEY_DOWN;
	case KEY_KP_PAGE_UP:
		return KEY_PAGE_UP;
	case KEY_KP_PAGE_DOWN:
		return KEY_PAGE_DOWN;
	case KEY_KP_HOME:
		return KEY_HOME;
	case KEY_KP_END:
		return KEY_END;
	case KEY_KP_INSERT:
		return KEY_INSERT;
	case KEY_KP_DELETE:
		return KEY_DELETE;
	}
	return c;
}

static uint32_t number_of(uint32_t c) {
	switch (c) {
	case KEY_ESCAPE:
		return 27;
	case KEY_ENTER:
		return 13;
	case KEY_TAB:
		return 9;
	case KEY_BACKSPACE:
		return 127;
	}
	return c;
}

static int writes_text(const char *t) {
	unsigned char c = (unsigned char)t[0];
	return c >= 0x20 && c != 0x7f;
}

static int is_text_key(const struct key *k) {
	return k->code < KEY_ESCAPE || (is_keypad(k->code) && writes_text(k->text));
}

static void codepoints(const char *t, char *buf, size_t size) {
	const unsigned char *p = (const unsigned char *)t;
	size_t n = 0;
	buf[0] = 0;
	while (*p) {
		uint32_t cp;
		int len = *p < 0x80 ? 1 : *p >> 5 == 6 ? 2 : *p >> 4 == 14 ? 3 : 4;
		cp = len == 1   ? *p
		     : len == 2 ? *p & 0x1F
		     : len == 3 ? *p & 0x0F
		                : *p & 7;
		for (int i = 1; i < len; i++) {
			if ((p[i] & 0xC0) != 0x80) {
				len = i;
				cp = 0xFFFD;
				break;
			}
			cp = cp << 6 | (p[i] & 0x3F);
		}
		p += len;
		if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
			continue;
		int w = snprintf(buf + n, size - n, "%s%u", n ? ":" : "", cp);
		if (w < 0 || (size_t)w >= size - n)
			break;
		n += (size_t)w;
	}
}

static void csi(struct out *o, uint32_t number, const char *alts, unsigned mods,
                int type, const char *text, char final) {
	int show = mods || type != KEY_PRESS;
	if (final != 'u' && final != '~' && number == 1 && !show) {
		putf(o, "\x1b[%c", final);
		return;
	}
	putf(o, "\x1b[%u", number);
	puts_(o, alts);
	if (show || *text) {
		puts_(o, ";");
		if (show)
			putf(o, "%u", mods + 1);
		if (type != KEY_PRESS)
			putf(o, ":%d", type);
	}
	if (*text) {
		puts_(o, ";");
		puts_(o, text);
	}
	putf(o, "%c", final);
}

static void legacy_enter(struct out *o, const struct key_modes *m) {
	puts_(o, m->newline ? "\r\n" : "\r");
}

static int ctrl_byte(uint32_t c, unsigned char *b) {
	static const char from[] = " /0123456789?@[\\]^_~";
	static const unsigned char to[] = {0,  31, 48, 49,  0,  27,  28,
	                                   29, 30, 31, 127, 57, 127, 0,
	                                   27, 28, 29, 30,  31, 30};
	if (c >= 'a' && c <= 'z')
		*b = (unsigned char)(c - 'a' + 1);
	else if (c >= 'A' && c <= 'Z')
		*b = (unsigned char)(c - 'A' + 1);
	else if (c && c < 128 && strchr(from, (int)c))
		*b = to[strchr(from, (int)c) - from];
	else if (c && c < 128)
		*b = (unsigned char)c;
	else
		return 0;
	return 1;
}

static void legacy(const struct key *k, const struct key_modes *m,
                   struct out *o) {
	unsigned mods = k->mods & PLAIN_MODS;
	uint32_t c = k->code;
	const char *text = k->text;
	int alt = mods & MOD_ALT;
	if (k->type == KEY_RELEASE || is_modifier(c))
		return;
	if (!c) {
		puts_(o, text);
		return;
	}
	if (is_keypad(c))
		c = writes_text(text) ? (uint32_t)(unsigned char)text[0]
		                      : off_keypad(c);

	if (!(mods & OTHER_MODS)) {
		const char *pre = alt ? "\x1b" : "";
		switch (c) {
		case KEY_ENTER:
			puts_(o, pre);
			legacy_enter(o, m);
			return;
		case KEY_ESCAPE:
			puts_(o, pre);
			puts_(o, "\x1b");
			return;
		case KEY_BACKSPACE:
			puts_(o, pre);
			puts_(o, mods & MOD_CTRL ? "\b" : "\x7f");
			return;
		case KEY_TAB:
			puts_(o, pre);
			puts_(o, mods & MOD_SHIFT ? "\x1b[Z" : "\t");
			return;
		}
	}

	if (c == KEY_MENU) {
		csi(o, 29, "", mods, KEY_PRESS, "", '~');
		return;
	}
	const struct form *f = form_of(c);
	if (f) {
		int ss3 =
		    !mods &&
		    (c == KEY_F(1) || c == KEY_F(2) || c == KEY_F(3) || c == KEY_F(4) ||
		     (m->app_cursor && f->number == 1 && c != KEY_KP_BEGIN));
		static const struct form f3 = {KEY_F(3), 1, 'R'};
		if (c == KEY_F(3))
			f = &f3;
		if (ss3)
			putf(o, "\x1bO%c", f->final);
		else
			csi(o, (uint32_t)f->number, "", mods, KEY_PRESS, "", f->final);
		return;
	}

	if (c < KEY_ESCAPE) {
		unsigned char b = 0;
		uint32_t ascii = c < 128 ? c : k->base < 128 ? k->base : 0;
		int ctrl = mods & MOD_CTRL;
		if ((mods & OTHER_MODS) || (ctrl && (mods & MOD_SHIFT) && c != ' ') ||
		    (ctrl && !ctrl_byte(ascii, &b))) {
			csi(o, c, "", mods, KEY_PRESS, "", 'u');
			return;
		}
		if (!ctrl && !*text)
			return;
		if (alt)
			puts_(o, "\x1b");
		if (ctrl)
			put(o, (const char *)&b, 1);
		else
			puts_(o, text);
		return;
	}
	csi(o, number_of(c), "", mods, KEY_PRESS, "", 'u');
}

static void enhanced(const struct key *k, const struct key_modes *m,
                     struct out *o) {
	unsigned f = m->flags;
	int all = f & KEYS_ALL, type = k->type;
	unsigned plain = k->mods & PLAIN_MODS;
	uint32_t c = k->code;
	const char *text = k->text;
	if (!(f & KEYS_EVENT_TYPES)) {
		if (type == KEY_RELEASE)
			return;
		type = KEY_PRESS;
	}
	if (is_modifier(c)) {
		if (all)
			csi(o, c, "", k->mods, type, "", 'u');
		return;
	}
	if (!c) {
		char cps[KEY_OUT_MAX / 2];
		codepoints(text, cps, sizeof(cps));
		if (type == KEY_RELEASE)
			return;
		if (all && (f & KEYS_TEXT) && *cps)
			csi(o, 0, "", 0, KEY_PRESS, cps, 'u');
		else if (!all || !(f & KEYS_TEXT))
			puts_(o, text);
		return;
	}
	int text_key = is_text_key(k);
	if (!all) {
		if (c == KEY_ENTER || c == KEY_TAB || c == KEY_BACKSPACE) {
			if (type == KEY_RELEASE)
				return;
			if (!plain) {
				if (c == KEY_ENTER)
					legacy_enter(o, m);
				else
					puts_(o, c == KEY_TAB ? "\t" : "\x7f");
				return;
			}
		} else if (text_key && !(plain & ~MOD_SHIFT)) {
			if (type != KEY_RELEASE)
				puts_(o, text);
			return;
		}
	}

	unsigned mods = k->mods;
	if (!all && text_key)
		mods &= ~LOCKS;
	const struct form *fm = form_of(c);
	if (fm) {
		csi(o, (uint32_t)fm->number, "", mods, type, "", fm->final);
		return;
	}

	char alts[32] = "";
	if ((f & KEYS_ALTERNATES) && c < KEY_ESCAPE) {
		uint32_t s = (k->mods & MOD_SHIFT) && k->shifted != c ? k->shifted : 0;
		uint32_t b = k->base != c ? k->base : 0;
		if (s && b)
			snprintf(alts, sizeof(alts), ":%u:%u", s, b);
		else if (s)
			snprintf(alts, sizeof(alts), ":%u", s);
		else if (b)
			snprintf(alts, sizeof(alts), "::%u", b);
	}
	char cps[KEY_OUT_MAX / 2] = "";
	if (all && (f & KEYS_TEXT) && type != KEY_RELEASE && !(plain & ~MOD_SHIFT))
		codepoints(text, cps, sizeof(cps));
	csi(o, number_of(c), alts, mods, type, cps, 'u');
}

size_t key_encode(const struct key *k, const struct key_modes *m, char *out) {
	struct out o = {out, 0};
	struct key key = *k;
	if (!key.text)
		key.text = "";
	if (m->flags)
		enhanced(&key, m, &o);
	else
		legacy(&key, m, &o);
	return o.n;
}
