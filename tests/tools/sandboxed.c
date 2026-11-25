#include "sandbox.c"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if (argc != 2)
		return 2;
	int p[2];
	if (pipe(p) < 0)
		return 1;
	int dir = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	setvbuf(stdout, NULL, _IONBF, 0);
	pid_t group = 2147480000;
	if (sandbox_term(p[0], dir, group) < 0)
		return 1;
	const char *a = argv[1];
	struct winsize ws = {24, 80, 0, 0};
	if (strcmp(a, "allowed") == 0) {
		char c = 'x';
		if (write(p[1], &c, 1) != 1 || read(p[0], &c, 1) != 1)
			return 1;
		int m = memfd_create("t", MFD_CLOEXEC);
		if (m < 0 || ftruncate(m, 4096) < 0 ||
		    mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0) ==
		        MAP_FAILED)
			return 1;
		void *big = malloc(1 << 22);
		free(big);
		ioctl(p[0], TIOCSWINSZ, &ws);
		kill(-group, SIGHUP);
		unlinkat(dir, "frost-none", 0);
	} else if (strcmp(a, "open") == 0) {
		open("/etc/passwd", O_RDONLY);
	} else if (strcmp(a, "exec") == 0) {
		execl("/bin/true", "true", (char *)NULL);
	} else if (strcmp(a, "socket") == 0) {
		socket(AF_INET, SOCK_STREAM, 0);
	} else if (strcmp(a, "kill") == 0) {
		kill(-group, SIGKILL);
	} else if (strcmp(a, "ioctl") == 0) {
		ioctl(p[1], TIOCSWINSZ, &ws);
	} else if (strcmp(a, "unlink") == 0) {
		unlinkat(AT_FDCWD, "frost-none", 0);
	} else if (strcmp(a, "exec-memory") == 0) {
		mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1,
		     0);
	} else {
		return 2;
	}
	printf("alive\n");
	return 0;
}
