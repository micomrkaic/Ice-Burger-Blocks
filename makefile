# ===== Config =====
# Output name (change if you like)
APP ?= iceburger
SRCS := main.c game.c render.c audio.c
HDRS := game.h render.h audio.h font8x8.h

CC ?= gcc
CSTD ?= -std=c11
WARN ?= -Wall -Wextra
OPT ?= -O2

PKGCONF ?= pkg-config
PKGS := sdl2

# Try pkg-config first
PKG_CFLAGS := $(shell $(PKGCONF) --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell $(PKGCONF) --libs   $(PKGS) 2>/dev/null)

# Fallback include/lib paths if pkg-config not found or not configured
# (Homebrew/Intel macs commonly use /usr/local; Apple Silicon uses /opt/homebrew).
# The source does #include <SDL.h>, so the include path must point into .../SDL2.
FALLBACK_INC := -I/opt/homebrew/include/SDL2 -I/usr/local/include/SDL2 -I/usr/include/SDL2
FALLBACK_LIB := -L/opt/homebrew/lib -L/usr/local/lib -lSDL2

SDL_CFLAGS := $(if $(strip $(PKG_CFLAGS)),$(PKG_CFLAGS),$(FALLBACK_INC))

CFLAGS ?= $(OPT) $(WARN) $(CSTD)
CFLAGS += $(SDL_CFLAGS)
LDFLAGS +=
LIBS := $(if $(strip $(PKG_LIBS)),$(PKG_LIBS),$(FALLBACK_LIB))

UNAME_S := $(shell uname -s)

# Linux needs -lm for sinf/cosf/etc. (macOS links libm via libSystem)
ifeq ($(UNAME_S),Linux)
  LIBS += -lm
endif

# Debug and sanitizer builds get their own binaries, so they are never
# considered up to date just because a release build exists.
DEBUG_APP := $(APP)-debug
ASAN_APP  := $(APP)-asan
DEBUG_CFLAGS := -g -O0 $(WARN) $(CSTD) $(SDL_CFLAGS)
ASAN_FLAGS   := -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_CFLAGS  := -g -O0 $(ASAN_FLAGS) $(WARN) $(CSTD) $(SDL_CFLAGS)

# Per-user install: binary in ~/.local/bin, man page in ~/.local/share/man/man6.
# man-db finds ~/.local/share/man automatically when ~/.local/bin is on PATH.
# Override for a system install, e.g.  sudo make install PREFIX=/usr/local
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
MAN6DIR := $(MANDIR)/man6
MANPAGE := iceburger.6
INSTALL ?= install

# Rules tests: game.c only, no SDL
TEST_BIN := tests/test_game

# WebAssembly (Emscripten): SDL2 comes from the emscripten port
EMCC ?= emcc
WEB_DIR := web
EMFLAGS := -O2 $(CSTD) -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sMINIFY_HTML=0 --shell-file shell.html

# ===== Targets =====
.PHONY: all run clean debug sanitize test wasm serve install uninstall man

all: $(APP)

$(APP): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LIBS)

run: $(APP)
	./$(APP)

# Debug build (symbols, no optimizations)
debug: $(DEBUG_APP)

$(DEBUG_APP): $(SRCS) $(HDRS)
	$(CC) $(DEBUG_CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LIBS)

# Address/UB sanitizer build (development only)
sanitize: $(ASAN_APP)

$(ASAN_APP): $(SRCS) $(HDRS)
	$(CC) $(ASAN_CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(ASAN_FLAGS) $(LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): tests/test_game.c game.c game.h
	$(CC) -g -O1 $(WARN) $(CSTD) $(ASAN_FLAGS) tests/test_game.c game.c -o $@ $(ASAN_FLAGS)

wasm: $(WEB_DIR)/index.html

$(WEB_DIR)/index.html: $(SRCS) $(HDRS) shell.html
	mkdir -p $(WEB_DIR)
	$(EMCC) $(EMFLAGS) $(SRCS) -o $@

# Browsers won't load .wasm from file://, so serve the folder locally
serve: wasm
	cd $(WEB_DIR) && python3 -m http.server 8000

install: $(APP) $(MANPAGE)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MAN6DIR)
	$(INSTALL) -m 755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)
	$(INSTALL) -m 644 $(MANPAGE) $(DESTDIR)$(MAN6DIR)/$(APP).6
	@case ":$$PATH:" in *":$(BINDIR):"*) ;; *) echo "Note: $(BINDIR) is not on your PATH";; esac

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(APP) $(DESTDIR)$(MAN6DIR)/$(APP).6

# Preview the man page without installing
man:
	man ./$(MANPAGE)

clean:
	$(RM) $(APP) $(DEBUG_APP) $(ASAN_APP) $(TEST_BIN) *.o
	$(RM) -r $(WEB_DIR)
