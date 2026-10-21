# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11
DIS      = ../usse-dis
CPPFLAGS += -I$(DIS) -D_POSIX_C_SOURCE=200809L

USSE_SRC = usse_main.c asm.c $(DIS)/usse.c $(DIS)/usse_form.c
PDS_SRC  = pds_main.c asm.c pds_forms.c $(DIS)/pds.c $(DIS)/usse_form.c $(DIS)/usse.c
HDRS     = asm.h pds_asm.h $(DIS)/usse.h $(DIS)/usse_form.h $(DIS)/pds.h $(wildcard $(DIS)/forms/*.inc)

all: usse-asm pds-asm

usse-asm: $(USSE_SRC) $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(USSE_SRC)

pds-asm: $(PDS_SRC) $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(PDS_SRC)

check: all
	../../.venv/bin/pytest -q

clean:
	rm -f usse-asm pds-asm

.PHONY: all check clean
