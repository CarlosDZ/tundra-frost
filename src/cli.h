#ifndef FROST_CLI_H
#define FROST_CLI_H

#include <stdio.h>

enum cli_mode { CLI_RUN, CLI_VERSION, CLI_HELP, CLI_USAGE };

struct cli {
	const char *app_id;
	char **cmd;
};

enum cli_mode cli_parse(int argc, char **argv, struct cli *c);
void cli_usage(FILE *f);

#endif
