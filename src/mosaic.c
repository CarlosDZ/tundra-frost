#include "mosaic.h"

static int part(int total, int n, int i) { return total / n + (i < total % n); }

static void along(int length, int n, int titles, int *start, int *len) {
	int gaps = titles ? 0 : n - 1;
	int room = length - gaps > n ? length - gaps : n;
	for (int i = 0, at = 0; i < n; i++) {
		start[i] = at;
		len[i] = part(room, n, i);
		at += len[i] + (titles ? 0 : 1);
	}
}

static void place(struct box *b, int x, int y, int cols, int rows, int titles) {
	b->x = x, b->cols = cols > 0 ? cols : 1;
	if (titles && rows > 1) {
		b->title_y = y;
		b->y = y + 1, b->rows = rows - 1;
	} else {
		b->title_y = -1;
		b->y = y, b->rows = rows > 0 ? rows : 1;
	}
}

static void grid(int cols, int rows, int n, int ncols, int titles,
                 struct box *out) {
	int cx[64], cw[64];
	along(cols, ncols, 0, cx, cw);
	for (int c = 0, k = 0; c < ncols; c++) {
		int in = part(n, ncols, c);
		int ry[64], rh[64];
		along(rows, in, titles, ry, rh);
		for (int r = 0; r < in; r++, k++)
			place(&out[k], cx[c], ry[r], cw[c], rh[r], titles);
	}
}

void mosaic_layout(int cols, int rows, int n, enum layout layout, int main,
                   int titles, struct box *out) {
	if (n <= 0)
		return;
	if (n > 64)
		n = 64;
	if (n == 1) {
		place(&out[0], 0, 0, cols, rows, 0);
		return;
	}
	switch (layout) {
	case LAYOUT_MAIN_VERTICAL: {
		int mw = (cols * (main > 0 && main < 100 ? main : 50) + 50) / 100;
		mw = mw < 1 ? 1 : mw > cols - 2 ? cols - 2 : mw;
		place(&out[0], 0, 0, mw, rows, titles);
		int ry[64], rh[64];
		along(rows, n - 1, titles, ry, rh);
		for (int i = 1; i < n; i++)
			place(&out[i], mw + 1, ry[i - 1], cols - mw - 1, rh[i - 1], titles);
		break;
	}
	case LAYOUT_TILED: {
		int c = 1;
		while (c * c < n)
			c++;
		int r = (n + c - 1) / c;
		int ry[64], rh[64];
		along(rows, r, titles, ry, rh);
		for (int row = 0, k = 0; row < r; row++) {
			int in = row < r - 1 ? c : n - c * (r - 1);
			int cx[64], cw[64];
			along(cols, in, 0, cx, cw);
			for (int i = 0; i < in; i++, k++)
				place(&out[k], cx[i], ry[row], cw[i], rh[row], titles);
		}
		break;
	}
	case LAYOUT_EVEN_HORIZONTAL:
		grid(cols, rows, n, n, titles, out);
		break;
	case LAYOUT_EVEN_VERTICAL:
		grid(cols, rows, n, 1, titles, out);
		break;
	}
}
