# libsgxfive: userspace test library and tool for the sgxfive render node.
#
#   make            build/libsgxfive.a and build/sgxfive-test
#   make check      the C unit tests (TAP) and the pytest tests, all against the mock
#   make gen        regenerate src/prog_asm_gen.h from tools/usse-asm/progs/ (needs python3
#                   and a host cc for the assembler); make check verifies it is current
#   make clean
#
# No dependencies beyond a C11 compiler; the CLI tests need pytest (PYTEST=...).

CC      ?= cc
AR      ?= ar
PYTEST  ?= $(firstword $(wildcard ../../.venv/bin/pytest) pytest)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter
CPPFLAGS += -D_DEFAULT_SOURCE -Iinclude -I../../kernel/uapi -Ithird_party/drm
ifneq ($(shell uname -s),Linux)
CPPFLAGS += -Ithird_party/compat	# <linux/bits.h>, <linux/const.h> for drm_mode.h
endif

B        = build
LIB_SRC  = src/dev.c src/xfer.c src/xfer_job.c src/mock.c src/prog_fake.c src/prog_asm.c
LIB_OBJ  = $(LIB_SRC:src/%.c=$(B)/%.o)
TESTS    = test_uapi test_xfer test_dev test_job test_prog
PYTHON  ?= python3
HEADERS  = $(wildcard include/*.h src/*.h) ../../kernel/uapi/sgxfive_drm.h

all: $(B)/libsgxfive.a $(B)/sgxfive-test

$(B):
	mkdir -p $(B)

$(B)/%.o: src/%.c $(HEADERS) | $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/libsgxfive.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(B)/sgxfive-test: tools/sgxfive-test.c $(B)/libsgxfive.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(B)/libsgxfive.a

$(B)/test_%: tests/test_%.c tests/tap.h $(B)/libsgxfive.a $(HEADERS)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) -o $@ $< $(B)/libsgxfive.a

$(B)/prog_dump: tests/prog_dump.c $(B)/libsgxfive.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(B)/libsgxfive.a

gen:
	$(PYTHON) tools/gen_progs.py -o src/prog_asm_gen.h

check: $(TESTS:%=$(B)/%) $(B)/sgxfive-test $(B)/prog_dump
	@set -e; for t in $(TESTS); do \
		echo "== $$t"; ./$(B)/$$t > $(B)/$$t.tap || { cat $(B)/$$t.tap; exit 1; }; \
		echo "   $$(grep -c '^ok' $(B)/$$t.tap) passed"; \
	done
	SGXFIVE_TEST=$(B)/sgxfive-test SGXFIVE_PROG_DUMP=$(B)/prog_dump $(PYTEST) -q tests

clean:
	rm -rf $(B)

.PHONY: all check clean gen
