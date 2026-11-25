#include "screen.c"
#include "vt.c"
#include "width.c"

#include <stdio.h>
#include <stdlib.h>

#include "dump.h"

int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb");
	if (!f || argc < 4)
		return 2;
	static struct scr s;
	scr_init(&s, atoi(argv[2]), atoi(argv[3]));
	int c, was_alt = 0;
	while ((c = fgetc(f)) != EOF) {
		uint8_t b = (uint8_t)c;
		scr_feed(&s, &b, 1);
		s.reply_len = 0;
		if (was_alt && !s.alt) {
			printf("-- the alternate grid as it was left\n");
			s.alt = 1;
			break;
		}
		if (s.alt)
			was_alt = 1;
	}
	dump(stdout, &s);
	return 0;
}
