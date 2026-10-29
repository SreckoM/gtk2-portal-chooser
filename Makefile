CFLAGS ?= -O2 -Wall
GIO_CFLAGS := $(shell pkg-config --cflags gio-2.0)
GIO_LIBS := $(shell pkg-config --libs gio-2.0)
GTK2 ?= -l:libgtk-x11-2.0.so.0

all: gtk2_portal_chooser.so

gtk2_portal_chooser.so: gtk2_portal_chooser.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< $(GIO_CFLAGS) $(GIO_LIBS) -ldl

# needs libgtk2.0-0 (the runtime library, not the -dev package)
test_chooser: test_chooser.c
	$(CC) $(CFLAGS) -o $@ $< $(GTK2)

test: gtk2_portal_chooser.so test_chooser
	GTK2_PORTAL_DEBUG=1 LD_PRELOAD=$(CURDIR)/gtk2_portal_chooser.so ./test_chooser

clean:
	rm -f gtk2_portal_chooser.so test_chooser

.PHONY: all test clean
