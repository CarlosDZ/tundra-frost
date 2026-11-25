#ifndef FROST_SANDBOX_H
#define FROST_SANDBOX_H

#include <sys/types.h>

int sandbox_term(int master, int dir, pid_t group);

#endif
