LIBS        := -lz -lm
9PLIBS      := -l9 -l9p -l9pclient
AVLIBS      := -lavutil -lavformat -lavcodec -lswscale -lswresample
SDL2LIBS    := $(shell pkg-config --libs sdl2)
SDL2CFLAGS  := $(shell pkg-config --cflags sdl2)

LIBS   += $(9PLIBS) $(AVLIBS) $(SDL2LIBS)
CFLAGS += $(SDL2CFLAGS)

# all: build/renderer7 build/renderer build/threadtest
all: build/renderer7 build/renderer1 build/renderer

build/renderer1: renderer1.c
	9c -g -o build/renderer1.o $(CFLAGS) renderer1.c
	9l -g -o build/renderer1 build/renderer1.o $(LIBS)

build/renderer7: renderer7.c
	9c -g -o build/renderer7.o $(CFLAGS) renderer7.c
	9l -g -o build/renderer7 build/renderer7.o $(LIBS)

build/renderer: renderer.c
	9c -g -o build/renderer.o $(CFLAGS) renderer.c
	9l -g -o build/renderer build/renderer.o $(LIBS)

# build/threadtest: threadtest.c
	# 9c -g -o build/threadtest.o $(CFLAGS) threadtest.c
	# 9l -g -o build/threadtest build/threadtest.o $(LIBS)

clean:
	rm -r build/*
