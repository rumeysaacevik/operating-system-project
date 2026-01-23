CC=gcc
CFLAGS=-O2 -Wall -Wextra -pthread
LDFLAGS=-lrt

all: procx

procx: procx.c
	$(CC) $(CFLAGS) -o procx procx.c $(LDFLAGS)

clean:
	rm -f procx
