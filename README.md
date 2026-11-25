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
over a socket. A terminal also listens in `$XDG_RUNTIME_DIR/frost/<number>`,
for its user only: a window attaches to it there, and it goes on without one
when its window detaches or falls. Once its program runs, frost-term locks
itself in seccomp: its pty, its sockets, its shared memory and the hang-up of
its program are all it keeps.

## Requirements

- libseccomp.
- tundra-libdraw, which paints the screen and reads the fonts and the theme.
  Built checkouts of tundra-libconf, tundra-libtoa and tundra-libdraw next to
  this one, or where `CONF_DEV`, `TOA_DEV` and `DRAW_DEV` say, are used
  instead of the installed ones.

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
