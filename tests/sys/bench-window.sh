#!/bin/sh
frost=build/bench/frost
picture=build/test/tools/picture
B=$BOREALIS_DEV
ask=$B/build/test/tools/ask
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

latency() {
	"$@" sh -c "printf '\\033[2 q'; exec cat" >/dev/null 2>&1 &
	pid=$!
	sleep 1.5
	for i in $(seq 50); do
		"$ask" latency 30 | awk '$1 == "latency" { print $2 }'
		sleep 0.05
	done | sort -n | awk '{ a[NR] = $1 }
		END { if (NR) printf "%6d us %6d us\n", a[int((NR + 1) / 2)], a[NR]
		      else print "     -" }'
	for k in "125 down" "14 down" "14 up" "125 up"; do
		"$ask" key $k >/dev/null
	done
	wait "$pid" 2>/dev/null
}

printf '\n%-7s %9s %9s\n' latency median worst
printf '%-7s %s\n' frost "$(latency "$frost")"
command -v foot >/dev/null && printf '%-7s %s\n' foot "$(latency foot)"

kill -TERM "$borealis"
wait "$borealis" 2>/dev/null
rm -rf "$WORK"
