#ifndef FROST_TMUXCMD_H
#define FROST_TMUXCMD_H

#include <stddef.h>
#include <stdint.h>

struct tmux_cmd {
	const char *name;
	uint64_t flags;
	const char *value[52];
	int argc;
	char **argv;
};

int tmux_parse(int argc, char **argv, struct tmux_cmd *c);
int tmux_flag(const struct tmux_cmd *c, char letter);
const char *tmux_value(const struct tmux_cmd *c, char letter);

typedef const char *(*tmux_var_fn)(void *ctx, const char *name);

size_t tmux_format(const char *fmt, tmux_var_fn var, void *ctx, char *out,
                   size_t max);

struct tmux_style {
	uint32_t fg;
	int bold;
};

void tmux_style(const char *spec, const struct tmux_style *base,
                struct tmux_style *st);

typedef void (*tmux_piece_fn)(void *ctx, const char *text, size_t len,
                              const struct tmux_style *st);
void tmux_format_styled(const char *fmt, const struct tmux_style *base,
                        tmux_var_fn var, void *vctx, tmux_piece_fn piece,
                        void *pctx);

#endif
