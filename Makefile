# Builds the tcc-media kit with TinyCC and the libav* of one FFmpeg pin. See README.md.
TCC ?= tcc
CC ?= cc
# The FFmpeg pin (ruling R2): its libav* for langc AND its ffmpeg CLI for the gate.
FFMPEG_PREFIX ?= /opt/homebrew/opt/ffmpeg-full
FFMPEG = $(FFMPEG_PREFIX)/bin/ffmpeg
FFMPEG_LIBS = libavfilter libavformat libavcodec libswscale libswresample libavutil
FFMPEG_PC = PKG_CONFIG_PATH=$(FFMPEG_PREFIX)/lib/pkgconfig pkg-config
FFMPEG_CFLAGS := $(shell $(FFMPEG_PC) --cflags $(FFMPEG_LIBS))
FFMPEG_LDLIBS := $(shell $(FFMPEG_PC) --libs $(FFMPEG_LIBS))
CFLAGS = -std=c99 -Wall -Werror -Isrc $(FFMPEG_CFLAGS)
CLANG_FLAGS = -std=c99 -Wall -Wextra -Wswitch-enum -Werror -fsyntax-only -Isrc $(FFMPEG_CFLAGS)
FRONT = src/front/base.c src/front/lexer.c src/front/parser.c src/front/front.c src/front/eval.c src/front/check.c
TARGET = src/json.c src/media.c
SRC = src/main.c $(FRONT) $(TARGET)
MEDIA_TOOL = test/mediatool.c $(FRONT) $(TARGET)
MEDIA_REVIEW = test/mediareview.c $(FRONT)
HEADERS = $(wildcard src/*.h src/front/*.h)

.PHONY: build check check-clang clean

build: build/langc

build/domain.c: domain/domain.lang gen/embed.c
	mkdir -p build
	$(TCC) -run gen/embed.c domain/domain.lang build/domain.c

build/langc: $(SRC) $(HEADERS) build/domain.c
	$(TCC) $(CFLAGS) -o build/langc $(SRC) build/domain.c $(FFMPEG_LDLIBS)

build/mediatool: $(MEDIA_TOOL) $(HEADERS) build/domain.c
	$(TCC) $(CFLAGS) -o build/mediatool $(MEDIA_TOOL) build/domain.c $(FFMPEG_LDLIBS)

build/mediareview: $(MEDIA_REVIEW) src/media.c $(HEADERS) build/domain.c
	$(TCC) $(CFLAGS) -o build/mediareview $(MEDIA_REVIEW) build/domain.c $(FFMPEG_LDLIBS)

check-clang: build/domain.c
	$(CC) $(CLANG_FLAGS) $(SRC) build/domain.c
	$(CC) $(CLANG_FLAGS) $(MEDIA_TOOL) build/domain.c
	$(CC) $(CLANG_FLAGS) $(MEDIA_REVIEW) build/domain.c

check: build/langc build/mediatool build/mediareview check-clang
	FFMPEG="$(FFMPEG)" sh test/gate.sh
	TCC="$(TCC)" FFMPEG_CFLAGS="$(FFMPEG_CFLAGS)" FFMPEG_LDLIBS="$(FFMPEG_LDLIBS)" node test/regressions.js
	node test/printer-regressions.js
	build/mediatool
	FFMPEG="$(FFMPEG)" build/mediareview

clean:
	rm -rf build
