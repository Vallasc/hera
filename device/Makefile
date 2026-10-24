# HERA device -- build for Raspberry Pi OS (ARMv6+).
#
# Dependencies (Debian / Raspberry Pi OS):
#   sudo apt install build-essential pkg-config \
#        libsystemd-dev libbluetooth-dev libsodium-dev \
#        hostapd dnsmasq wpasupplicant iw
#
# No audio codec library is needed: the protocol carries PCM or G.711, and
# G.711 is implemented in src/g711.c (PROTOCOL.md §6.1). libsodium provides the
# primitives of the encrypted profile (§11). Any libsodium version works;
# src/sec.c implements HKDF on top of HMAC-SHA256 because
# crypto_kdf_hkdf_sha256_* requires 1.0.19.
#
# Build:   make
# Run:     sudo -E ./hera-device          (reads .env / environment)
#
# Pi Zero W (ARMv6) tuning -- the default Raspberry Pi OS toolchain already
# targets ARMv6 with hard float; on a cross toolchain add:
#   make CFLAGS="-O2 -pipe -march=armv6zk -mfpu=vfp -mfloat-abi=hard"

CC      ?= cc
PKGS     = libsystemd libsodium

# Simply expanded, so pkg-config runs once and not once per source file.
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs $(PKGS) 2>/dev/null)

CFLAGS  ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=gnu11 -pthread $(PKG_CFLAGS)
LDFLAGS += -pthread
LDLIBS   = $(PKG_LIBS) -lbluetooth -lm

# Check the service dependencies up front. `make test` and `make tools` only
# build the portable modules and locate libsodium themselves (see below), so
# they are exempt.
GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
ifneq ($(filter-out test test-link tools clean,$(GOALS)),)
MISSING := $(strip $(foreach p,$(PKGS),\
             $(if $(shell pkg-config --exists $(p) 2>/dev/null && echo yes),,$(p))))
ifneq ($(MISSING),)
$(error missing development packages: $(MISSING) -- \
on Debian / Raspberry Pi OS: sudo apt install \
$(patsubst libsystemd,libsystemd-dev,$(patsubst libsodium,libsodium-dev,$(MISSING))))
endif
endif

BIN   = hera-device
SRC   = $(wildcard src/*.c)
OBJ   = $(SRC:.c=.o)
DEP   = $(OBJ:.o=.d)

# Host-side tests: portable modules only, no BlueZ required.
TEST_BIN = tests/test_proto
TEST_SRC = tests/test_proto.c src/proto.c src/resample.c src/stats.c \
           src/netcfg.c src/g711.c src/sec.c src/jitter.c src/util.c

# libsodium for host builds: pkg-config if available, otherwise the Homebrew
# prefix on Apple silicon.
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null || \
                         echo -I/opt/homebrew/opt/libsodium/include)
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null || \
                         echo -L/opt/homebrew/opt/libsodium/lib -lsodium)

# Bring-up client (tools/hera_probe). Portable: plain TCP and UDP sockets.
PROBE_BIN = tools/hera_probe
PROBE_SRC = tools/hera_probe.c src/proto.c src/sec.c src/util.c

.PHONY: test-audio test-tsan all clean test test-link tools

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Auto-generate header dependencies so edits trigger correct rebuilds.
src/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc \
	    $(SODIUM_CFLAGS) -o $@ $(TEST_SRC) $(SODIUM_LIBS) -lm

# Integration test of link.c over localhost. Linux only (eventfd).
LINK_TEST_BIN = tests/test_link
LINK_TEST_SRC = tests/test_link.c src/link.c src/proto.c src/sec.c src/util.c \
                src/log.c src/stats.c src/config.c

test-link: $(LINK_TEST_BIN)
	./$(LINK_TEST_BIN)

$(LINK_TEST_BIN): $(LINK_TEST_SRC)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc \
	    $(SODIUM_CFLAGS) -o $@ $(LINK_TEST_SRC) $(SODIUM_LIBS) -lm

# audio.c end to end over a fake SCO socket (Linux: needs the BlueZ headers).
AUDIO_TEST_SRC = tests/test_audio.c src/audio.c src/g711.c src/jitter.c \
                 src/resample.c src/proto.c src/util.c src/log.c src/stats.c \
                 src/config.c
test-audio: $(AUDIO_TEST_SRC)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread \
	    -Isrc -o tests/test_audio $(AUDIO_TEST_SRC) -lm
	./tests/test_audio

# The link test under ThreadSanitizer: reports any unsynchronised access
# between the link thread and the threads that send.
test-tsan: $(LINK_TEST_SRC)
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -Wall -Wextra \
	    -Wno-unused-parameter -pthread -Isrc $(SODIUM_CFLAGS) \
	    -o tests/test_link_tsan $(LINK_TEST_SRC) $(SODIUM_LIBS) -lm
	TSAN_OPTIONS=halt_on_error=1 HERA_TEST_PEER_TIMEOUT=6 ./tests/test_link_tsan

# The real link behind a scripted relay, for testing a client against it
# (tests/link_server.c).
tests/link_server: tests/link_server.c src/link.c src/proto.c src/sec.c \
                   src/util.c src/log.c src/stats.c src/config.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread \
	    -Isrc $(SODIUM_CFLAGS) -o $@ $^ $(SODIUM_LIBS) -lm

tools: $(PROBE_BIN)

$(PROBE_BIN): $(PROBE_SRC)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc \
	    $(SODIUM_CFLAGS) -o $@ $(PROBE_SRC) $(SODIUM_LIBS) -lm

clean:
	rm -f $(OBJ) $(DEP) $(BIN) $(TEST_BIN) $(LINK_TEST_BIN) $(PROBE_BIN)
