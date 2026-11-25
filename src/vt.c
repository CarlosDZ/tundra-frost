#include "vt.h"

#include <string.h>

#define REPLACEMENT 0xFFFD

void vt_init(struct vt *vt) {
	vt->state = VT_GROUND;
	vt->utf8_need = 0;
	vt->len = 0;
	vt->overflow = 0;
}

unsigned vt_param(const struct vt_seq *seq, int i, unsigned def) {
	if (i < 0 || i >= seq->nparams || (seq->empty >> i & 1))
		return def;
	return seq->param[i];
}

static void seq_clear(struct vt *vt) {
	memset(&vt->seq, 0, sizeof(vt->seq));
	vt->ninter = 0;
	vt->digits = 0;
}

static void str_clear(struct vt *vt) {
	vt->len = 0;
	vt->overflow = 0;
}

static void str_put(struct vt *vt, uint8_t b) {
	if (vt->len < VT_STR_MAX)
		vt->str[vt->len++] = b;
	else
		vt->overflow = 1;
}

static void emit(vt_sink sink, void *ctx, enum vt_kind kind, uint32_t cp,
                 const uint8_t *str, size_t len, const struct vt_seq *seq) {
	struct vt_event ev = {kind, cp, str, len, seq};
	sink(ctx, &ev);
}

static void collect(struct vt *vt, uint8_t b, enum vt_state ignore) {
	if (vt->ninter == VT_MAX_INTER) {
		vt->state = ignore;
		return;
	}
	vt->seq.inter[vt->ninter++] = (char)b;
}

static void param(struct vt *vt, uint8_t b) {
	struct vt_seq *s = &vt->seq;
	if (s->nparams == 0) {
		s->nparams = 1;
		s->empty |= 1;
	}
	if (b == ';' || b == ':') {
		if (s->nparams == VT_MAX_PARAMS)
			return;
		int i = s->nparams++;
		s->param[i] = 0;
		s->empty |= 1u << i;
		if (b == ':')
			s->sub |= 1u << i;
		return;
	}
	int i = s->nparams - 1;
	unsigned v = s->param[i] * 10u + (b - '0');
	s->param[i] = v > VT_PARAM_MAX ? VT_PARAM_MAX : (uint16_t)v;
	s->empty &= ~(1u << i);
}

static void end_string(struct vt *vt, vt_sink sink, void *ctx) {
	if (!vt->overflow) {
		if (vt->state == VT_OSC_STRING)
			emit(sink, ctx, VT_OSC, 0, vt->str, vt->len, NULL);
		else if (vt->state == VT_DCS_PASS)
			emit(sink, ctx, VT_DCS, 0, vt->str, vt->len, &vt->seq);
		else if (vt->state == VT_APC_STRING && vt->apc)
			emit(sink, ctx, VT_APC, 0, vt->str, vt->len, NULL);
	}
	str_clear(vt);
}

static void utf8_byte(struct vt *vt, uint8_t b, vt_sink sink, void *ctx) {
	if (vt->utf8_need) {
		if ((b & 0xC0) == 0x80) {
			vt->utf8_cp = vt->utf8_cp << 6 | (b & 0x3F);
			if (--vt->utf8_need)
				return;
			uint32_t cp = vt->utf8_cp;
			if (cp < vt->utf8_min || (cp >= 0xD800 && cp <= 0xDFFF) ||
			    cp > 0x10FFFF)
				cp = REPLACEMENT;
			if (cp >= 0x80 && cp <= 0x9F)
				return;
			emit(sink, ctx, VT_PRINT, cp, NULL, 0, NULL);
			return;
		}
		vt->utf8_need = 0;
		emit(sink, ctx, VT_PRINT, REPLACEMENT, NULL, 0, NULL);
	}
	if (b >= 0xC2 && b <= 0xDF) {
		vt->utf8_need = 1, vt->utf8_cp = b & 0x1F, vt->utf8_min = 0x80;
	} else if (b >= 0xE0 && b <= 0xEF) {
		vt->utf8_need = 2, vt->utf8_cp = b & 0x0F, vt->utf8_min = 0x800;
	} else if (b >= 0xF0 && b <= 0xF4) {
		vt->utf8_need = 3, vt->utf8_cp = b & 0x07, vt->utf8_min = 0x10000;
	} else {
		emit(sink, ctx, VT_PRINT, REPLACEMENT, NULL, 0, NULL);
	}
}

static void to_escape(struct vt *vt, vt_sink sink, void *ctx) {
	if (vt->state == VT_OSC_STRING || vt->state == VT_DCS_PASS ||
	    vt->state == VT_APC_STRING)
		end_string(vt, sink, ctx);
	seq_clear(vt);
	vt->state = VT_ESCAPE;
}

static void byte(struct vt *vt, uint8_t b, vt_sink sink, void *ctx) {
	if (vt->utf8_need && (b < 0x80 || b > 0xBF)) {
		vt->utf8_need = 0;
		emit(sink, ctx, VT_PRINT, REPLACEMENT, NULL, 0, NULL);
	}
	if (b == 0x1B) {
		to_escape(vt, sink, ctx);
		return;
	}
	if (b == 0x18 || b == 0x1A) {
		str_clear(vt);
		vt->state = VT_GROUND;
		emit(sink, ctx, VT_EXECUTE, b, NULL, 0, NULL);
		return;
	}

	switch (vt->state) {
	case VT_GROUND:
		if (b < 0x20)
			emit(sink, ctx, VT_EXECUTE, b, NULL, 0, NULL);
		else if (b < 0x7F)
			emit(sink, ctx, VT_PRINT, b, NULL, 0, NULL);
		else if (b >= 0x80)
			utf8_byte(vt, b, sink, ctx);
		return;

	case VT_ESCAPE:
		if (b < 0x20) {
			emit(sink, ctx, VT_EXECUTE, b, NULL, 0, NULL);
		} else if (b < 0x30) {
			collect(vt, b, VT_GROUND);
			if (vt->state == VT_ESCAPE)
				vt->state = VT_ESCAPE_INTER;
		} else if (b == '[') {
			vt->state = VT_CSI_ENTRY;
		} else if (b == ']') {
			str_clear(vt);
			vt->state = VT_OSC_STRING;
		} else if (b == 'P') {
			vt->state = VT_DCS_ENTRY;
		} else if (b == '_' || b == 'X' || b == '^') {
			str_clear(vt);
			vt->apc = b == '_';
			vt->state = VT_APC_STRING;
		} else if (b < 0x7F) {
			vt->seq.final = (char)b;
			vt->state = VT_GROUND;
			emit(sink, ctx, VT_ESC, 0, NULL, 0, &vt->seq);
		}
		return;

	case VT_ESCAPE_INTER:
		if (b < 0x20) {
			emit(sink, ctx, VT_EXECUTE, b, NULL, 0, NULL);
		} else if (b < 0x30) {
			collect(vt, b, VT_GROUND);
		} else if (b < 0x7F) {
			vt->seq.final = (char)b;
			vt->state = VT_GROUND;
			emit(sink, ctx, VT_ESC, 0, NULL, 0, &vt->seq);
		}
		return;

	case VT_CSI_ENTRY:
	case VT_CSI_PARAM:
	case VT_CSI_INTER:
		if (b < 0x20) {
			emit(sink, ctx, VT_EXECUTE, b, NULL, 0, NULL);
		} else if (b < 0x30) {
			collect(vt, b, VT_CSI_IGNORE);
			if (vt->state != VT_CSI_IGNORE)
				vt->state = VT_CSI_INTER;
		} else if (b < 0x3C) {
			if (vt->state == VT_CSI_INTER)
				vt->state = VT_CSI_IGNORE;
			else
				param(vt, b), vt->state = VT_CSI_PARAM;
		} else if (b < 0x40) {
			if (vt->state == VT_CSI_ENTRY)
				vt->seq.priv = (char)b, vt->state = VT_CSI_PARAM;
			else
				vt->state = VT_CSI_IGNORE;
		} else if (b < 0x7F) {
			vt->seq.final = (char)b;
			vt->state = VT_GROUND;
			emit(sink, ctx, VT_CSI, 0, NULL, 0, &vt->seq);
		}
		return;

	case VT_CSI_IGNORE:
		if (b < 0x20)
			emit(sink, ctx, VT_EXECUTE, b, NULL, 0, NULL);
		else if (b >= 0x40 && b < 0x7F)
			vt->state = VT_GROUND;
		return;

	case VT_DCS_ENTRY:
	case VT_DCS_PARAM:
	case VT_DCS_INTER:
		if (b < 0x20) {
			return;
		} else if (b < 0x30) {
			collect(vt, b, VT_DCS_IGNORE);
			if (vt->state != VT_DCS_IGNORE)
				vt->state = VT_DCS_INTER;
		} else if (b < 0x3C) {
			if (vt->state == VT_DCS_INTER)
				vt->state = VT_DCS_IGNORE;
			else
				param(vt, b), vt->state = VT_DCS_PARAM;
		} else if (b < 0x40) {
			if (vt->state == VT_DCS_ENTRY)
				vt->seq.priv = (char)b, vt->state = VT_DCS_PARAM;
			else
				vt->state = VT_DCS_IGNORE;
		} else if (b < 0x7F) {
			vt->seq.final = (char)b;
			str_clear(vt);
			vt->state = VT_DCS_PASS;
		}
		return;

	case VT_DCS_PASS:
		if (b != 0x7F)
			str_put(vt, b);
		return;

	case VT_DCS_IGNORE:
		return;

	case VT_OSC_STRING:
		if (b == 0x07) {
			end_string(vt, sink, ctx);
			vt->state = VT_GROUND;
		} else if (b >= 0x20) {
			str_put(vt, b);
		}
		return;

	case VT_APC_STRING:
		if (b >= 0x20 || b == 0x09)
			str_put(vt, b);
		return;
	}
}

void vt_feed(struct vt *vt, const uint8_t *buf, size_t len, vt_sink sink,
             void *ctx) {
	size_t i = 0;
	while (i < len) {
		if (vt->state == VT_GROUND && !vt->utf8_need) {
			size_t j = i;
			while (j < len && buf[j] >= 0x20 && buf[j] < 0x7F)
				j++;
			if (j > i) {
				emit(sink, ctx, VT_PRINT_RUN, 0, buf + i, j - i, NULL);
				i = j;
				continue;
			}
		}
		byte(vt, buf[i++], sink, ctx);
	}
}
