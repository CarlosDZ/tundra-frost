#ifndef FROST_LINKS_H
#define FROST_LINKS_H

#include <stddef.h>

#include "render.h"

enum { LINK_NONE, LINK_OSC8, LINK_BARE };

#define LINK_MAX 2048

int link_at(const struct view *v, int pos, int *from, int *to, int *id,
            char *url);
int link_allowed(const char *url);
int link_cwd(const char *uri, const char *host, char *out, size_t max);

#endif
