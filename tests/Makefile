# Host tests and UI preview (run from the repo root: make -C tests)
OO_PS4_TOOLCHAIN ?= /opt/oo/OpenOrbis/PS4Toolchain
STB ?= $(OO_PS4_TOOLCHAIN)/include/stb
SRC := ../src
CC  ?= gcc
CFLAGS := -g -O1 -Wall -Wno-unused-result -I$(STB)
COMMON := $(SRC)/chat.c $(SRC)/config.c $(SRC)/font.c $(SRC)/json.c $(SRC)/layout.c $(SRC)/rich.c \
          $(SRC)/strbuf.c $(SRC)/plat_stub.c $(SRC)/history.c

.PHONY: all test preview phone
all: test preview

test:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o test_host test_host.c $(COMMON) $(SRC)/stream.c $(SRC)/tools.c $(SRC)/setup.c -DCONFIG_SAVE_DIR='"/tmp/claude-ps4-test"' -lpthread -lm
	mkdir -p /tmp/claude-ps4-test && cd .. && ASAN_OPTIONS=detect_leaks=0 tests/test_host

preview:
	$(CC) $(CFLAGS) -w -o preview preview.c $(COMMON) $(SRC)/ui.c $(SRC)/setup.c $(SRC)/qrcodegen.c -lpthread -lm
	cd .. && tests/preview tests

# Serves the phone page on http://127.0.0.1:8765 with a fake chat (PIN 123456).
phone:
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o phone_server phone_server.c $(COMMON) $(SRC)/setup.c -DCONFIG_SAVE_DIR='"/tmp/claude-ps4-test"' -lpthread -lm
	./phone_server 123456
