LIBS := -lm -lPocoUtil -lPocoFoundation
CFLAGS := -Wno-format-security

# all: build/resgen build/libommdvb.a build/tunedvb build/scandvb
all: build/resgen build/libommdvb.so build/tunedvb build/scandvb

build/resgen: resgen.cpp
	clang -g -o build/resgen -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) resgen.cpp

# build/libommdvb.a: CMakeLists.txt build/resgen
build/libommdvb.so: CMakeLists.txt build/resgen
	cmake -S . -B build
	make -C build

# build/tunedvb: TuneDvb.cpp build/libommdvb.a
	# # FIXME linking of static lib libommdvb.a fails, note the order of the libs ... (build libommdvb.so instead?)
	# # clang -g -o build/tunedvb -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb TuneDvb.cpp
	# clang -g -c -o build/tunedvb.o -Wl,--copy-dt-needed-entries $(CFLAGS) TuneDvb.cpp
	# clang -o build/tunedvb $(LIBS) build/tunedvb.o build/libommdvb.a $(LIBS) build/tunedvb.o build/libommdvb.a $(LIBS) build/tunedvb.o build/libommdvb.a

# build/scandvb: ScanDvb.cpp build/libommdvb.a
#	clang -g -o build/scandvb -Wl,--copy-dt-needed-entries $(CFLAGS) $(LIBS) -Lbuild -lommdvb ScanDvb.cpp

clean:
	rm -r build/*
