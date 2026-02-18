CC = gcc
CFLAGS = `pkg-config --cflags glib-2.0 gio-2.0` -Iinclude
LDFLAGS = `pkg-config --libs glib-2.0 gio-2.0` -lwayland-client -lxkbcommon

SOURCES = src/gesture-sensors.c src/logind.c src/virtual-keyboard-unstable-v1-protocol.c src/virtkey.c

TARGET = gesture-sensors

PREFIX ?= /usr

$(TARGET): $(SOURCES)
	$(CC) $(SOURCES) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/libexec
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/libexec/
	install -d $(DESTDIR)$(PREFIX)/lib/systemd/user
	install -m 0644 data/gesture-sensors.service $(DESTDIR)$(PREFIX)/lib/systemd/user/
	install -d $(DESTDIR)$(PREFIX)/share/glib-2.0/schemas
	install -m 644 data/io.furios.gesture.gschema.xml $(DESTDIR)$(PREFIX)/share/glib-2.0/schemas/

.PHONY: all clean install
