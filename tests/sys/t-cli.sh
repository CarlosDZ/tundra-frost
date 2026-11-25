#!/bin/sh
version=$(grep '^version=' metapkg | cut -d= -f2)
fail=0

check() {
	if [ "$1" = "$2" ]; then
		echo "ok - $3"
	else
		echo "not ok - $3 (got '$1', want '$2')"
		fail=1
	fi
}

check "$(./frost --version)" "tundra-frost $version" "--version prints the version"
./frost --help >/dev/null
check $? 0 "--help exits 0"
./frost -x 2>/dev/null
check $? 2 "an unknown option exits 2"
check "$(./frost -x 2>&1 >/dev/null | head -1)" \
	"usage: frost [--app-id name] [program [argument...]]" \
	"the usage goes to stderr"
exit $fail
