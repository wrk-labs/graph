# graph - personal knowledge hub
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

# --- display shell ---
# `graph display` opens its page in a thin native window when one is
# installed under libexec/graph next to the binary: Graph.app (WKWebView) on
# macOS, graph-shell (WebKitGTK) on Linux. Neither is required — without one
# the page opens in the browser — so on Linux the shell is built only when
# the WebKitGTK development files are present.
ifeq ($(UNAME_S),Darwin)
SHELL_BIN = Graph.app
else
WEBKIT := $(shell pkg-config --exists webkit2gtk-4.1 && echo webkit2gtk-4.1 || \
	    (pkg-config --exists webkit2gtk-4.0 && echo webkit2gtk-4.0))
ifneq ($(WEBKIT),)
SHELL_BIN = graph-shell
endif
endif

all: $(BIN) $(SHELL_BIN)

.c.o:
	$(CC) -c $(CFLAGS) -o $@ $<

$(OBJ): src/graph.h
src/display.o: src/ui.h

# UI is authored as HTML and embedded so the binary stays self-contained.
src/ui.h: src/ui.html tools/embed.sh
	sh tools/embed.sh src/ui.html ui_html > $@

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

# The bundle carries its own copy of graph so it can live in /Applications on
# its own: double-clicking asks for a repository and runs `graph display`.
Graph.app: $(BIN) shell/mac.m shell/Info.plist shell/icon.svg tools/icns.sh
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	$(CC) -fobjc-arc -Wall -Wextra -Os -framework Cocoa -framework WebKit \
	    -o $@/Contents/MacOS/graph-shell shell/mac.m
	cp -f $(BIN) $@/Contents/MacOS/graph
	sed 's|__VERSION__|$(VERSION)|g' shell/Info.plist > $@/Contents/Info.plist
	sh tools/icns.sh shell/icon.svg $@/Contents/Resources/graph.icns

graph-shell: shell/linux.c
	$(CC) -Wall -Wextra -Os -o $@ shell/linux.c \
	    $(shell pkg-config --cflags --libs gtk+-3.0 $(WEBKIT))

clean:
	rm -rf $(BIN) $(OBJ) src/ui.h Graph.app graph-shell

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin/
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
ifeq ($(SHELL_BIN),Graph.app)
	rm -rf $(DESTDIR)$(PREFIX)/libexec/graph/Graph.app
	mkdir -p $(DESTDIR)$(PREFIX)/libexec/graph
	cp -R Graph.app $(DESTDIR)$(PREFIX)/libexec/graph/
endif
ifeq ($(SHELL_BIN),graph-shell)
	mkdir -p $(DESTDIR)$(PREFIX)/libexec/graph \
	    $(DESTDIR)$(PREFIX)/share/applications \
	    $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	cp -f graph-shell $(DESTDIR)$(PREFIX)/libexec/graph/
	chmod 755 $(DESTDIR)$(PREFIX)/libexec/graph/graph-shell
	cp -f shell/graph.desktop $(DESTDIR)$(PREFIX)/share/applications/
	cp -f shell/icon.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/graph.svg
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(PREFIX)/libexec/graph
	rm -f $(DESTDIR)$(PREFIX)/share/applications/graph.desktop \
	    $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/graph.svg

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
