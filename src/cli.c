#include <string.h>

#include "cli.h"

#define APP_ID_MAX 255

static int app_id_ok(const char *s) {
	size_t n = strlen(s);
	if (n == 0 || n > APP_ID_MAX)
		return 0;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7F)
			return 0;
	return 1;
}

enum cli_mode cli_parse(int argc, char **argv, struct cli *c) {
	c->app_id = NULL;
	c->cmd = NULL;
	if (argc == 2 && strcmp(argv[1], "--version") == 0)
		return CLI_VERSION;
	if (argc == 2 &&
	    (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
		return CLI_HELP;
	int i = 1;
	while (i < argc && argv[i][0] == '-') {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "--app-id") == 0 && i + 1 < argc) {
			c->app_id = argv[i + 1];
			i += 2;
		} else if (strncmp(argv[i], "--app-id=", 9) == 0) {
			c->app_id = argv[i] + 9;
			i++;
		} else {
			return CLI_USAGE;
		}
		if (!app_id_ok(c->app_id))
			return CLI_USAGE;
	}
	if (i < argc)
		c->cmd = argv + i;
	return CLI_RUN;
}

void cli_usage(FILE *f) {
	fprintf(f, "usage: frost [--app-id name] [program [argument...]]\n"
	           "       frost --version\n");
}
