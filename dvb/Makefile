LIBS         := -lz -lm -lPocoFoundation -lPocoUtil

all: build/resgen

build/resgen: resgen.cpp
	clang -g -o build/resgen -Wl,--copy-dt-needed-entries $(CFLAGS) -L/usr/lib/x86_64-linux-gnu $(LIBS) resgen.cpp

clean:
	rm -r build/*
