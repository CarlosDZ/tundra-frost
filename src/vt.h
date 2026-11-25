#ifndef FROST_VT_H
#define FROST_VT_H

#include <stddef.h>
#include <stdint.h>

#define VT_MAX_PARAMS 32
#define VT_MAX_INTER 2
#define VT_STR_MAX (1 << 17)
#define VT_PARAM_MAX 65535

enum vt_kind {
	VT_PRINT,
	VT_PRINT_RUN,
	VT_EXECUTE,
	VT_CSI,
	VT_ESC,
	VT_OSC,
	VT_DCS,
	VT_APC,
};

struct vt_seq {
	char priv;
	char inter[VT_MAX_INTER + 1];
	char final;
	int nparams;
	uint16_t param[VT_MAX_PARAMS];
	uint32_t sub;
	uint32_t empty;
};

struct vt_event {
	enum vt_kind kind;
	uint32_t cp;
	const uint8_t *str;
	size_t len;
	const struct vt_seq *seq;
};

typedef void (*vt_sink)(void *ctx, const struct vt_event *ev);

enum vt_state {
	VT_GROUND,
	VT_ESCAPE,
	VT_ESCAPE_INTER,
	VT_CSI_ENTRY,
	VT_CSI_PARAM,
	VT_CSI_INTER,
	VT_CSI_IGNORE,
	VT_DCS_ENTRY,
	VT_DCS_PARAM,
	VT_DCS_INTER,
	VT_DCS_PASS,
	VT_DCS_IGNORE,
	VT_OSC_STRING,
	VT_APC_STRING,
};

struct vt {
	enum vt_state state;
	struct vt_seq seq;
	int ninter;
	int digits;
	uint32_t utf8_cp;
	int utf8_need;
	uint32_t utf8_min;
	int apc;
	int overflow;
	size_t len;
	uint8_t str[VT_STR_MAX];
};

void vt_init(struct vt *vt);
void vt_feed(struct vt *vt, const uint8_t *buf, size_t len, vt_sink sink,
             void *ctx);

unsigned vt_param(const struct vt_seq *seq, int i, unsigned def);

#endif
