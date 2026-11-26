#ifndef FROST_PASTE_H
#define FROST_PASTE_H

#include <stddef.h>

#define PASTE_WRAP 12

size_t paste_filter(const char *in, size_t n, int bracketed, char *out);

long base64_decode(const char *in, size_t n, char *out);

#endif
