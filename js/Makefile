# Builds quickjs.wasm: the engine and c/interface.c as a WASI reactor
# module, for the JavaScript bindings in src/.
#
# Needs wasi-sdk (https://github.com/WebAssembly/wasi-sdk). `make setup`
# downloads it to $(WASI_SDK); set WASI_SDK to use another installation.

WASI_SDK_VERSION = 32
WASI_SDK ?= $(HOME)/.cache/wasi-sdk/wasi-sdk-$(WASI_SDK_VERSION)
CC = $(WASI_SDK)/bin/clang
SYSROOT = $(WASI_SDK)/share/wasi-sysroot

# experimental Temporal and Intl (adds about 0.9 MB to quickjs.wasm)
#CONFIG_TEMPORAL_INTL=y

ROOT = ..
BUILD = build
OUTPUT = quickjs.wasm
OPT ?= -Os

SRCS = $(ROOT)/quickjs.c $(ROOT)/dtoa.c $(ROOT)/libregexp.c \
       $(ROOT)/libunicode.c $(ROOT)/cutils.c c/interface.c
ifdef CONFIG_TEMPORAL_INTL
SRCS += $(addprefix $(ROOT)/intl/, intl.c locale.c locale-data.c \
        numberformat.c collator.c datetimeformat.c pluralrules.c \
        listformat.c relativetimeformat.c displaynames.c segmenter.c \
        durationformat.c)
SRCS += $(addprefix $(ROOT)/temporal/, temporal.c instant.c duration.c \
        plaintime.c now.c plaindate.c plaindatetime.c plainyearmonth.c \
        plainmonthday.c zoneddatetime.c calendar.c timezone.c \
        tzdata_generated.c)
endif
OBJS = $(patsubst %.c,$(BUILD)/%.o,$(subst $(ROOT)/,,$(SRCS)))

CFLAGS = --target=wasm32-wasip1 --sysroot=$(SYSROOT) $(OPT) -flto \
         -I$(ROOT) -D_GNU_SOURCE \
         -DCONFIG_VERSION=\"$(shell cat $(ROOT)/VERSION)\" \
         -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_SIGNAL \
         -Wall -Wno-sign-compare -Wno-missing-field-initializers \
         -Wno-unused-parameter -Wno-unused-variable \
         -Wno-unused-but-set-variable -Wno-unused-function \
         -Wno-implicit-fallthrough -Wno-array-bounds
ifdef CONFIG_TEMPORAL_INTL
CFLAGS += -DCONFIG_TEMPORAL_INTL
endif

# 1 MiB of wasm stack (JS_SetMaxStackSize() defaults to half of it), placed
# first so that an overflow traps instead of overwriting data. -Os: about
# 30% smaller than -O2 and a few percent slower.
LDFLAGS = --target=wasm32-wasip1 --sysroot=$(SYSROOT) $(OPT) -flto \
          -mexec-model=reactor \
          -lwasi-emulated-process-clocks -lwasi-emulated-signal \
          -Wl,--wrap=__secs_to_zone \
          -Wl,-z,stack-size=1048576 -Wl,--stack-first \
          -Wl,--strip-debug

WASM_OPT ?= wasm-opt

.PHONY: all clean setup check-wasi-sdk ts test

all: $(OUTPUT)

check-wasi-sdk:
	@if [ ! -x "$(CC)" ]; then \
		echo "wasi-sdk not found at $(WASI_SDK): run 'make setup' or set WASI_SDK"; \
		exit 1; \
	fi

setup:
	@case "$$(uname -s)-$$(uname -m)" in \
		Linux-x86_64)  PLATFORM=x86_64-linux ;; \
		Linux-aarch64) PLATFORM=arm64-linux ;; \
		Darwin-arm64)  PLATFORM=arm64-macos ;; \
		Darwin-x86_64) PLATFORM=x86_64-macos ;; \
		*) echo "unsupported platform $$(uname -s)-$$(uname -m)"; exit 1 ;; \
	esac; \
	URL=https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$(WASI_SDK_VERSION)/wasi-sdk-$(WASI_SDK_VERSION).0-$$PLATFORM.tar.gz; \
	echo "downloading $$URL"; \
	rm -rf "$(WASI_SDK)" && mkdir -p "$(WASI_SDK)" && \
	curl -sSfL "$$URL" | tar xz -C "$(WASI_SDK)" --strip-components=1

$(OUTPUT): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	@if command -v $(WASM_OPT) >/dev/null 2>&1; then \
		$(WASM_OPT) $(OPT) --strip-debug --strip-producers $@ -o $@; \
	fi

$(BUILD)/c/interface.o: c/interface.c $(ROOT)/quickjs.h | check-wasi-sdk
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(ROOT)/%.c | check-wasi-sdk
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

ts:
	npx tsc -p tsconfig.json

test: $(OUTPUT)
	node --test test/*.test.ts

clean:
	rm -rf $(BUILD) $(OUTPUT) dist
