#include "width.h"

#include <stddef.h>

struct width_range {
	uint32_t first, last;
};

#include "width-table.h"

static int in(const struct width_range *r, size_t n, uint32_t cp) {
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (cp < r[mid].first)
			hi = mid;
		else if (cp > r[mid].last)
			lo = mid + 1;
		else
			return 1;
	}
	return 0;
}

int width_of(uint32_t cp) {
	if (cp < 0x300)
		return 1;
	if (in(width_zero, sizeof(width_zero) / sizeof(width_zero[0]), cp))
		return 0;
	if (in(width_wide, sizeof(width_wide) / sizeof(width_wide[0]), cp))
		return 2;
	return 1;
}
