#ifndef FROST_PTY_H
#define FROST_PTY_H

#include <sys/types.h>

int pty_spawn(char **cmd, int cols, int rows, pid_t *pid);
int pty_resize(int master, int cols, int rows, int w, int h);

#endif
