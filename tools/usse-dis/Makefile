CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
OBJS     = usse.o main.o

usse-dis: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

usse.o: usse.c usse.h
main.o: main.c usse.h

check: usse-dis
	python3 oracle_check.py

clean:
	rm -f $(OBJS) usse-dis

.PHONY: check clean
