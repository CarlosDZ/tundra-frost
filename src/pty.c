#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "pty.h"

int pty_resize(int master, int cols, int rows, int w, int h) {
	struct winsize ws = {(unsigned short)rows, (unsigned short)cols,
	                     (unsigned short)(cols * w),
	                     (unsigned short)(rows * h)};
	return ioctl(master, TIOCSWINSZ, &ws);
}

static const char *user_shell(void) {
	const char *sh = getenv("SHELL");
	if (sh && *sh)
		return sh;
	struct passwd *pw = getpwuid(getuid());
	if (pw && pw->pw_shell && *pw->pw_shell)
		return pw->pw_shell;
	return "/bin/sh";
}

int pty_spawn(char **cmd, int cols, int rows, pid_t *pid) {
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0) {
		perror("posix_openpt error");
		return -1;
	}
	char name[64];
	if (grantpt(master) < 0 || unlockpt(master) < 0 ||
	    ptsname_r(master, name, sizeof(name)) != 0) {
		perror("pty error");
		close(master);
		return -1;
	}
	pty_resize(master, cols, rows, 0, 0);
	*pid = fork();
	if (*pid < 0) {
		perror("fork error");
		close(master);
		return -1;
	}
	if (*pid > 0)
		return master;

	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (int sig = 1; sig < NSIG; sig++)
		signal(sig, SIG_DFL);
	setsid();
	int slave = open(name, O_RDWR);
	if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) < 0)
		_exit(127);
	dup2(slave, 0), dup2(slave, 1), dup2(slave, 2);
	close_range(3, ~0U, 0);
	setenv("TERM", "frost", 1);
	if (cmd) {
		execvp(cmd[0], cmd);
		fprintf(stderr, "frost: cannot run %s\n", cmd[0]);
		_exit(127);
	}
	const char *sh = user_shell();
	const char *base = strrchr(sh, '/');
	execl(sh, base ? base + 1 : sh, (char *)NULL);
	fprintf(stderr, "frost: cannot run %s\n", sh);
	_exit(127);
}
