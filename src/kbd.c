#include "kbd.h"

#include <stdlib.h>
#include <string.h>

uint32_t kbd_keysym_code(xkb_keysym_t s) {
	if (s >= XKB_KEY_F1 && s <= XKB_KEY_F35)
		return FKEY_F1 + (s - XKB_KEY_F1);
	if (s >= XKB_KEY_KP_0 && s <= XKB_KEY_KP_9)
		return FKEY_KP_0 + (s - XKB_KEY_KP_0);
	switch (s) {
	case XKB_KEY_Escape:
		return FKEY_ESCAPE;
	case XKB_KEY_Return:
		return FKEY_ENTER;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
		return FKEY_TAB;
	case XKB_KEY_BackSpace:
		return FKEY_BACKSPACE;
	case XKB_KEY_Insert:
		return FKEY_INSERT;
	case XKB_KEY_Delete:
		return FKEY_DELETE;
	case XKB_KEY_Left:
		return FKEY_LEFT;
	case XKB_KEY_Right:
		return FKEY_RIGHT;
	case XKB_KEY_Up:
		return FKEY_UP;
	case XKB_KEY_Down:
		return FKEY_DOWN;
	case XKB_KEY_Page_Up:
		return FKEY_PAGE_UP;
	case XKB_KEY_Page_Down:
		return FKEY_PAGE_DOWN;
	case XKB_KEY_Home:
		return FKEY_HOME;
	case XKB_KEY_End:
		return FKEY_END;
	case XKB_KEY_Caps_Lock:
		return FKEY_CAPS_LOCK;
	case XKB_KEY_Scroll_Lock:
		return FKEY_SCROLL_LOCK;
	case XKB_KEY_Num_Lock:
		return FKEY_NUM_LOCK;
	case XKB_KEY_Print:
		return FKEY_PRINT_SCREEN;
	case XKB_KEY_Pause:
		return FKEY_PAUSE;
	case XKB_KEY_Menu:
		return FKEY_MENU;
	case XKB_KEY_KP_Decimal:
		return FKEY_KP_DECIMAL;
	case XKB_KEY_KP_Divide:
		return FKEY_KP_DIVIDE;
	case XKB_KEY_KP_Multiply:
		return FKEY_KP_MULTIPLY;
	case XKB_KEY_KP_Subtract:
		return FKEY_KP_SUBTRACT;
	case XKB_KEY_KP_Add:
		return FKEY_KP_ADD;
	case XKB_KEY_KP_Enter:
		return FKEY_KP_ENTER;
	case XKB_KEY_KP_Equal:
		return FKEY_KP_EQUAL;
	case XKB_KEY_KP_Separator:
		return FKEY_KP_SEPARATOR;
	case XKB_KEY_KP_Left:
		return FKEY_KP_LEFT;
	case XKB_KEY_KP_Right:
		return FKEY_KP_RIGHT;
	case XKB_KEY_KP_Up:
		return FKEY_KP_UP;
	case XKB_KEY_KP_Down:
		return FKEY_KP_DOWN;
	case XKB_KEY_KP_Page_Up:
		return FKEY_KP_PAGE_UP;
	case XKB_KEY_KP_Page_Down:
		return FKEY_KP_PAGE_DOWN;
	case XKB_KEY_KP_Home:
		return FKEY_KP_HOME;
	case XKB_KEY_KP_End:
		return FKEY_KP_END;
	case XKB_KEY_KP_Insert:
		return FKEY_KP_INSERT;
	case XKB_KEY_KP_Delete:
		return FKEY_KP_DELETE;
	case XKB_KEY_KP_Begin:
		return FKEY_KP_BEGIN;
	case XKB_KEY_XF86AudioPlay:
		return FKEY_MEDIA_PLAY;
	case XKB_KEY_XF86AudioPause:
		return FKEY_MEDIA_PLAY + 1;
	case XKB_KEY_XF86AudioStop:
		return FKEY_MEDIA_PLAY + 4;
	case XKB_KEY_XF86AudioForward:
		return FKEY_MEDIA_PLAY + 5;
	case XKB_KEY_XF86AudioRewind:
		return FKEY_MEDIA_PLAY + 6;
	case XKB_KEY_XF86AudioNext:
		return FKEY_MEDIA_PLAY + 7;
	case XKB_KEY_XF86AudioPrev:
		return FKEY_MEDIA_PLAY + 8;
	case XKB_KEY_XF86AudioRecord:
		return FKEY_MEDIA_PLAY + 9;
	case XKB_KEY_XF86AudioLowerVolume:
		return FKEY_MEDIA_PLAY + 10;
	case XKB_KEY_XF86AudioRaiseVolume:
		return FKEY_MEDIA_PLAY + 11;
	case XKB_KEY_XF86AudioMute:
		return FKEY_MUTE_VOLUME;
	case XKB_KEY_Shift_L:
		return FKEY_LEFT_SHIFT;
	case XKB_KEY_Control_L:
		return FKEY_LEFT_CONTROL;
	case XKB_KEY_Alt_L:
		return FKEY_LEFT_ALT;
	case XKB_KEY_Super_L:
		return FKEY_LEFT_SUPER;
	case XKB_KEY_Hyper_L:
		return FKEY_LEFT_HYPER;
	case XKB_KEY_Meta_L:
		return FKEY_LEFT_META;
	case XKB_KEY_Shift_R:
		return FKEY_RIGHT_SHIFT;
	case XKB_KEY_Control_R:
		return FKEY_RIGHT_CONTROL;
	case XKB_KEY_Alt_R:
		return FKEY_RIGHT_ALT;
	case XKB_KEY_Super_R:
		return FKEY_RIGHT_SUPER;
	case XKB_KEY_Hyper_R:
		return FKEY_RIGHT_HYPER;
	case XKB_KEY_Meta_R:
		return FKEY_RIGHT_META;
	case XKB_KEY_ISO_Level3_Shift:
		return FKEY_ISO_LEVEL3_SHIFT;
	case XKB_KEY_ISO_Level5_Shift:
		return FKEY_ISO_LEVEL5_SHIFT;
	}
	return 0;
}

static unsigned mod_of(uint32_t code) {
	switch (code) {
	case FKEY_LEFT_SHIFT:
	case FKEY_RIGHT_SHIFT:
		return MOD_SHIFT;
	case FKEY_LEFT_CONTROL:
	case FKEY_RIGHT_CONTROL:
		return MOD_CTRL;
	case FKEY_LEFT_ALT:
	case FKEY_RIGHT_ALT:
		return MOD_ALT;
	case FKEY_LEFT_SUPER:
	case FKEY_RIGHT_SUPER:
		return MOD_SUPER;
	case FKEY_LEFT_HYPER:
	case FKEY_RIGHT_HYPER:
		return MOD_HYPER;
	case FKEY_LEFT_META:
	case FKEY_RIGHT_META:
		return MOD_META;
	}
	return 0;
}

int kbd_init(struct kbd *k, const char *locale) {
	memset(k, 0, sizeof(*k));
	k->ctx = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	if (!k->ctx)
		return -1;
	struct xkb_rule_names us = {.rules = "evdev", .layout = "us"};
	k->us = xkb_keymap_new_from_names(k->ctx, &us, XKB_KEYMAP_COMPILE_NO_FLAGS);
	const char *loc = locale;
	if (!loc && !((loc = getenv("LC_ALL")) && *loc) &&
	    !((loc = getenv("LC_CTYPE")) && *loc))
		loc = getenv("LANG");
	if (!loc || !*loc || !strcmp(loc, "C") || !strcmp(loc, "POSIX") ||
	    !strncmp(loc, "C.", 2))
		loc = "en_US.UTF-8";
	k->compose_table = xkb_compose_table_new_from_locale(
	    k->ctx, loc, XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!k->compose_table)
		k->compose_table = xkb_compose_table_new_from_locale(
		    k->ctx, "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (k->compose_table)
		k->compose =
		    xkb_compose_state_new(k->compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
	return 0;
}

void kbd_free(struct kbd *k) {
	xkb_compose_state_unref(k->compose);
	xkb_compose_table_unref(k->compose_table);
	xkb_state_unref(k->state);
	xkb_keymap_unref(k->map);
	xkb_keymap_unref(k->us);
	xkb_context_unref(k->ctx);
	memset(k, 0, sizeof(*k));
}

int kbd_keymap(struct kbd *k, const char *text) {
	struct xkb_keymap *map = xkb_keymap_new_from_string(
	    k->ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	struct xkb_state *st = map ? xkb_state_new(map) : NULL;
	if (!st) {
		xkb_keymap_unref(map);
		return -1;
	}
	xkb_state_unref(k->state);
	xkb_keymap_unref(k->map);
	k->map = map, k->state = st;
	k->ndown = 0;
	return 0;
}

void kbd_modifiers(struct kbd *k, uint32_t depressed, uint32_t latched,
                   uint32_t locked, uint32_t group) {
	if (k->state)
		xkb_state_update_mask(k->state, depressed, latched, locked, 0, 0,
		                      group);
}

static unsigned state_mods(struct kbd *k) {
	static const struct {
		const char *name;
		unsigned mod;
	} mods[] = {
	    {XKB_MOD_NAME_SHIFT, MOD_SHIFT},    {XKB_MOD_NAME_CTRL, MOD_CTRL},
	    {XKB_MOD_NAME_ALT, MOD_ALT},        {XKB_MOD_NAME_LOGO, MOD_SUPER},
	    {XKB_MOD_NAME_CAPS, MOD_CAPS_LOCK}, {XKB_MOD_NAME_NUM, MOD_NUM_LOCK}};
	unsigned m = 0;
	for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
		if (xkb_state_mod_name_is_active(k->state, mods[i].name,
		                                 XKB_STATE_MODS_EFFECTIVE) > 0)
			m |= mods[i].mod;
	return m;
}

static unsigned after(struct kbd *k, uint32_t evdev, unsigned bit, int type,
                      unsigned mods) {
	int i = 0;
	while (i < k->ndown && k->down[i] != evdev)
		i++;
	if (type == KEY_RELEASE) {
		if (i < k->ndown)
			k->down[i] = k->down[--k->ndown];
		mods &= ~bit;
		for (int j = 0; j < k->ndown; j++)
			if (mod_of(kbd_keysym_code(xkb_state_key_get_one_sym(
			        k->state, k->down[j] + 8))) == bit)
				mods |= bit;
		return mods;
	}
	if (i == k->ndown && k->ndown < (int)(sizeof(k->down) / sizeof(k->down[0])))
		k->down[k->ndown++] = evdev;
	return mods | bit;
}

static uint32_t level_cp(struct xkb_keymap *m, xkb_keycode_t kc,
                         xkb_layout_index_t layout, xkb_level_index_t level) {
	const xkb_keysym_t *syms;
	if (xkb_keymap_key_get_syms_by_level(m, kc, layout, level, &syms) != 1)
		return 0;
	xkb_keysym_t s = level ? syms[0] : xkb_keysym_to_lower(syms[0]);
	return xkb_keysym_to_utf32(s);
}

int kbd_key(struct kbd *k, uint32_t evdev, int type, struct key *out) {
	if (!k->state)
		return -1;
	xkb_keycode_t kc = evdev + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(k->state, kc);
	unsigned mods = state_mods(k);
	uint32_t code = kbd_keysym_code(sym);
	unsigned bit = mod_of(code);
	k->text[0] = 0;
	*out = (struct key){0, 0, 0, 0, type, k->text};
	if (bit)
		mods = after(k, evdev, bit, type, mods);
	out->mods = mods;

	if (type == KEY_PRESS && k->compose && !bit) {
		xkb_compose_state_feed(k->compose, sym);
		switch (xkb_compose_state_get_status(k->compose)) {
		case XKB_COMPOSE_COMPOSING:
			return -1;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(k->compose);
			return -1;
		case XKB_COMPOSE_COMPOSED:
			xkb_compose_state_get_utf8(k->compose, k->text, sizeof(k->text));
			xkb_compose_state_reset(k->compose);
			return k->text[0] ? 0 : -1;
		default:
			break;
		}
	}

	if (xkb_keysym_to_utf8(sym, k->text, sizeof(k->text)) <= 0)
		k->text[0] = 0;
	if (code) {
		out->code = code;
		return 0;
	}
	xkb_layout_index_t layout = xkb_state_key_get_layout(k->state, kc);
	out->code = level_cp(k->map, kc, layout, 0);
	out->shifted = level_cp(k->map, kc, layout, 1);
	if (out->shifted == out->code)
		out->shifted = 0;
	uint32_t base = k->us ? level_cp(k->us, kc, 0, 0) : 0;
	out->base = base != out->code ? base : 0;
	return out->code || k->text[0] ? 0 : -1;
}

int kbd_repeats(struct kbd *k, uint32_t evdev) {
	return k->map && xkb_keymap_key_repeats(k->map, evdev + 8);
}
