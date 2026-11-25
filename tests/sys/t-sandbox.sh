#!/bin/sh
tool=build/test/tools/sandboxed
fail=0

check() {
	if [ "$1" = "$2" ]; then
		echo "ok - $3"
	else
		echo "not ok - $3 (got '$1', want '$2')"
		fail=1
	fi
}

check "$($tool allowed)" "alive" \
	"its pty, shared memory, size, hang-up and socket go through"
for what in open exec socket kill ioctl unlink exec-memory; do
	$tool $what >/dev/null 2>&1
	check $? 159 "$what kills it with SIGSYS"
done
exit $fail
