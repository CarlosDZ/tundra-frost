CC = gcc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
VERSION = $(shell grep '^version=' metapkg | cut -d= -f2)

TUNDRA_STD  = -std=c11
TUNDRA_DEFS = -D_GNU_SOURCE -DTUNDRA_FROST_VERSION=\"$(VERSION)\" \
              -DTUNDRA_FROST_TERM=\"$(PREFIX)/libexec/frost-term\"
TUNDRA_INC  = -Isrc -I$(PROTO)
TUNDRA_TERM_LIBS = -lseccomp
TUNDRA_WL_PKGS = wayland-client wayland-cursor xkbcommon
TUNDRA_WL_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(TUNDRA_WL_PKGS))
TUNDRA_LIBS = -ltundra-draw $(shell $(PKG_CONFIG) --libs $(TUNDRA_WL_PKGS))

CONF_DEV ?= $(wildcard ../tundra-libconf)
DRAW_DEV ?= $(wildcard ../tundra-libdraw)
TOA_DEV ?= $(wildcard ../tundra-libtoa)
DEV_BUILDS   = $(foreach d,$(CONF_DEV) $(DRAW_DEV) $(TOA_DEV),$(abspath $(d))/build)
TUNDRA_INC  += $(foreach d,$(DRAW_DEV),-I$(d)/include)
TUNDRA_LIBS := $(DEV_BUILDS:%=-L%) $(DEV_BUILDS:%=-Wl,-rpath-link,%) \
               $(TUNDRA_LIBS)
TEST_RPATH   = $(DEV_BUILDS:%=-Wl,-rpath,%)
empty :=
TEST_ENV = $(if $(DEV_BUILDS),LD_LIBRARY_PATH=$(subst $(empty) ,:,$(strip $(DEV_BUILDS))))

CFLAGS ?= -Wall -Wextra -O2

TARGET = frost
TERM_PROG = frost-term

BUILD = build
SRCS = $(wildcard src/*.c)
HDRS = $(wildcard src/*.h)
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS = $(OBJS:.o=.d)

WIN_SRCS  = src/main.c src/win.c src/render.c src/keys.c src/kbd.c \
            src/mouse.c src/paste.c src/select.c
TERM_SRCS = src/term.c src/sandbox.c

PROTO = $(BUILD)/protocols
WL_PROTOCOLS = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_DECO_XML = $(WL_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
PROTO_NAMES = xdg-shell xdg-decoration-unstable-v1
PROTO_HDRS = $(PROTO_NAMES:%=$(PROTO)/%-client-protocol.h)
PROTO_SRCS = $(PROTO_NAMES:%=$(PROTO)/%-protocol.c)
PROTO_OBJS = $(PROTO_SRCS:.c=.o)

all: $(TARGET) $(TERM_PROG)

$(TARGET): $(filter-out $(TERM_SRCS:src/%.c=$(BUILD)/%.o),$(OBJS)) $(PROTO_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(TUNDRA_LIBS)

$(TERM_PROG): $(filter-out $(WIN_SRCS:src/%.c=$(BUILD)/%.o),$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(TUNDRA_TERM_LIBS)

$(BUILD)/%.o: src/%.c | $(PROTO_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CPPFLAGS) $(CFLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		$(TUNDRA_WL_CFLAGS) -MMD -MP -c $< -o $@

$(PROTO)/xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	@mkdir -p $(PROTO)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
	@mkdir -p $(PROTO)
	$(WAYLAND_SCANNER) private-code $< $@

$(PROTO)/xdg-decoration-unstable-v1-client-protocol.h: $(XDG_DECO_XML)
	@mkdir -p $(PROTO)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO)/xdg-decoration-unstable-v1-protocol.c: $(XDG_DECO_XML)
	@mkdir -p $(PROTO)
	$(WAYLAND_SCANNER) private-code $< $@

$(PROTO)/%.o: $(PROTO)/%.c
	$(CC) $(TUNDRA_STD) $(CPPFLAGS) $(CFLAGS) $(TUNDRA_WL_CFLAGS) -c $< -o $@

-include $(DEPS)

$(OBJS): Makefile metapkg

clean:
	rm -rf $(BUILD) $(TARGET) $(TERM_PROG)

TEST_BUILD = $(BUILD)/test
UNIT_FLAGS = -g -fsanitize=address,undefined -fno-omit-frame-pointer
UNIT_TESTS = $(patsubst tests/unit/%.c,$(TEST_BUILD)/unit/%,$(wildcard tests/unit/*.c))

$(TEST_BUILD)/unit/%: tests/unit/%.c tests/unit/test.h tests/tools/dump.h \
		$(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) -Itests/unit \
		-Itests/tools $(TUNDRA_DEFS) -o $@ $< $(TUNDRA_LIBS) $(TEST_RPATH)

TEST_TOOLS = $(patsubst tests/tools/%.c,$(TEST_BUILD)/tools/%,$(wildcard tests/tools/*.c))

$(TEST_BUILD)/tools/%: tests/tools/%.c tests/tools/dump.h tests/unit/bdf-write.h \
		$(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(TUNDRA_INC) -Itests/tools -Itests/unit \
		$(TUNDRA_DEFS) -o $@ $< $(TUNDRA_TERM_LIBS) $(TUNDRA_LIBS) $(TEST_RPATH)

tools: $(TEST_TOOLS)

check-unit: $(UNIT_TESTS)
	@rc=0; for t in $(UNIT_TESTS); do echo "# $$t"; $(TEST_ENV) $$t || rc=1; done; \
		exit $$rc

TEST_TERM = $(TEST_BUILD)/frost-term
TEST_TERM_SRCS = $(filter-out $(WIN_SRCS),$(SRCS))

$(TEST_TERM): $(TEST_TERM_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		-DFROST_NO_SECCOMP -o $@ $(TEST_TERM_SRCS) $(TUNDRA_TERM_LIBS)

TEST_WIN = $(TEST_BUILD)/frost
TEST_WIN_SRCS = $(filter-out $(TERM_SRCS),$(SRCS))

$(TEST_WIN): $(TEST_WIN_SRCS) $(HDRS) $(PROTO_SRCS) | $(PROTO_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		$(TUNDRA_WL_CFLAGS) -DFROST_TEST -o $@ $(TEST_WIN_SRCS) $(PROTO_SRCS) \
		$(TUNDRA_LIBS) $(TEST_RPATH)

BOREALIS_DEV ?= $(wildcard ../tundra-borealis)

BENCH_WIN = $(BUILD)/bench/frost

$(BENCH_WIN): $(TEST_WIN_SRCS) $(HDRS) $(PROTO_SRCS) | $(PROTO_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		-UTUNDRA_FROST_TERM -DTUNDRA_FROST_TERM=\"$(abspath $(TERM_PROG))\" \
		$(TUNDRA_WL_CFLAGS) -o $@ $(TEST_WIN_SRCS) $(PROTO_SRCS) \
		$(TUNDRA_LIBS) $(TEST_RPATH)

bench: $(BENCH_WIN) $(TERM_PROG) $(TEST_TOOLS)
	@$(TEST_ENV) BOREALIS_DEV=$(abspath $(BOREALIS_DEV)) sh tests/sys/bench-window.sh

check: check-unit $(TARGET) $(TERM_PROG) $(TEST_TERM) $(TEST_WIN) $(TEST_TOOLS)
	@$(TEST_ENV) BOREALIS_DEV=$(abspath $(BOREALIS_DEV)) sh tests/sys/run.sh

.PHONY: all install uninstall clean check check-unit tools bench

PREFIX ?= /usr
DESTDIR ?=

install: $(TARGET) $(TERM_PROG)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm755 $(TERM_PROG) $(DESTDIR)$(PREFIX)/libexec/$(TERM_PROG)
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/tundra-frost/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/libexec/$(TERM_PROG)
	rm -rf $(DESTDIR)$(PREFIX)/share/licenses/tundra-frost
