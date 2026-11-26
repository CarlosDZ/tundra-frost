#ifndef FROST_SELECT_H
#define FROST_SELECT_H

#include <stddef.h>

#include "render.h"

enum { SELECT_CELLS, SELECT_WORDS, SELECT_LINES };

void select_span(const struct view *v, int a, int b, int mode, int *from,
                 int *to);
char *select_text(const struct view *v, int from, int to, size_t *len);

#endif
