# ktop -- top(1) clone with a per-process guest-architecture column.
# Usage: make            (builds ./ktop)
#        make install    (installs to $(PREFIX)/bin, default /usr/local/bin)
#        make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

.PHONY: all install clean

all: ktop

ktop: ktop.c
	$(CC) $(CFLAGS) -o $@ $<

install: ktop
	install -d "$(PREFIX)/bin"
	install -m 0755 ktop "$(PREFIX)/bin/ktop"

clean:
	rm -f ktop
