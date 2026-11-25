CC = gcc
VERSION = $(shell grep '^version=' metapkg | cut -d= -f2)

TUNDRA_STD  = -std=c11
TUNDRA_DEFS = -D_GNU_SOURCE -DTUNDRA_FROST_VERSION=\"$(VERSION)\"
TUNDRA_INC  = -Isrc

CFLAGS ?= -Wall -Wextra -O2

TARGET = frost

BUILD = build
SRCS = $(wildcard src/*.c)
HDRS = $(wildcard src/*.h)
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CPPFLAGS) $(CFLAGS) $(TUNDRA_INC) $(TUNDRA_DEFS) \
		-MMD -MP -c $< -o $@

-include $(DEPS)

$(OBJS): Makefile metapkg

clean:
	rm -rf $(BUILD) $(TARGET)

TEST_BUILD = $(BUILD)/test
UNIT_FLAGS = -g -fsanitize=address,undefined -fno-omit-frame-pointer
UNIT_TESTS = $(patsubst tests/unit/%.c,$(TEST_BUILD)/unit/%,$(wildcard tests/unit/*.c))

$(TEST_BUILD)/unit/%: tests/unit/%.c tests/unit/test.h $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TUNDRA_STD) $(CFLAGS) $(UNIT_FLAGS) $(TUNDRA_INC) -Itests/unit \
		$(TUNDRA_DEFS) -o $@ $<

check-unit: $(UNIT_TESTS)
	@rc=0; for t in $(UNIT_TESTS); do echo "# $$t"; $$t || rc=1; done; \
		exit $$rc

check: check-unit $(TARGET)
	@sh tests/sys/run.sh

.PHONY: all install uninstall clean check check-unit

PREFIX ?= /usr
DESTDIR ?=

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/tundra-frost/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -rf $(DESTDIR)$(PREFIX)/share/licenses/tundra-frost
