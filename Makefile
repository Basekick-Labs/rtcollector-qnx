# rtcollector for QNX
#
#   make                 host build (macOS / Linux): build/rtcollector
#   make test            unit + integration tests on the host
#   make test SANITIZE=1 with AddressSanitizer + UBSan
#   make tsan            the concurrency tests under ThreadSanitizer
#   make format-check    clang-format, must be clean
#   make lint            cppcheck, must be clean
#
# QNX targets (make qnx ARCH=aarch64le|x86_64) arrive with the QNX layer.

SHELL      := /bin/bash
.SHELLFLAGS := -o pipefail -c

CC      ?= cc
BUILD   ?= build
VERSION ?= 0.1.0-dev
BUILD_EPOCH := $(shell date +%s)

UNAME := $(shell uname -s)
# Linux kernels with high mmap randomisation crash TSan at startup; run it
# with address randomisation off.
TSAN_RUN :=
ifeq ($(UNAME),Linux)
  TSAN_RUN := setarch $(shell uname -m) -R
endif
ifeq ($(UNAME),Darwin)
  BREW    := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
  DEPS_CFLAGS := -I$(BREW)/include
  DEPS_LIBS   := -L$(BREW)/lib
endif

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
           -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -DRTC_BUILD_EPOCH=$(BUILD_EPOCH) -DRTC_VERSION=\"$(VERSION)\" $(DEPS_CFLAGS)
LDLIBS  += $(DEPS_LIBS) -lyaml -lcurl -lzstd -lpthread -lm

ifeq ($(SANITIZE),1)
  CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -DRTC_ASAN=1
  LDFLAGS += -fsanitize=address,undefined
endif
ifeq ($(SANITIZE),thread)
  CFLAGS  += -fsanitize=thread -O1
  LDFLAGS += -fsanitize=thread
endif

CORE_SRC := src/core/record.c src/core/ring.c src/core/msgpack.c src/core/config.c \
            src/core/log.c src/core/collector.c src/inputs/internal.c \
            src/outputs/arc_encode.c src/outputs/arc.c src/outputs/stdout.c
TEST_SRC := tests/main.c tests/test_record.c tests/test_ring.c tests/test_msgpack.c \
            tests/test_config.c tests/test_arc_encode.c tests/test_arc_http.c tests/fake_arc.c \
            tests/mp_read.c

CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)

.PHONY: all test tsan format format-check lint clean

all: $(BUILD)/rtcollector

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/rtcollector: $(CORE_OBJ) $(BUILD)/src/main.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/rtc_tests: $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

test: $(BUILD)/rtc_tests $(BUILD)/rtcollector
	$(BUILD)/rtc_tests 2>&1 | grep -vE ' (error|warn) (config|arc):'

tsan:
	$(MAKE) BUILD=build-tsan SANITIZE=thread build-tsan/rtc_tests
	$(TSAN_RUN) build-tsan/rtc_tests ring
	$(TSAN_RUN) build-tsan/rtc_tests lifecycle

FMT_FILES := $(shell find src tests -name '*.[ch]')

format:
	clang-format -i $(FMT_FILES)

format-check:
	@clang-format --dry-run --Werror $(FMT_FILES)

lint:
	cppcheck --quiet --error-exitcode=1 --enable=warning,performance,portability --std=c11 \
	  --suppress=missingIncludeSystem --inline-suppr -I src src tests

clean:
	rm -rf build build-tsan

-include $(CORE_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
