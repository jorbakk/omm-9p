LIBS := -lPocoUtil -lPocoFoundation

all: build/resgen build/libommdvb.so

build/resgen: resgen.cpp
	clang -g -o build/resgen -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) resgen.cpp

build/libommdvb.so: CMakeLists.txt
	cmake -S . -B build
	make -C build

clean:
	rm -r build/*
