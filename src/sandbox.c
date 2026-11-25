#include "sandbox.h"

#include <fcntl.h>
#include <seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <wchar.h>

static int allow(scmp_filter_ctx ctx, int master, int dir, pid_t group) {
	static const int calls[] = {
	    SCMP_SYS(read),
	    SCMP_SYS(write),
	    SCMP_SYS(readv),
	    SCMP_SYS(writev),
	    SCMP_SYS(close),
	    SCMP_SYS(poll),
	    SCMP_SYS(ppoll),
	    SCMP_SYS(recvmsg),
	    SCMP_SYS(sendmsg),
	    SCMP_SYS(accept4),
	    SCMP_SYS(getsockopt),
	    SCMP_SYS(getuid),
	    SCMP_SYS(wait4),
	    SCMP_SYS(fcntl),
	    SCMP_SYS(memfd_create),
	    SCMP_SYS(ftruncate),
	    SCMP_SYS(munmap),
	    SCMP_SYS(mremap),
	    SCMP_SYS(madvise),
	    SCMP_SYS(brk),
	    SCMP_SYS(getrandom),
	    SCMP_SYS(clock_gettime),
	    SCMP_SYS(rt_sigreturn),
	    SCMP_SYS(rt_sigprocmask),
	    SCMP_SYS(restart_syscall),
	    SCMP_SYS(exit),
	    SCMP_SYS(exit_group),
	};
	for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
		if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, calls[i], 0) < 0)
			return -1;
	if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 1,
	                     SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0)) < 0 ||
	    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 1,
	                     SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, 0)) < 0)
		return -1;
	if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 2,
	                     SCMP_A0_32(SCMP_CMP_EQ, (uint32_t)master),
	                     SCMP_A1(SCMP_CMP_EQ, TIOCSWINSZ)) < 0 ||
	    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(kill), 2,
	                     SCMP_A0_32(SCMP_CMP_EQ, (uint32_t)-group),
	                     SCMP_A1_32(SCMP_CMP_EQ, SIGHUP)) < 0)
		return -1;
	if (dir >= 0 && seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(unlinkat), 2,
	                                 SCMP_A0_32(SCMP_CMP_EQ, (uint32_t)dir),
	                                 SCMP_A2_32(SCMP_CMP_EQ, 0)) < 0)
		return -1;
	return 0;
}

int sandbox_term(int master, int dir, pid_t group) {
	fwide(stderr, -1);
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		perror("PR_SET_NO_NEW_PRIVS error");
		return -1;
	}
	scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
	if (!ctx ||
	    seccomp_attr_set(ctx, SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_KILL_PROCESS) <
	        0 ||
	    allow(ctx, master, dir, group) < 0 || seccomp_load(ctx) < 0) {
		fprintf(stderr, "frost-term: cannot load the seccomp filter\n");
		if (ctx)
			seccomp_release(ctx);
		return -1;
	}
	seccomp_release(ctx);
	return 0;
}
