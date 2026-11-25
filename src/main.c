#include <stdio.h>

#include "cli.h"
#include "win.h"

int main(int argc, char **argv) {
	struct cli c;
	switch (cli_parse(argc, argv, &c)) {
	case CLI_VERSION:
		printf("tundra-frost %s\n", TUNDRA_FROST_VERSION);
		return 0;
	case CLI_HELP:
		cli_usage(stdout);
		return 0;
	case CLI_USAGE:
		cli_usage(stderr);
		return 2;
	case CLI_RUN:
		break;
	}
	return win_run(&c);
}
