# Vellum - build the VM library, tools, tests, and local fuzz drivers.
#
#   make            build the library and tools
#   make test       build and run the unit tests
#   make fuzz-local build the fuzz harnesses with a standalone driver + page-guard
#                   allocator, so a proof-of-concept module can be replayed locally
#   make SAN=1 ...   add AddressSanitizer/UBSan (needs a sanitizer-capable compiler)
#   make clean
#
# The ClusterFuzzLite build (.clusterfuzzlite/build.sh) builds each harness
# against libFuzzer; this Makefile is for local development.

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g -Wall -Wextra
CPPFLAGS ?=
INCLUDES  = -Iinclude -Isrc -Itests

ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
ifeq ($(GUARD),1)
CPPFLAGS += -DVL_GUARD_ALLOC
endif

BUILD    = build
LIB_SRC  = $(wildcard src/*.c)
LIB_OBJ  = $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRC))
LIB      = $(BUILD)/libvellum.a

TOOL_SRC = $(wildcard tools/*.c)
TOOLS    = $(patsubst tools/%.c,$(BUILD)/%,$(TOOL_SRC))

TEST_SRC = $(wildcard tests/test_*.c)
TESTS    = $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

FUZZ_SRC = $(wildcard fuzz/*_fuzzer.c)

.PHONY: all lib tools test fuzz-local clean
all: lib tools

lib: $(LIB)
$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) -c $< -o $@

tools: $(TOOLS)
$(BUILD)/%: tools/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $< $(LIB) -o $@

test: $(TESTS)
	@echo "== running tests =="
	@fail=0; for t in $(TESTS); do echo "-- $$t"; "$$t" || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TEST FAILURES"; exit 1; fi

$(BUILD)/test_%: tests/test_%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $< $(LIB) -o $@

# Local, engine-free fuzz drivers: guard allocator + standalone main, so a PoC
# module can be replayed and a memory-safety fault observed without libFuzzer.
fuzz-local: $(patsubst fuzz/%.c,$(BUILD)/%_local,$(FUZZ_SRC))
$(BUILD)/%_fuzzer_local: fuzz/%_fuzzer.c fuzz/standalone_main.c $(LIB_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DVL_GUARD_ALLOC $(INCLUDES) $< fuzz/standalone_main.c $(LIB_SRC) -o $@

clean:
	rm -rf $(BUILD)
