all: renderer.c
	9c -g -o build/renderer.o renderer.c
	9l -g -o build/renderer build/renderer.o -l9 -l9p -l9pclient -lavutil -lavformat -lavcodec -lswscale -lz -lm

clean:
	rm -r build/*
