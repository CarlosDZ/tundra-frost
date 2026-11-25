# tundra-frost

Terminal of Tundra OS. Its design is in ADR-042 of
[Tundra-OS](https://github.com/CarlosDZ/Tundra-OS).

## Usage

```
frost [--app-id name] [program [argument...]]
frost --version
```

Each terminal runs in its own process, `frost-term`, installed in
`/usr/libexec`; frost starts it. They share the screen through a memfd and talk
over a socket.

## Exit status

`0` the program ended well, `1` it failed or frost could not start, `2` invalid
usage.

## Tests

`make check-unit` builds and runs the unit tests with ASan and UBSan.
`make check` adds the tests of the built programs: `tests/tools/client` plays
the window for a `frost-term` built with the sanitizers.

`tests/rec` holds what real programs wrote to a terminal (nvim, less, man,
htop, git, ls) and the screen each one leaves, reviewed once by eye; a unit test
replays them. `make tools` builds `replay`, which prints the screen a recording
leaves, and `bench-vt` and `bench-screen`, which measure the parser and the
screen. `tests/tools/gen-width.py` writes `src/width-table.h` from the Unicode
16 database of Python.
