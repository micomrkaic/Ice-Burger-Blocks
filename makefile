# ===== Config =====
# Output name (change if you like)
APP ?= tetris
# C source file
SRC ?= tetris.c

CC ?= gcc
CSTD ?= -std=c11
WARN ?= -Wall -Wextra
OPT ?= -O2

PKGCONF ?= pkg-config
PKGS := sdl2 SDL2_ttf

# Try pkg-config first
PKG_CFLAGS := $(shell $(PKGCONF) --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell $(PKGCONF) --libs   $(PKGS) 2>/dev/null)

# Fallback include/lib paths if pkg-config not found or not configured
# (Homebrew/Intel macs commonly use /usr/local; Apple Silicon uses /opt/homebrew).
# The source does #include <SDL.h>, so the include path must point into .../SDL2.
FALLBACK_INC := -I/opt/homebrew/include/SDL2 -I/usr/local/include/SDL2 -I/usr/include/SDL2
FALLBACK_LIB := -L/opt/homebrew/lib -L/usr/local/lib -lSDL2 -lSDL2_ttf

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

# ===== Targets =====
.PHONY: all run clean debug sanitize

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

run: $(APP)
	./$(APP)

clean:
	$(RM) $(APP) $(DEBUG_APP) $(ASAN_APP) *.o

# Debug build (symbols, no optimizations)
debug: $(DEBUG_APP)

$(DEBUG_APP): $(SRC)
	$(CC) $(DEBUG_CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

# Address/UB sanitizer build (development only)
sanitize: $(ASAN_APP)

$(ASAN_APP): $(SRC)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(LDFLAGS) $(ASAN_FLAGS) $(LIBS)
