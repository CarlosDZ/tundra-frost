#ifndef FROST_KBD_H
#define FROST_KBD_H

#include <stdint.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "keys.h"

#define KBD_TEXT_MAX 64

struct kbd {
	struct xkb_context *ctx;
	struct xkb_keymap *map, *us;
	struct xkb_state *state;
	struct xkb_compose_table *compose_table;
	struct xkb_compose_state *compose;
	unsigned held;
	uint32_t down[16];
	int ndown;
	char text[KBD_TEXT_MAX];
};

int kbd_init(struct kbd *k, const char *locale);
void kbd_free(struct kbd *k);
int kbd_keymap(struct kbd *k, const char *text);
void kbd_modifiers(struct kbd *k, uint32_t depressed, uint32_t latched,
                   uint32_t locked, uint32_t group);
int kbd_key(struct kbd *k, uint32_t evdev, int type, struct key *out);
int kbd_repeats(struct kbd *k, uint32_t evdev);
unsigned kbd_mods(struct kbd *k);
uint32_t kbd_keysym_code(xkb_keysym_t sym);

#endif
