#include "cli.c"
#include "test.h"

static struct cli c;

static enum cli_mode parse(char **argv) {
	int argc = 0;
	while (argv[argc])
		argc++;
	return cli_parse(argc, argv, &c);
}

int main(void) {
	char *none[] = {"frost", NULL};
	CHECK(parse(none) == CLI_RUN && !c.cmd && !c.app_id,
	      "no arguments runs the shell");

	char *version[] = {"frost", "--version", NULL};
	CHECK(parse(version) == CLI_VERSION, "--version");
	char *help[] = {"frost", "-h", NULL};
	CHECK(parse(help) == CLI_HELP, "-h");

	char *prog[] = {"frost", "htop", "-d", "5", NULL};
	CHECK(parse(prog) == CLI_RUN && c.cmd == prog + 1 && !c.app_id,
	      "a program keeps its own arguments, options included");

	char *id[] = {"frost", "--app-id", "htop", "htop", NULL};
	CHECK(parse(id) == CLI_RUN && strcmp(c.app_id, "htop") == 0 &&
	          c.cmd == id + 3,
	      "--app-id before the program");
	char *eq[] = {"frost", "--app-id=icelink", NULL};
	CHECK(parse(eq) == CLI_RUN && strcmp(c.app_id, "icelink") == 0 && !c.cmd,
	      "--app-id=name, with the shell");

	char *dash[] = {"frost", "--", "-weird", NULL};
	CHECK(parse(dash) == CLI_RUN && c.cmd == dash + 2, "-- ends the options");

	char *empty[] = {"frost", "--app-id=", "htop", NULL};
	CHECK(parse(empty) == CLI_USAGE, "an empty app_id is a usage error");
	char *ctl[] = {"frost", "--app-id", "a\tb", NULL};
	CHECK(parse(ctl) == CLI_USAGE, "an app_id with a control character too");
	char *missing[] = {"frost", "--app-id", NULL};
	CHECK(parse(missing) == CLI_USAGE, "--app-id without a name too");
	char *unknown[] = {"frost", "-x", "htop", NULL};
	CHECK(parse(unknown) == CLI_USAGE, "an unknown option too");
	char *late[] = {"frost", "--version", "x", NULL};
	CHECK(parse(late) == CLI_USAGE, "--version takes nothing after it");

	return test_summary();
}
