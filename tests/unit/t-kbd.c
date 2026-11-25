#include "kbd.c"
#include "keys.c"
#include "test.h"

#include <linux/input-event-codes.h>

static struct kbd k;
static struct xkb_keymap *server_map;
static struct xkb_state *server;
static struct key got;
static int sent;

static int layout(const char *name, const char *options) {
	struct xkb_rule_names n = {
	    .rules = "evdev", .layout = name, .options = options};
	xkb_state_unref(server);
	xkb_keymap_unref(server_map);
	server_map = xkb_keymap_new_from_names(k.ctx, &n, 0);
	server = server_map ? xkb_state_new(server_map) : NULL;
	char *text =
	    server_map
	        ? xkb_keymap_get_as_string(server_map, XKB_KEYMAP_FORMAT_TEXT_V1)
	        : NULL;
	int r = text ? kbd_keymap(&k, text) : -1;
	free(text);
	return r;
}

static void key(uint32_t evdev, int down) {
	sent = kbd_key(&k, evdev, down ? KEY_PRESS : KEY_RELEASE, &got) == 0;
	xkb_state_update_key(server, evdev + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
	kbd_modifiers(
	    &k, xkb_state_serialize_mods(server, XKB_STATE_MODS_DEPRESSED),
	    xkb_state_serialize_mods(server, XKB_STATE_MODS_LATCHED),
	    xkb_state_serialize_mods(server, XKB_STATE_MODS_LOCKED),
	    xkb_state_serialize_layout(server, XKB_STATE_LAYOUT_EFFECTIVE));
}

static void tap(uint32_t evdev) {
	key(evdev, 1);
	struct key press = got;
	int was = sent;
	static char text[KBD_TEXT_MAX];
	strcpy(text, got.text);
	press.text = text;
	key(evdev, 0);
	got = press;
	sent = was;
}

static int is(uint32_t code, uint32_t shifted, uint32_t base, unsigned mods,
              const char *text) {
	return sent && got.code == code && got.shifted == shifted &&
	       got.base == base && got.mods == mods && !strcmp(got.text, text);
}

static void encodes(const char *want, const char *desc) {
	struct key_modes m = {0, 0, 0};
	char out[KEY_OUT_MAX];
	size_t n = sent ? key_encode(&got, &m, out) : 0;
	CHECK(n == strlen(want) && !memcmp(out, want, n), desc);
}

int main(void) {
	kbd_init(&k, "en_US.UTF-8");
	if (layout("us", NULL) < 0) {
		printf("not ok - no xkb data\n");
		return 1;
	}
	tap(KEY_A);
	CHECK(is('a', 'A', 0, 0, "a"), "a key writes its letter");
	key(KEY_LEFTSHIFT, 1);
	CHECK(is(FKEY_LEFT_SHIFT, 0, 0, MOD_SHIFT, ""),
	      "pressing Shift says Shift is down");
	tap(KEY_A);
	CHECK(is('a', 'A', 0, MOD_SHIFT, "A"),
	      "with Shift: the key, its shifted letter and the text");
	key(KEY_LEFTSHIFT, 0);
	CHECK(is(FKEY_LEFT_SHIFT, 0, 0, 0, ""), "releasing it says it is up");
	key(KEY_LEFTSHIFT, 1);
	key(KEY_RIGHTSHIFT, 1);
	key(KEY_LEFTSHIFT, 0);
	CHECK(is(FKEY_LEFT_SHIFT, 0, 0, MOD_SHIFT, ""),
	      "with the other Shift still down, Shift stays");
	key(KEY_RIGHTSHIFT, 0);

	key(KEY_LEFTCTRL, 1);
	tap(KEY_A);
	CHECK(is('a', 'A', 0, MOD_CTRL, "a"), "Ctrl does not change the text");
	encodes("\x01", "Ctrl + A goes as 0x01");
	key(KEY_LEFTSHIFT, 1);
	tap(KEY_TAB);
	CHECK(is(FKEY_TAB, 0, 0, MOD_CTRL | MOD_SHIFT, ""),
	      "Shift + Tab is still Tab, with Shift");
	key(KEY_LEFTSHIFT, 0);
	key(KEY_LEFTCTRL, 0);

	tap(KEY_UP);
	CHECK(is(FKEY_UP, 0, 0, 0, ""), "an arrow");
	tap(KEY_F5);
	CHECK(is(FKEY_F(5), 0, 0, 0, ""), "a function key");
	tap(KEY_KP1);
	CHECK(is(FKEY_KP_END, 0, 0, 0, ""), "keypad 1 without Num Lock is End");
	tap(KEY_NUMLOCK);
	tap(KEY_KP1);
	CHECK(is(FKEY_KP_0 + 1, 0, 0, MOD_NUM_LOCK, "1"),
	      "with Num Lock, keypad 1 writes 1");
	tap(KEY_NUMLOCK);
	tap(KEY_KPENTER);
	CHECK(is(FKEY_KP_ENTER, 0, 0, 0, "\r"), "keypad Enter is its own key");
	CHECK(kbd_repeats(&k, KEY_A) && !kbd_repeats(&k, KEY_LEFTSHIFT),
	      "letters repeat, Shift does not");

	layout("us", "caps:escape");
	tap(KEY_CAPSLOCK);
	CHECK(is(FKEY_ESCAPE, 0, 0, 0, "\x1b"),
	      "with caps:escape, Caps Lock is Escape");

	layout("es", NULL);
	tap(KEY_SEMICOLON);
	CHECK(is(0xf1, 0xd1, ';', 0, "ñ"),
	      "Spanish ñ: the letter, its capital and the US key");
	key(KEY_RIGHTALT, 1);
	tap(KEY_2);
	CHECK(is('2', '"', 0, 0, "@"),
	      "AltGr + 2 writes @, and AltGr is not a modifier of the key");
	key(KEY_RIGHTALT, 0);
	tap(KEY_APOSTROPHE);
	CHECK(!sent, "a dead key sends nothing");
	tap(KEY_E);
	CHECK(is(0, 0, 0, 0, "é"), "and the next letter takes its accent");
	encodes("é", "which goes as its text");

	layout("ru", NULL);
	tap(KEY_C);
	CHECK(is(0x441, 0x421, 'c', 0, "с"),
	      "Russian с: the letter and the US key under it");
	key(KEY_LEFTCTRL, 1);
	tap(KEY_C);
	encodes("\x03", "Ctrl + с is Ctrl + C for the classic programs");
	key(KEY_LEFTCTRL, 0);

	xkb_state_unref(server);
	xkb_keymap_unref(server_map);
	kbd_free(&k);
	return test_summary();
}
