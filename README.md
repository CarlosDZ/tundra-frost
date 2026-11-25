# tundra-frost

Terminal of Tundra OS. Its design is in ADR-042 of
[Tundra-OS](https://github.com/CarlosDZ/Tundra-OS).

## Usage

```
frost [--app-id name] [program [argument...]]
frost --version
```

## Exit status

`0` the program ended well, `1` it failed or frost could not start, `2` invalid
usage.

## Tests

`make check-unit` builds and runs the unit tests with ASan and UBSan.
`make check` adds the tests of the built program.
