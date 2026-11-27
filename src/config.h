#ifndef FROST_CONFIG_H
#define FROST_CONFIG_H

#include <stddef.h>

#include "screen.h"

#define CONFIG_ERR_MAX 128

struct config {
	int size;
	int history;
	enum cursor_shape cursor;
	int blink;
};

void config_default(struct config *c);
int config_parse(const char *text, size_t len, struct config *c, char *err);

#endif
