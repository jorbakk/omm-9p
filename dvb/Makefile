CXXFLAGS = -Wno-format-security -Wno-return-type
LIBS     = -lPocoUtil -lPocoFoundation -lm
B        = build

## Honor locally set include and linker paths
export $(CPATH)
export $(LIBRARY_PATH)

.PHONY: libommdvb clean

all: $(B) $(B)/resgen $(B)/TransponderData.h libommdvb $(B)/tunedvbcpp $(B)/scandvbcpp $(B)/tunedvb

$(B):
	mkdir -p $(B)

clean:
	rm -r $(B)/*

$(B)/resgen: resgen.cpp
	$(CXX) -g -o $(B)/resgen resgen.cpp $(CXXFLAGS) $(LIBS)

$(B)/TransponderData.h: $(B)/resgen transponder.zip
	$(B)/resgen --output-directory=$(B) --resource-name=TransponderData transponder.zip

libommdvb: CMakeLists.txt $(B)/resgen
	cmake -S . -B $(B)
	make -C $(B)

$(B)/tunedvbcpp: TuneDvb.cpp libommdvb
	$(CXX) -g -o $(B)/tunedvbcpp TuneDvb.cpp -Wl,--copy-dt-needed-entries $(CXXFLAGS) $(LIBS) -Lbuild -lommdvb

$(B)/scandvbcpp: ScanDvb.cpp libommdvb
	$(CXX) -g -o $(B)/scandvbcpp ScanDvb.cpp -Wl,--copy-dt-needed-entries $(CXXFLAGS) $(LIBS) -Lbuild -lommdvb

$(B)/tunedvb: tunedvb.c libommdvb
	9c -g -o $(B)/tunedvb.o tunedvb.c
	9l -g -o $(B)/tunedvb build/tunedvb.o -Lbuild -lommdvb
