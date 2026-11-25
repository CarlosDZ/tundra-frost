#!/bin/sh
term=${FROST_TERM:-build/test/frost-term}
frost=build/test/frost
picture=build/test/tools/picture
client=build/test/tools/client
B=$BOREALIS_DEV
if [ -z "$B" ] || [ ! -x "$B/build/test/tundra-borealis" ] ||
	[ ! -x "$B/build/test/tools/ask" ]; then
	echo "# skip: no tundra-borealis built with its tests next to this one"
	exit 0
fi
ask="$B/build/test/tools/ask"
WORK=$(mktemp -d)
export XDG_RUNTIME_DIR=$WORK
export HOME=$WORK/home
export XDG_CONFIG_HOME=$HOME/.config
export TUNDRA_BOREALIS_ART=$B/build/art
export TUNDRA_BOREALIS_SYS=$WORK/sys
export WAYLAND_DISPLAY=wayland-0
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
export FROST_TEST_TERM=$term
fail=0
others=

check() {
	if [ "$1" = "$2" ]; then
		echo "ok - $3"
	else
		printf 'not ok - %s\n# got:\n%s\n# want:\n%s\n' "$3" "$1" "$2"
		fail=1
	fi
}

theme() {
	printf 'background = %s\ntext = d0d4d8\ncolor1 = e04040\n' "$1" >"$WORK/theme"
	printf 'color4 = 4060e0\nfrost.cursor = f0c040\n' >>"$WORK/theme"
	mv "$WORK/theme" "$XDG_CONFIG_HOME/tundra/theme"
}

windows() {
	"$ask" windows | grep '^window '
}

wait_window() {
	i=0
	until windows | awk -v a="$1" '$3 == a { f = 1 } END { exit !f }'; do
		i=$((i + 1))
		[ "$i" -gt 50 ] && return 1
		sleep 0.1
	done
}

grid_of() {
	windows | awk -v a="$1" '$3 == a { print int($9 / 24), int($8 / 16) }'
}

wait_end() {
	i=0
	while kill -0 "$1" 2>/dev/null; do
		i=$((i + 1))
		if [ "$i" -gt 50 ]; then
			kill -9 "$1"
			break
		fi
		sleep 0.1
	done
	wait "$1"
}

finish() {
	for p in $others; do
		kill "$p" 2>/dev/null
	done
	kill -TERM "$borealis" 2>/dev/null
	wait "$borealis" 2>/dev/null
	rm -rf "$WORK"
	exit $fail
}

mkdir -p "$XDG_CONFIG_HOME/tundra"
"$picture" font "$WORK/test.bdf"
printf 'mono = %s/test.bdf\n' "$WORK" >"$XDG_CONFIG_HOME/tundra/fonts"
theme 101418

"$B/build/test/tundra-borealis" >"$WORK/borealis.log" 2>&1 &
borealis=$!
i=0
while [ ! -S "$WORK/wayland-0" ] || [ ! -S "$WORK/borealis-test" ]; do
	i=$((i + 1))
	if [ "$i" -gt 50 ] || ! kill -0 "$borealis" 2>/dev/null; then
		echo "not ok - tundra-borealis starts"
		sed 's/^/# /' "$WORK/borealis.log"
		fail=1
		finish
	fi
	sleep 0.1
done

ref=tests/ref/window.txt
FROST_TEST_DUMP=$WORK/shown.png $frost sh -c \
	"head -c 210 $ref; sleep 0.3; tail -c +211 $ref; sleep 0.8"
check "$?" 0 "a program that ends well ends frost with 0"
check "$("$picture" same "$WORK/shown.png" tests/ref/window.png)" "" \
	"the window shows the screen as the reference paints it"

$frost sh -c 'exit 3'
check "$?" 1 "a program that fails ends frost with 1"

$frost --app-id frost-close sh -c 'sleep 30' &
win=$!
wait_window frost-close
check "$?" 0 "the window takes the app_id of --app-id"
"$ask" key 125 down >/dev/null
"$ask" key 14 down >/dev/null
"$ask" key 14 up >/dev/null
"$ask" key 125 up >/dev/null
wait_end $win
check "$?" 0 "closing the window hangs up its program, and frost ends with 0"

$frost --app-id frost-size sh -c \
	"stty size >$WORK/size1; sleep 2.5; stty size >$WORK/size2" &
win=$!
wait_window frost-size
check "$(cat "$WORK/size1")" "$(grid_of frost-size)" \
	"the program starts with the grid that fits the window"
"$B/build/test/tools/client" window other >/dev/null 2>&1 &
others="$others $!"
wait_window other
sleep 0.3
before=$(grid_of frost-size)
wait_end $win
check "$(cat "$WORK/size2")" "$before" \
	"another window beside it: the program gets the new grid"

FROST_TEST_DUMP=$WORK/theme.png $frost sh -c 'printf A; sleep 1.2' &
win=$!
sleep 0.6
theme 203040
wait_end $win
check "$("$picture" pixel "$WORK/theme.png" 0 0)" "ff203040" \
	"a new theme repaints the window"
theme 101418

$frost --app-id frost-fall sh -c 'printf tres; sleep 30' &
win=$!
wait_window frost-fall
sleep 0.2
kill -9 $win
wait $win 2>/dev/null
sleep 0.2
listed=$($client ls "$WORK/frost")
check "$(echo "$listed" | awk '{ print $3 }')" "detached" \
	"a window that falls leaves its terminal running, detached"
n=$(echo "$listed" | awk '{ print $1 }')
[ -n "$n" ] && $client attach "$WORK/frost/$n" 20 3 -c >/dev/null

finish
