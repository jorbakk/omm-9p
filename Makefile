LIBS        := -lz -lm
9PLIBS      := -l9 -l9p -l9pclient
# 9PLIBS      := -l9 -l9p -l9pclient -ldraw
AVLIBS      := -lavutil -lavformat -lavcodec -lswscale -lswresample
SDL2LIBS    := $(shell pkg-config --libs sdl2)
SDL2CFLAGS  := $(shell pkg-config --cflags sdl2)

LIBS   += $(9PLIBS) $(AVLIBS) $(SDL2LIBS)
CFLAGS += $(SDL2CFLAGS)

all: build/renderer build/server

build/renderer: renderer.c
	9c -g -o build/renderer.o $(CFLAGS) renderer.c
	9l -g -o build/renderer build/renderer.o $(LIBS)

build/server: server.c
	9c -g -o build/server.o server.c
	9l -g -o build/server build/server.o $(9PLIBS)

clean:
	rm -r build/*
