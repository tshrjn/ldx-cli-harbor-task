CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall
ldx: ldx.c $(wildcard fam_*.c)
	$(CC) $(CFLAGS) -o ldx ldx.c
clean:
	rm -f ldx
