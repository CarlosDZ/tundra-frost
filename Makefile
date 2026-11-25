CC = gcc
VERSION = $(shell grep '^version=' metapkg | cut -d= -f2)

TUNDRA_STD  = -std=c11
TUNDRA_DEFS = -D_GNU_SOURCE -DTUNDRA_FROST_VERSION=\"$(VERSION)\"
TUNDRA_INC  = -Isrc
TUNDRA_TERM_LIBS = -lseccomp
TUNDRA_LIBS = -ltundra-draw

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

all: $(TARGET) $(TERM_PROG)

$(TARGET): $(filter-out $(BUILD)/term.o $(BUILD)/sandbox.o,$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(TUNDRA_LIBS)

$(TERM_PROG): $(filter-out $(BUILD)/main.o $(BUILD)/render.o,$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(TUNDRA_TERM_LIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CPPFLAGS) $(CFLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		-MMD -MP -c $< -o $@

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

$(TEST_BUILD)/tools/%: tests/tools/%.c tests/tools/dump.h $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(TUNDRA_INC) -Itests/tools $(TUNDRA_DEFS) \
		-o $@ $< $(TUNDRA_TERM_LIBS)

tools: $(TEST_TOOLS)

check-unit: $(UNIT_TESTS)
	@rc=0; for t in $(UNIT_TESTS); do echo "# $$t"; $(TEST_ENV) $$t || rc=1; done; \
		exit $$rc

TEST_TERM = $(TEST_BUILD)/frost-term
TERM_SRCS = $(filter-out src/main.c src/render.c,$(SRCS))

$(TEST_TERM): $(TERM_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		-DFROST_NO_SECCOMP -o $@ $(TERM_SRCS) $(TUNDRA_TERM_LIBS)

check: check-unit $(TARGET) $(TERM_PROG) $(TEST_TERM) $(TEST_TOOLS)
	@$(TEST_ENV) sh tests/sys/run.sh

.PHONY: all install uninstall clean check check-unit tools

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
