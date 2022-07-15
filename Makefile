LIBS         := -lz -lm
9PLIBS       := -l9 -l9p
9PCLIENTLIBS := $(9PLIBS) -l9pclient  # -ldraw
AVLIBS       := -lavutil -lavformat -lavcodec -lswscale -lswresample
SDL2LIBS     := $(shell pkg-config --libs sdl2)
SDL2CFLAGS   := $(shell pkg-config --cflags sdl2)
SQLITE3LIBS  := $(shell pkg-config --libs sqlite3)

LIBS   += $(9PLIBS) $(AVLIBS) $(SDL2LIBS)
CFLAGS += $(SDL2CFLAGS)

all: build/ommrender build/ommserve

build/ommrender: renderer.c
	9c -g -o build/renderer.o $(CFLAGS) renderer.c
	9l -g -o build/ommrender build/renderer.o $(LIBS)

build/ommserve: server.c
	9c -g -o build/server.o server.c
	9l -g -o build/ommserve build/server.o $(9PLIBS) $(SQLITE3LIBS)

clean:
	rm -r build/*
