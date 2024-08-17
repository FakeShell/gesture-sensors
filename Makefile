CC = gcc
CFLAGS = `pkg-config --cflags glib-2.0 gio-2.0`
LDFLAGS = `pkg-config --libs glib-2.0 gio-2.0` -lbatman-wrappers -lwayland-client -lxkbcommon
SRC = gesture-sensors.c virtual-keyboard-unstable-v1-protocol.c virtkey.c
TARGET = gesture-sensors

PREFIX ?= /usr

SCHEMADIR = $(PREFIX)/share/glib-2.0/schemas
SCHEMA = io.furios.gesture.gschema.xml

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: install-binary install-schema compile-schema

install-binary:
	install -d $(DESTDIR)$(PREFIX)/libexec
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/libexec/

install-schema:
	install -d $(DESTDIR)$(SCHEMADIR)
	install -m 644 $(SCHEMA) $(DESTDIR)$(SCHEMADIR)/

compile-schema:
	@if [ -z "${DEB_HOST_MULTIARCH}" ]; then \
		echo "Compiling GSettings schema..."; \
		glib-compile-schemas $(DESTDIR)$(SCHEMADIR); \
	else \
		echo "Running in debian build system, skipping schema compilation"; \
	fi
