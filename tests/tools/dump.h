#ifndef FROST_DUMP_H
#define FROST_DUMP_H

#include <stdio.h>

static void dump(FILE *out, struct scr *s) {
	for (int y = 0; y < s->rows; y++) {
		fputc('|', out);
		for (int x = 0; x < s->cols; x++) {
			struct cell *c = scr_cell(s, x, y);
			if (c->attr & ATTR_WIDE_TAIL)
				continue;
			uint32_t cps[3] = {c->cp ? c->cp : ' ', c->comb[0], c->comb[1]};
			for (int i = 0; i < 3 && cps[i]; i++) {
				uint32_t cp = cps[i];
				if (cp < 0x80)
					fputc((int)cp, out);
				else if (cp < 0x800)
					fprintf(out, "%c%c", 0xC0 | cp >> 6, 0x80 | (cp & 0x3F));
				else if (cp < 0x10000)
					fprintf(out, "%c%c%c", 0xE0 | cp >> 12,
					        0x80 | (cp >> 6 & 0x3F), 0x80 | (cp & 0x3F));
				else
					fprintf(out, "%c%c%c%c", 0xF0 | cp >> 18,
					        0x80 | (cp >> 12 & 0x3F), 0x80 | (cp >> 6 & 0x3F),
					        0x80 | (cp & 0x3F));
			}
		}
		fprintf(out, "|\n");
	}
	fprintf(out, "cursor %d,%d%s\n", s->x, s->y, s->alt ? " alt" : "");
	for (int y = 0; y < s->rows; y++) {
		int x = 0;
		while (x < s->cols) {
			struct cell *c = scr_cell(s, x, y);
			int k = x + 1;
			while (k < s->cols) {
				struct cell *d = scr_cell(s, k, y);
				if (d->fg != c->fg || d->bg != c->bg || d->ul != c->ul ||
				    (d->attr & ~(ATTR_WIDE | ATTR_WIDE_TAIL)) !=
				        (c->attr & ~(ATTR_WIDE | ATTR_WIDE_TAIL)))
					break;
				k++;
			}
			uint16_t attr = c->attr & ~(ATTR_WIDE | ATTR_WIDE_TAIL);
			if (c->fg || c->bg || c->ul || attr)
				fprintf(out,
				        "style %d %d-%d fg=%08x bg=%08x ul=%08x attr=%04x\n", y,
				        x, k - 1, c->fg, c->bg, c->ul, attr);
			x = k;
		}
	}
}

#endif
