LIBS := -lm -lPocoUtil -lPocoFoundation
CFLAGS := -Wno-format-security -Wno-return-type

.PHONY: libommdvb clean

all: build build/resgen libommdvb build/tunedvbcpp build/scandvbcpp build/tunedvb

build:
	mkdir -p build

clean:
	rm -r build/*

build/resgen: resgen.cpp
	$(CXX) -g -o build/resgen -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) resgen.cpp

libommdvb: CMakeLists.txt build/resgen
	cmake -S . -B build
	make -C build

build/tunedvbcpp: TuneDvb.cpp libommdvb
	$(CXX) -g -o build/tunedvbcpp -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb TuneDvb.cpp

build/scandvbcpp: ScanDvb.cpp libommdvb
	$(CXX) -g -o build/scandvbcpp -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb ScanDvb.cpp

build/tunedvb: tunedvb.c libommdvb
	9c -g -o build/tunedvb.o tunedvb.c
	9l -g -o build/tunedvb build/tunedvb.o -Lbuild -lommdvb
