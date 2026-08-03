# miniZip build
#
#   make            optimised build       -> build/minizip
#   make debug      -O0 -g + sanitizers   -> build/minizip
#   make no-threads serial build (no pthread dependency)
#   make test       build + run tests/run_tests.sh
#   make bench      compare against system zip(1), if installed
#   make install    install to $(PREFIX)/bin  (default /usr/local)

CC      ?= cc
PREFIX  ?= /usr/local
BUILD   ?= build

WARN     = -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
           -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
CFLAGS  ?= -O2
CFLAGS  += -std=c99 $(WARN)
LDLIBS   = -lz -lpthread

SRC = src/minizip.c
BIN = $(BUILD)/minizip

.PHONY: all debug no-threads test zip64 bench install uninstall clean

all: $(BIN)

$(BIN): $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

debug: CFLAGS := -std=c99 $(WARN) -O0 -g3 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN)

no-threads: CFLAGS += -DMZ_NO_THREADS
no-threads: LDLIBS  = -lz
no-threads: clean $(BIN)

test: $(BIN)
	@MINIZIP=$(abspath $(BIN)) sh tests/run_tests.sh

# Needs ~11 GB of scratch space, so it is not part of `make test`.
zip64: $(BIN)
	@MINIZIP=$(abspath $(BIN)) sh tests/zip64.sh

bench: $(BIN)
	@MINIZIP=$(abspath $(BIN)) sh tests/bench.sh

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/minizip

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/minizip

clean:
	rm -rf $(BUILD)
