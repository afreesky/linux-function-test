CC = gcc
CFLAGS = -Wall -g -O2
LDFLAGS = -libverbs -lrdmacm

TARGETS = rdma_server rdma_client

.PHONY: all clean

all: $(TARGETS)

rdma_server: rdma_server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rdma_client: rdma_client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)
