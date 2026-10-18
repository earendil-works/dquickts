# Test hosts for heap snapshots and in-process suspension.
#
#   make check          build and run every test host (with self-contained
#                       and incremental snapshots), then hibernate scripts
#                       across processes (cross_process.sh)
#   make ASAN=1 check   same with AddressSanitizer + UBSan (separate build dir)
#   make stress         hibernate every 97 branches: test_hibernate on the
#                       scripts of js/ and on the upstream tests
#
# The engine sources are compiled directly (no Intl/Temporal, no
# quickjs-libc) so that the hosts exercise exactly the core runtime.

Q := ../..
CC ?= clang
VERSION := $(shell cat $(Q)/VERSION)

CFLAGS := -g -O1 -Wall -Wno-unused-parameter -Wno-sign-compare -fno-omit-frame-pointer \
          -funsigned-char -fwrapv -MMD -D_GNU_SOURCE -DCONFIG_VERSION=\"$(VERSION)\" -I$(Q) \
          $(EXTRA_CFLAGS)
LDLIBS := -lm -lpthread

ifdef ASAN
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined
LDFLAGS += -fsanitize=address,undefined
OBJDIR ?= build-asan
else
OBJDIR ?= build
endif

ENGINE := quickjs dtoa libregexp libunicode cutils
ENGINE_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(ENGINE)))

HOSTS := $(patsubst %.c,%,$(wildcard test_*.c))
HOST_BINS := $(addprefix $(OBJDIR)/,$(HOSTS))

SCRIPTS := $(wildcard js/*.js)
MODULES := $(wildcard js/*.mjs)
UPSTREAM_TESTS := $(addprefix $(Q)/tests/,test_closure.js test_loop.js \
                    test_language.js test_bigint.js test_builtin.js)

all: $(HOST_BINS) $(OBJDIR)/hibernate

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/%.o: $(Q)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/test_%.o: test_%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/test_%: $(OBJDIR)/test_%.o $(ENGINE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/hibernate.o: hibernate.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/hibernate: $(OBJDIR)/hibernate.o $(ENGINE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: all
	./$(OBJDIR)/test_suspend
	./$(OBJDIR)/test_snapshot_api
	./$(OBJDIR)/test_snapshot $(SCRIPTS) $(MODULES)
	./$(OBJDIR)/test_snapshot -d $(SCRIPTS) $(MODULES)
	./$(OBJDIR)/test_hibernate $(SCRIPTS) $(UPSTREAM_TESTS)
	./$(OBJDIR)/test_hibernate -d $(SCRIPTS) $(UPSTREAM_TESTS)
	sh cross_process.sh ./$(OBJDIR)/hibernate js/hibernate-sync.js \
	    js/control-async.js js/control-generators.js js/data-map.js \
	    js/control-using.js

stress:
	$(MAKE) OBJDIR=$(OBJDIR)-stress EXTRA_CFLAGS=-DJS_INTERRUPT_COUNTER_INIT=97 \
	    $(OBJDIR)-stress/test_hibernate
	./$(OBJDIR)-stress/test_hibernate $(SCRIPTS) $(UPSTREAM_TESTS)
	./$(OBJDIR)-stress/test_hibernate -d $(SCRIPTS) $(UPSTREAM_TESTS)

clean:
	rm -rf build build-asan build-stress build-asan-stress

.PHONY: all check stress clean
.SECONDARY:

-include $(wildcard $(OBJDIR)/*.d)
