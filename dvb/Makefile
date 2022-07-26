LIBS := -lm -lPocoUtil -lPocoFoundation

all: build/resgen build/libommdvb.a build/tunedvb build/scandvb

build/resgen: resgen.cpp
	clang -g -o build/resgen -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) resgen.cpp

build/libommdvb.a: CMakeLists.txt build/resgen
	cmake -S . -B build
	make -C build

build/tunedvb: TuneDvb.cpp
	clang -g -o build/tunedvb -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb TuneDvb.cpp

build/scandvb: ScanDvb.cpp
	clang -g -o build/scandvb -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb ScanDvb.cpp

clean:
	rm -r build/*
