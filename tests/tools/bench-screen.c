#include "screen.c"
#include "vt.c"
#include "width.c"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	uint8_t *buf = malloc(n);
	if (fread(buf, 1, n, f) != (size_t)n)
		return 1;
	int rounds = argc > 2 ? atoi(argv[2]) : 1;
	static struct scr s;
	scr_init(&s, 160, 45);
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int r = 0; r < rounds; r++) {
		scr_feed(&s, buf, n);
		s.reply_len = 0;
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	double t = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	printf("%.1f MB in %.3f s: %.0f MB/s\n", n * rounds / 1e6, t,
	       n * rounds / 1e6 / t);
	return 0;
}
