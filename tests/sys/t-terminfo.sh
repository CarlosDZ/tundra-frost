#!/bin/sh
term=build/test/frost-term
client=build/test/tools/client
fail=0

check() {
	if [ "$1" = "$2" ]; then
		echo "ok - $3"
	else
		printf 'not ok - %s\n# got:\n%s\n# want:\n%s\n' "$3" "$1" "$2"
		fail=1
	fi
}

if ! command -v tic >/dev/null || ! command -v tput >/dev/null; then
	echo "# skip: no tic or tput"
	exit 0
fi

check "$(tic -c -x terminfo/frost.ti 2>&1)" "" \
	"tic has nothing to say about the terminfo of frost"

export TERMINFO_DIRS=$PWD/build/terminfo
gone=
for cap in mc0 mc4 mc5 mc5i meml memu smm rmm km smglp smglr smgrp mgc XR xr; do
	infocmp -x -1 frost | grep -q "^	$cap[=,]" && gone="$gone $cap"
done
check "$gone" "" "it leaves out what frost does not do"
check "$(tput -T frost smcup | od -An -c | tr -d ' \n'):$(tput -T frost Se | od -An -c | tr -d ' \n')" \
	'033[?1049h:033[0q' "the alternate screen keeps no title, Se goes back to the cursor of frost"

check "$($client $term 20 3 -- sh -c 'tput clear; tput cup 1 2; printf X')" "|
|  X
|
cursor 3,1
exit 0" "tput clear and cup move where they say"

check "$($client $term 20 3 -- sh -c \
	'printf abcdef; tput cub 3; tput ech 2; tput nel; printf ac; tput cub1; tput ich 1; printf b; tput nel; tput rep 65 3')" "|abc  f
|abc
|AAA
cursor 3,2
exit 0" "erase, insert and repeat as the terminfo writes them"

check "$($client $term 20 3 -- sh -c \
	'printf "\033]2;a\007"; tput tsl; printf frost; tput fsl; printf x' | grep -c '^title frost$')" \
	"1" "the status line of the terminfo is the title"

if command -v ssh >/dev/null; then
	cfg=$(mktemp)
	printf 'Host casa\n\tTag tundra\n\nHost *\nInclude %s/ssh/frost.conf\n' \
		"$PWD" >"$cfg"
	sent() {
		TERM=$1 ssh -G -F "$cfg" "$2" 2>/dev/null | grep -i '^setenv' ||
			echo none
	}
	check "$(sent frost server):$(sent frost casa):$(sent xterm-256color server)" \
		"setenv TERM=xterm-256color:none:none" \
		"ssh says xterm-256color from frost, except to a machine tagged tundra"
	rm -f "$cfg"
else
	echo "# skip: no ssh"
fi

exit $fail
