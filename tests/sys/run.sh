#!/bin/sh
cd "$(dirname "$0")/../.." || exit 1

rc=0
for t in tests/sys/t-*.sh; do
	[ -e "$t" ] || continue
	case $t in
	*/t-term.sh | */t-attach.sh | */t-window.sh)
		for term in build/test/frost-term ./frost-term; do
			printf '# %s with %s\n' "$t" "$term"
			FROST_TERM=$term sh "$t" || rc=1
		done
		;;
	*)
		printf '# %s\n' "$t"
		sh "$t" || rc=1
		;;
	esac
done
exit $rc
