# graph - filesystem repository over SMB
# See LICENSE file for copyright and license details.

# Version lives in the VERSION file (single source of truth).
# To release: bump VERSION, commit, tag the same value with `v` prefix, push tag.
VERSION := $(shell cat VERSION)

PREFIX = /usr/local

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
endif

CFLAGS  += -std=c99 -pedantic -Wall -Wextra -Os \
           -D_XOPEN_SOURCE=700 -DVERSION=\"$(VERSION)\"

SRC = src/graph.c src/init.c src/display.c src/util.c
OBJ = $(SRC:.c=.o)
BIN = graph

all: $(BIN)

.c.o:
	$(CC) -c $(CFLAGS) -o $@ $<

$(OBJ): src/graph.h
src/display.o: src/ui.h

# UI is authored as HTML and embedded so the binary stays self-contained.
src/ui.h: src/ui.html tools/embed.sh
	sh tools/embed.sh src/ui.html ui_html > $@

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJ) src/ui.h

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin/
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

# --- debian package ---
# `make deb` produces dist/graph_<version>_<arch>.deb. CI overrides DEB_VERSION
# from the git tag (refs/tags/vX.Y.Z -> X.Y.Z); local builds default to the
# VERSION file.
DEB_VERSION ?= $(VERSION)
DEB_ARCH    := $(shell dpkg --print-architecture 2>/dev/null)
DEB_STAGE   := dist/deb-stage
DEB_FILE    := dist/graph_$(DEB_VERSION)_$(DEB_ARCH).deb

deb: debian/control.in
ifneq ($(UNAME_S),Linux)
	@echo "make deb builds a Debian package and must run on Linux."
	@echo "Use: docker run --rm -v \"\$$PWD:/src\" -w /src graph-build make deb"
	@exit 1
endif
	$(MAKE) clean
	$(MAKE) PREFIX=/usr
	rm -rf $(DEB_STAGE)
	$(MAKE) install DESTDIR=$(DEB_STAGE) PREFIX=/usr
	mkdir -p $(DEB_STAGE)/DEBIAN
	sed -e 's|__VERSION__|$(DEB_VERSION)|g' \
	    -e 's|__ARCH__|$(DEB_ARCH)|g' \
	    debian/control.in > $(DEB_STAGE)/DEBIAN/control
	mkdir -p dist
	fakeroot dpkg-deb --build --root-owner-group $(DEB_STAGE) $(DEB_FILE)
	@echo
	@echo "  built $(DEB_FILE)"

.PHONY: all clean install uninstall deb
