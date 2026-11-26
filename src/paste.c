#include "paste.h"

#include <string.h>

static size_t letter(const unsigned char *p, size_t n, unsigned *cp) {
	size_t len = p[0] < 0x80       ? 1
	             : p[0] >> 5 == 6  ? 2
	             : p[0] >> 4 == 14 ? 3
	             : p[0] >> 3 == 30 ? 4
	                               : 0;
	if (!len || len > n)
		return 0;
	*cp = len == 1 ? p[0] : p[0] & (0x7F >> len);
	for (size_t i = 1; i < len; i++) {
		if ((p[i] & 0xC0) != 0x80)
			return 0;
		*cp = *cp << 6 | (p[i] & 0x3F);
	}
	return len;
}

size_t paste_filter(const char *in, size_t n, int bracketed, char *out) {
	const unsigned char *p = (const unsigned char *)in;
	size_t o = 0;
	if (bracketed) {
		memcpy(out, "\x1b[200~", 6);
		o = 6;
	}
	for (size_t i = 0; i < n;) {
		unsigned cp;
		size_t len = letter(p + i, n - i, &cp);
		if (!len) {
			i++;
			continue;
		}
		if (cp == '\r' || cp == '\n') {
			out[o++] = '\r';
			i += cp == '\r' && i + 1 < n && p[i + 1] == '\n' ? 2 : 1;
			continue;
		}
		if ((cp < 0x20 && cp != '\t') || (cp >= 0x7F && cp < 0xA0)) {
			i += len;
			continue;
		}
		memcpy(out + o, p + i, len);
		o += len, i += len;
	}
	if (bracketed) {
		memcpy(out + o, "\x1b[201~", 6);
		o += 6;
	}
	return o;
}

static int b64(int c) {
	return c >= 'A' && c <= 'Z'   ? c - 'A'
	       : c >= 'a' && c <= 'z' ? c - 'a' + 26
	       : c >= '0' && c <= '9' ? c - '0' + 52
	       : c == '+'             ? 62
	       : c == '/'             ? 63
	                              : -1;
}

long base64_decode(const char *in, size_t n, char *out) {
	unsigned acc = 0;
	int bits = 0;
	long o = 0;
	for (size_t i = 0; i < n; i++) {
		if (in[i] == '=')
			break;
		int v = b64((unsigned char)in[i]);
		if (v < 0)
			return -1;
		acc = acc << 6 | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (char)(acc >> bits & 0xFF);
		}
	}
	return o;
}
