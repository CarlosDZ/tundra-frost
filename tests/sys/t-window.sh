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

chord() {
	for c in "$@"; do
		"$ask" key "$c" down >/dev/null
	done
	for c in $(printf '%s\n' "$@" | sed -n '1!G;h;$p'); do
		"$ask" key "$c" up >/dev/null
	done
}

type_text() {
	for k in $(printf '%s' "$1" | awk '
		BEGIN {
			s = "abcdefghijklmnopqrstuvwxyz"
			split("30 48 46 32 18 33 34 35 23 36 37 38 50 49 24 25 16 19 " \
			      "31 20 22 47 17 45 21 44", c, " ")
		}
		{
			for (i = 1; i <= length($0); i++) {
				ch = substr($0, i, 1)
				if (ch == " ")
					print 57
				else if (ch ~ /[0-9]/)
					print (ch == "0" ? 11 : ch + 1)
				else
					print c[index(s, ch)]
			}
		}'); do
		chord $k
	done
}

line_order() {
	chord 29 42 57
	type_text "$1"
	chord 28
	sleep 0.4
}

state_of() {
	$client ls "$WORK/frost" | awk -v n="$1" '$2 == n { print $3 }'
}

wait_file() {
	i=0
	until [ -e "$1" ]; do
		i=$((i + 1))
		[ "$i" -gt 50 ] && return 1
		sleep 0.1
	done
}

hex() {
	od -An -tx1 "$1" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//'
}

reader() {
	app=$1 setup=$2 read=$3
	$frost --app-id "$app" sh -c \
		"$setup stty raw -echo; : >$WORK/$app.ready; $read >$WORK/$app" &
	win=$!
	wait_window "$app" && wait_file "$WORK/$app.ready"
	sleep 0.3
}

point() {
	cx=${2:-4} cy=${3:-2}
	set -- $(windows | awk -v a="$1" '$3 == a { print $6, $7, $8, $9 }')
	cols=$(($3 / 16)) rows=$(($4 / 24))
	"$ask" move $(($1 + ($3 - cols * 16) / 2 + cx * 16 + 8)) \
		$(($2 + ($4 - rows * 24) / 2 + cy * 24 + 12)) >/dev/null
}

keyboard_on() {
	"$ask" focus | awk '$1 == "focus" { print $3 }'
}

close_detached() {
	for n in $($client ls "$WORK/frost" | awk '$3 == "detached" { print $1 }'); do
		$client attach "$WORK/frost/$n" 20 3 -c >/dev/null
	done
}

click() {
	"$ask" button down >/dev/null
	"$ask" button up >/dev/null
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

reader keys "" "head -c 11"
chord 29 42 46
chord 30
chord 42 30
chord 29 46
chord 103
chord 63
wait_end $win
check "$(hex "$WORK/keys")" "61 41 03 1b 5b 41 1b 5b 31 35 7e" \
	"keys reach the program as xterm sends them, Ctrl + Shift + C not"

reader kitty "printf '\\033[>1u';" "head -c 12"
chord 1
chord 29 46
wait_end $win
check "$(hex "$WORK/kitty")" "1b 5b 32 37 75 1b 5b 39 39 3b 35 75" \
	"a program that asks for kitty's keys gets them: Esc and Ctrl + C"

reader repeat "" "head -c 5"
"$ask" key 30 down >/dev/null
sleep 1.2
"$ask" key 30 up >/dev/null
wait_end $win
check "$(cat "$WORK/repeat")" "aaaaa" "a key held down repeats"

reader focus "printf '\\033[?1004h';" "timeout --foreground 3 cat"
"$B/build/test/tools/client" window other2 >/dev/null 2>&1 &
others="$others $!"
wait_end $win
check "$(hex "$WORK/focus" | grep -o '1b 5b 4f$')" "1b 5b 4f" \
	"a program that asks hears the window lose the focus"

reader mouse "printf '\\033[?1000h\\033[?1006h';" "head -c 18"
point mouse
"$ask" button down >/dev/null
"$ask" button up >/dev/null
wait_end $win
check "$(hex "$WORK/mouse")" \
	"1b 5b 3c 30 3b 35 3b 33 4d 1b 5b 3c 30 3b 35 3b 33 6d" \
	"a click reaches the program that asks, in SGR, on its cell"

reader wheel "printf '\\033[?1049h';" "head -c 6"
point wheel
"$ask" scroll 48 >/dev/null
wait_end $win
check "$(hex "$WORK/wheel")" "1b 5b 42 1b 5b 42" \
	"in the alternate screen the touchpad is arrows, a line a cell"

reader wheelm "printf '\\033[?1000h\\033[?1006h';" "head -c 20"
point wheelm
"$ask" scroll -48 >/dev/null
wait_end $win
check "$(hex "$WORK/wheelm")" \
	"1b 5b 3c 36 34 3b 35 3b 33 4d 1b 5b 3c 36 34 3b 35 3b 33 4d" \
	"to a program that asks, the touchpad is the wheel, a line a cell"

FROST_TEST_LOG=$WORK/log.clipa $frost --app-id clipa sh -c \
	'printf "hola mundo"; sleep 4' &
clipa=$!
wait_window clipa
sleep 0.3
export FROST_TEST_LOG=$WORK/log.clipb
reader clipb "printf '\\033[?2004h';" "head -c 17"
unset FROST_TEST_LOG
point clipa 7 0
on_a=$(keyboard_on)
click
click
chord 29 42 46
point clipb
on_b=$(keyboard_on)
chord 29 42 47
wait_end $win
want="1b 5b 32 30 30 7e 6d 75 6e 64 6f 1b 5b 32 30 31 7e"
if [ "$(hex "$WORK/clipb")" != "$want" ]; then
	echo "# keyboard on $on_a, then on $on_b"
	sed 's/^/# clipa: /' "$WORK/log.clipa" 2>/dev/null
	sed 's/^/# clipb: /' "$WORK/log.clipb" 2>/dev/null
fi
check "$(hex "$WORK/clipb")" "$want" \
	"a word copied with two clicks in one window is pasted in another"
wait_end $clipa

$frost --app-id osc sh -c \
	'sleep 1; printf "\033]52;c;%s\007" "$(printf "x\033[201~y" | base64)"; sleep 3' &
osc=$!
wait_window osc
point osc
click
sleep 1.2
reader oscb "printf '\\033[?2004h';" "head -c 19"
point oscb
chord 29 42 47
wait_end $win
check "$(hex "$WORK/oscb")" \
	"1b 5b 32 30 30 7e 78 5b 32 30 31 7e 79 1b 5b 32 30 31 7e" \
	"OSC 52 writes the clipboard, and a pasted ESC [ 201 ~ does not end the paste"
wait_end $osc

printf '#!/bin/sh\nprintf "%%s\\n" "$1" >>"%s/opened"\n' "$WORK" >"$WORK/browser"
chmod +x "$WORK/browser"
BROWSER=$WORK/browser $frost --app-id links sh -c \
	'printf "\033]8;;https://tundra.example/o\007enlace\033]8;;\007 y https://tundra.example/b.\n"; sleep 3' &
links=$!
wait_window links
sleep 0.3
for cell in 2 14; do
	point links $cell 0
	"$ask" key 29 down >/dev/null
	click
	"$ask" key 29 up >/dev/null
done
sleep 0.5
check "$(cat "$WORK/opened" 2>/dev/null)" "https://tundra.example/o
https://tundra.example/b" \
	"Ctrl + click opens an OSC 8 link and an address written in the line"
wait_end $links

reader tabs "" "head -c 1"
chord 29 42 57
for k in 49 18 17 28; do
	chord $k
done
sleep 0.5
chord 29 42 57
chord 2
chord 28
sleep 0.3
chord 45
sleep 0.3
check "$(cat "$WORK/tabs")" "x" \
	"the line opens a tab, and a number goes back to the first"
chord 125 14
wait_end $win
check "$?" 0 "closing the window ends the program of every tab"

reader keep "" "head -c 1"
line_order "new"
line_order "detach work"
check "$(state_of work)" "detached" "detach takes a tab out, with its name"
line_order "attach work"
check "$(state_of work)" "attached" "attach brings it back by its name"
chord 125 14
wait_end $win
check "$?" 0 "and the window closes them both"

$frost --app-id solo sh -c 'sleep 30' &
solo=$!
wait_window solo
sleep 0.3
line_order "detach solo"
wait_end $solo
check "$?:$(state_of solo)" "0:detached" \
	"detaching the last tab closes the window and keeps its terminal"
close_detached

mkdir -p "$WORK/aqui"
printf '#!/bin/sh\npwd >"%s/cwd"\nexec sleep 5\n' "$WORK" >"$WORK/where"
chmod +x "$WORK/where"
SHELL=$WORK/where $frost --app-id cwd sh -c \
	'cd "$0" && printf "\033]7;file://localhost%s\007" "$PWD"; sleep 5' \
	"$WORK/aqui" &
cwdw=$!
wait_window cwd
sleep 0.3
line_order "new"
sleep 0.5
check "$(cat "$WORK/cwd" 2>/dev/null)" "$WORK/aqui" \
	"a new tab opens in the directory of the tab shown, as OSC 7 said"
chord 125 14
wait_end $cwdw

FROST_TEST_DUMP=$WORK/back.png $frost --app-id back sh -c \
	'echo B; for i in $(seq 100); do echo A; done; sleep 1.5' &
win=$!
wait_window back
sleep 0.3
point back
"$ask" scroll -100000 >/dev/null
wait_end $win
printf 'B\r\nA\r\nA\r\nA\r\nA\r\nA\033[?25l' >"$WORK/back.txt"
"$picture" paint "$WORK/back.txt" 20 6 "$WORK/back-want.png"
check "$("$picture" same "$WORK/back.png" "$WORK/back-want.png")" "" \
	"the touchpad goes back to the first line of the history"

close_detached
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
