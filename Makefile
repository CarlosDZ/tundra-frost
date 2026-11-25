CC = gcc
VERSION = $(shell grep '^version=' metapkg | cut -d= -f2)

TUNDRA_STD  = -std=c11
TUNDRA_DEFS = -D_GNU_SOURCE -DTUNDRA_FROST_VERSION=\"$(VERSION)\"
TUNDRA_INC  = -Isrc
TUNDRA_TERM_LIBS = -lseccomp

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
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(TERM_PROG): $(filter-out $(BUILD)/main.o,$(OBJS))
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
		-Itests/tools $(TUNDRA_DEFS) -o $@ $<

TEST_TOOLS = $(patsubst tests/tools/%.c,$(TEST_BUILD)/tools/%,$(wildcard tests/tools/*.c))

$(TEST_BUILD)/tools/%: tests/tools/%.c tests/tools/dump.h $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(TUNDRA_INC) -Itests/tools $(TUNDRA_DEFS) \
		-o $@ $< $(TUNDRA_TERM_LIBS)

tools: $(TEST_TOOLS)

check-unit: $(UNIT_TESTS)
	@rc=0; for t in $(UNIT_TESTS); do echo "# $$t"; $$t || rc=1; done; \
		exit $$rc

TEST_TERM = $(TEST_BUILD)/frost-term
TERM_SRCS = $(filter-out src/main.c,$(SRCS))

$(TEST_TERM): $(TERM_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		-DFROST_NO_SECCOMP -o $@ $(TERM_SRCS) $(TUNDRA_TERM_LIBS)

check: check-unit $(TARGET) $(TERM_PROG) $(TEST_TERM) $(TEST_TOOLS)
	@sh tests/sys/run.sh

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
