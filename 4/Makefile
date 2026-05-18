CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
TARGETS = server client gen_numbers

.PHONY: all clean

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

gen_numbers: gen_numbers.c
	$(CC) $(CFLAGS) -o gen_numbers gen_numbers.c

clean:
	rm -f $(TARGETS) result.txt numbers.txt
	rm -f /tmp/brownian_bot.sock
	rm -f /tmp/server.log
	rm -rf client_logs