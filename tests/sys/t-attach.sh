#!/bin/sh
term=build/test/frost-term
client=build/test/tools/client
XDG_RUNTIME_DIR=$(mktemp -d)
export XDG_RUNTIME_DIR
dir=$XDG_RUNTIME_DIR/frost
fail=0

check() {
	if [ "$1" = "$2" ]; then
		echo "ok - $3"
	else
		printf 'not ok - %s\n# got:\n%s\n# want:\n%s\n' "$3" "$1" "$2"
		fail=1
	fi
}

check "$($client $term 20 3 -d -- sh -c 'printf uno; sleep 30')" "number 1" \
	"a terminal detaches and says its number"
check "$($client ls "$dir")" "1 - detached -" "it goes on, listed as detached"
check "$(stat -c %a "$dir")" "700" "its directory is private"
check "$($client attach "$dir/1" 20 3 | head -1)" "|uno" \
	"attaching brings its screen back"

$client attach "$dir/1" 20 3 -n build >/dev/null
check "$($client ls "$dir")" "1 build detached -" "a name is kept"

$client attach "$dir/1" 20 3 -w 800 >/dev/null &
holder=$!
sleep 0.3
check "$($client attach "$dir/1" 20 3)" "busy" \
	"a terminal with a window is not taken by another"
wait $holder

$client $term 20 3 -- sh -c 'printf dos; sleep 30' >/dev/null &
win=$!
sleep 0.3
kill -9 $win
wait $win 2>/dev/null
sleep 0.2
check "$($client ls "$dir" | sort)" "1 build detached -
2 - detached -" "when its window falls, a terminal goes on detached"

check "$($client attach "$dir/2" 20 3 -c | tail -1)" "exit 129" \
	"closing it hangs up the program"
$client attach "$dir/1" 20 3 -c >/dev/null
sleep 0.2
check "$(ls "$dir")" "" "a terminal that ends takes its socket away"

rm -rf "$XDG_RUNTIME_DIR"
exit $fail
