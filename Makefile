# rtcollector for QNX
#
#   make                 host build (macOS / Linux): build/rtcollector
#   make test            unit + integration tests on the host
#   make test SANITIZE=1 with AddressSanitizer + UBSan
#   make tsan            the concurrency tests under ThreadSanitizer
#   make format-check    clang-format, must be clean
#   make lint            cppcheck, must be clean
#
#   make qnx ARCH=x86_64|aarch64le   QNX 8.0 build (source qnxsdp-env.sh first)

SHELL      := /bin/bash
.SHELLFLAGS := -o pipefail -c

CC      ?= cc
BUILD   ?= build
VERSION ?= 0.1.0-dev
BUILD_EPOCH := $(shell date +%s)

UNAME := $(shell uname -s)
PLATFORM ?= host
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

# Header dependency files. qcc ignores -MMD; pass it to the preprocessor.
DEPFLAGS = -MMD -MP
ifeq ($(PLATFORM),qnx)
  DEPFLAGS = -Wp,-MMD,$(@:.o=.d),-MT,$@,-MP
  # SDP 8.0 ships libcurl (shared, on the image) and libzstd (static here: the
  # mkqnximage image has no libzstd.so). libyaml is vendored. pthreads are in
  # libc on QNX: no -lpthread.
  DEPS_CFLAGS := -I$(QNX_TARGET)/usr/include/zstd -Ithird_party/libyaml/include -DYAML_DECLARE_STATIC -DRTC_QNX=1 \
                 -D_QNX_SOURCE
  YAML_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(wildcard third_party/libyaml/src/*.c))
  PLATFORM_SRC := $(wildcard src/qnx/*.c)
  QNX_LIBS := -lcurl -Wl,-Bstatic -lzstd -Wl,-Bdynamic -lm
endif

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
           -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -DRTC_BUILD_EPOCH=$(BUILD_EPOCH) -DRTC_VERSION=\"$(VERSION)\" $(DEPS_CFLAGS)
ifeq ($(PLATFORM),qnx)
  LDLIBS += $(QNX_LIBS)
else
  LDLIBS += $(DEPS_LIBS) -lyaml -lcurl -lzstd -lpthread -lm
endif

# Upstream code: no -Wall/-Werror of ours, but an implicit declaration must
# stay an error: under strict C11 without POSIX, strdup is undeclared, its
# result is truncated to int, and libyaml crashes on 64-bit. (No -w: with gcc
# it would silence these -Werror= too. libyaml 0.2.5 is clean without it.)
THIRD_PARTY_CFLAGS := -O2 -g -std=c11 -D_POSIX_C_SOURCE=200809L -Werror=implicit-function-declaration \
  -Werror=int-conversion -DYAML_DECLARE_STATIC -DYAML_VERSION_MAJOR=0 \
  -DYAML_VERSION_MINOR=2 -DYAML_VERSION_PATCH=5 -DYAML_VERSION_STRING=\"0.2.5\" -Ithird_party/libyaml/include

ifeq ($(SANITIZE),1)
  CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -DRTC_ASAN=1
  LDFLAGS += -fsanitize=address,undefined
endif
ifeq ($(SANITIZE),thread)
  CFLAGS  += -fsanitize=thread -O1
  LDFLAGS += -fsanitize=thread
endif

CORE_SRC := src/core/record.c src/core/ring.c src/core/msgpack.c src/core/config.c \
            src/core/cpumath.c src/core/inputs.c \
            src/core/log.c src/core/collector.c src/inputs/internal.c \
            src/outputs/arc_encode.c src/outputs/arc.c src/outputs/stdout.c
TEST_SRC := tests/main.c tests/test_record.c tests/test_ring.c tests/test_msgpack.c \
            tests/test_config.c tests/test_arc_encode.c tests/test_arc_http.c tests/fake_arc.c \
            tests/mp_read.c tests/test_cpumath.c

CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o) $(PLATFORM_SRC:%.c=$(BUILD)/%.o) $(YAML_OBJ)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)

.PHONY: all test tsan format format-check lint clean qnx

all: $(BUILD)/rtcollector

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(THIRD_PARTY_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

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

ARCH ?= x86_64
qnx:
	@test -n "$(QNX_TARGET)" || { echo "source qnxsdp-env.sh first"; exit 1; }
	$(MAKE) PLATFORM=qnx BUILD=build-qnx-$(ARCH) CC="qcc -Vgcc_nto$(ARCH)" \
	  build-qnx-$(ARCH)/rtcollector

clean:
	rm -rf build build-tsan build-asan build-qnx-*

-include $(CORE_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BUILD)/src/main.d
