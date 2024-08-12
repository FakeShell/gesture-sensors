CC = gcc
CFLAGS = `pkg-config --cflags glib-2.0 gio-2.0`
LDFLAGS = `pkg-config --libs glib-2.0 gio-2.0` -lbatman-wrappers -lwayland-client -lxkbcommon
SRC = wake.c virtual-keyboard-unstable-v1-protocol.c virtkey.c
TARGET = wake

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
