#!/bin/sh
frost=build/bench/frost
picture=build/test/tools/picture
B=$BOREALIS_DEV
if [ -z "$B" ] || [ ! -x "$B/build/test/tundra-borealis" ]; then
	echo "no tundra-borealis built with its tests next to this one" >&2
	exit 1
fi
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

mkdir -p "$XDG_CONFIG_HOME/tundra" "$XDG_CONFIG_HOME/foot"
"$picture" font "$WORK/test.bdf"
printf 'mono = %s/test.bdf\n' "$WORK" >"$XDG_CONFIG_HOME/tundra/fonts"
printf '[main]\nfont=monospace:pixelsize=24\n' >"$XDG_CONFIG_HOME/foot/foot.ini"
seq 1 30000000 | head -c 100000000 >"$WORK/short"
head -c 75000000 /dev/urandom | base64 -w 100 >"$WORK/long"

"$B/build/test/tundra-borealis" >"$WORK/borealis.log" 2>&1 &
borealis=$!
while [ ! -S "$WORK/wayland-0" ]; do
	sleep 0.1
done

now() {
	date +%s%N
}

best() {
	b=
	for i in 1 2 3; do
		t0=$(now)
		"$@" >/dev/null 2>&1
		ms=$((($(now) - t0) / 1000000))
		[ -z "$b" ] || [ "$ms" -lt "$b" ] && b=$ms
	done
	echo "$b"
}

printf '%-6s %8s %8s %8s\n' data script frost foot
for d in short long; do
	f=$WORK/$d
	pty=$(best script -qfc "cat $f" /dev/null)
	fr=$(best "$frost" cat "$f")
	ft=-
	command -v foot >/dev/null && ft=$(best foot cat "$f")
	printf '%-6s %6s ms %5s ms %5s ms\n' "$d" "$pty" "$fr" "$ft"
done

kill -TERM "$borealis"
wait "$borealis" 2>/dev/null
rm -rf "$WORK"
