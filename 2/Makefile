CC=gcc
CFLAGS=-Wall -Wextra -O2

all: locker

locker: locker.c
	$(CC) $(CFLAGS) locker.c -o locker

clean:
	rm -f locker stats.txt result.txt sharedfile *.lck