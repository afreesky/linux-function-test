CC = gcc
CFLAGS = -g -Wall -O2
LIBS = -libverbs

.PHONY: all clean

all: server client

server: rdma_server.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

client: rdma_client.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f server client
