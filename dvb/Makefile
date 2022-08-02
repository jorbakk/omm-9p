LIBS := -lm -lPocoUtil -lPocoFoundation
CFLAGS := -Wno-format-security

all: build/resgen build/libommdvb.so build/tunedvbcpp build/scandvbcpp build/tunedvb

clean:
	rm -r build/*

build/resgen: resgen.cpp
	clang -g -o build/resgen -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) resgen.cpp

build/libommdvb.so: CMakeLists.txt build/resgen
	cmake -S . -B build
	make -C build

build/tunedvbcpp: TuneDvb.cpp build/libommdvb.so
	clang -g -o build/tunedvbcpp -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb TuneDvb.cpp

build/scandvbcpp: ScanDvb.cpp build/libommdvb.so
	clang -g -o build/scandvbcpp -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb ScanDvb.cpp

build/tunedvb: tunedvb.c build/libommdvb.so
	9c -g -o build/tunedvb.o tunedvb.c
	9l -g -o build/tunedvb build/tunedvb.o -Lbuild -lommdvb
