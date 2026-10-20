CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
OBJS     = usse.o main.o

all: usse-dis pds-dis

usse-dis: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

usse.o: usse.c usse.h
main.o: main.c usse.h

pds-dis: pds.c pdsmain.c pds.h
	$(CC) $(CFLAGS) -o $@ pds.c pdsmain.c

check: usse-dis pds-dis
	python3 oracle_check.py
	python3 pds_oracle_check.py

clean:
	rm -f $(OBJS) usse-dis pds-dis

.PHONY: all check clean
