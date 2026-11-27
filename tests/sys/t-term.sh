#!/bin/sh
term=${FROST_TERM:-build/test/frost-term}
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

check "$($client $term 20 3 -- sh -c 'printf "hola\nmundo"')" "|hola
|mundo
|
cursor 5,1
exit 0" "the program writes on the screen"

check "$($client $term 20 3 -- sh -c 'exit 3' | tail -1)" "exit 3" \
	"its exit status reaches the window"

check "$($client $term 20 3 -i 'abc\n' -- head -n1 | head -2)" "|abc
|abc" "typed input reaches the program"

check "$($client $term 20 3 -r 10 4 -- sh -c 'sleep 0.3; stty size' | head -1)" \
	"|4 10" "a resize reaches the program"

check "$($client $term 20 3 -- sh -c 'printf %s "$TERM"' | head -1)" "|frost" \
	"TERM is frost"

check "$($term -h 0 3 20 3 2>/dev/null; echo "$?")" "2" \
	"a history of 0 MB is a usage error"

check "$($client $term 30 3 -- sh -c 'printf "\033]2;t\303\255tulo\007"' | head -1)" \
	"title título" "the title reaches the window"

check "$($client $term 20 3 -- sh -c 'printf "\033[c"; sleep 0.2' | tail -1)" \
	"exit 0" "an answer to the program does not hang the terminal"

check "$($client $term 20 3 -- sh -c 'seq 1 3000 | tail -1' | head -1)" "|3000" \
	"a lot of output gets through"

check "$($client $term 20 3 -l 400 -- sh -c 'printf a; sleep 0.2; printf "\nb\nc"; sleep 0.5' | head -1)" \
	"dirty 0 1 2" "a window that answers late gets what was written meanwhile"

check "$($client $term 20 3 -w 300 -b 2 -- sh -c 'seq 1 10; sleep 0.5')" "|7
|8
|9
cursor 0,4
exit 0" "back in the history, the view shows its rows over the screen"

check "$($client $term 20 3 -w 300 -b 2 -- sh -c 'seq 1 10; sleep 0.4; seq 11 12; sleep 0.4')" "|7
|8
|9
cursor 0,5
exit 0" "and keeps showing them while the program writes on"

check "$($client $term 20 3 -w 300 -b 50 -- sh -c 'seq 1 10; sleep 0.5' | head -1)" "|1" \
	"it stops at the first line"

lines=$(awk 'BEGIN { for (i = 0; i < 1000; i++) printf "%099d\\n", 0 }')
got=$(mktemp)
$client $term 20 3 -i "$lines" -- sh -c \
	"sleep 0.5; head -c 100000 | wc -c >$got" >/dev/null
check "$(cat "$got")" "100000" \
	"100 KB typed into a program that reads late all reach it"
rm -f "$got"

start=$(date +%s%N)
last=$($client $term 20 3 -s 200 -- seq 1 100000 | grep -c '^|100000$')
ms=$((($(date +%s%N) - start) / 1000000))
check "$last $([ "$ms" -lt 5000 ] && echo fast || echo "$ms ms")" "1 fast" \
	"a window that paints slowly does not hold the program"
exit $fail
