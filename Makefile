LIBS        := -lz -lm
9PLIBS      := -l9 -l9p -l9pclient
AVLIBS      := -lavutil -lavformat -lavcodec -lswscale -lswresample
SDL2LIBS    := $(shell pkg-config --libs sdl2)
SDL2CFLAGS  := $(shell pkg-config --cflags sdl2)

LIBS   += $(9PLIBS) $(AVLIBS) $(SDL2LIBS)
CFLAGS += $(SDL2CFLAGS)

all: renderer.c
	9c -g -o build/renderer.o $(CFLAGS) renderer.c
	9l -g -o build/renderer build/renderer.o $(LIBS)

clean:
	rm -r build/*
