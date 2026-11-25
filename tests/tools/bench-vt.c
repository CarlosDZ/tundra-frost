#include "vt.c"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static size_t events;

static void sink(void *ctx, const struct vt_event *e) {
	(void)ctx, (void)e;
	events++;
}

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
	static struct vt vt;
	vt_init(&vt);
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int r = 0; r < rounds; r++)
		vt_feed(&vt, buf, n, sink, NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	printf("%.1f MB in %.3f s: %.0f MB/s, %zu events\n", n * rounds / 1e6, s,
	       n * rounds / 1e6 / s, events);
	return 0;
}
