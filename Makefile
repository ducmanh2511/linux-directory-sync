CC := gcc
CPPFLAGS := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g
LDLIBS := -pthread -lcrypto

.PHONY: all clean
all: server client

server: server.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

client: client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -f server client
