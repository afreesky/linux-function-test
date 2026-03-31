CC = gcc
CFLAGS = -Wall -Wextra -g -DLIBCLI_USE_POLL
LDFLAGS = -lcli -lnm -lglib-2.0 -lgobject-2.0 -std=c11
TARGET = nm-cli
SRC = main.c nm_cli.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) `pkg-config --cflags --libs glib-2.0` `pkg-config --cflags --libs libnm`  $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
