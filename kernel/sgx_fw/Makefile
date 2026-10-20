# Userspace build of the SGX545 firmware loader core and its fwdump harness.
# tools/fwcutter/test_cloader.py builds fwdump and checks it against fwload.py.
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -pedantic

all: fwdump

fwdump: fwdump.o sgx_fw.o
	$(CC) $(CFLAGS) -o $@ fwdump.o sgx_fw.o

fwdump.o: fwdump.c sgx_fw.h sgx_fw_types.h
sgx_fw.o: sgx_fw.c sgx_fw.h sgx_fw_types.h

clean:
	rm -f fwdump *.o

.PHONY: all clean
