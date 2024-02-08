CXXFLAGS = -Wno-format-security -Wno-return-type
LIBS     = -lPocoUtil -lPocoFoundation -lm
B        = build
## Honor locally set include and linker paths
export $(CPATH)
export $(LIBRARY_PATH)

OMMDVB_SRC = \
dvb.cpp \
Sys.cpp \
AvStream.cpp \
Descriptor.cpp \
Device.cpp \
Log.cpp \
Section.cpp \
Stream.cpp \
Service.cpp \
Transponder.cpp \
Frontend.cpp \
Demux.cpp \
Remux.cpp \
Dvr.cpp \
TransportStream.cpp \
ElementaryStream.cpp \
$(B)/TransponderData.cpp
#Mux.cpp \

.PHONY: clean

all: $(B) $(B)/resgen $(B)/TransponderData.h $(B)/libommdvb.so $(B)/tunedvbcpp $(B)/scandvbcpp $(B)/tunedvb

$(B):
	mkdir -p $(B)

clean:
	rm -r $(B)/*

$(B)/resgen: resgen.cpp
	$(CXX) -g -o $(B)/resgen resgen.cpp $(CXXFLAGS) $(LIBS)

$(B)/TransponderData.h: $(B)/resgen transponder.zip
	$(B)/resgen --output-directory=$(B) --resource-name=TransponderData transponder.zip

$(B)/libommdvb.so: CMakeLists.txt $(B)/resgen
	$(CXX) -shared -fPIC -o $(B)/libommdvb.so -I$(B) -DPOCO_VERSION_HEADER_FOUND -D__DVB_SUPPORT__ -Dommdvb_EXPORTS $(OMMDVB_SRC) $(CXXFLAGS) $(LIBS)

$(B)/tunedvbcpp: TuneDvb.cpp $(B)/libommdvb.so
	$(CXX) -g -o $(B)/tunedvbcpp TuneDvb.cpp -Wl,--copy-dt-needed-entries $(CXXFLAGS) $(LIBS) -Lbuild -lommdvb

$(B)/scandvbcpp: ScanDvb.cpp $(B)/libommdvb.so
	$(CXX) -g -o $(B)/scandvbcpp ScanDvb.cpp -Wl,--copy-dt-needed-entries $(CXXFLAGS) $(LIBS) -Lbuild -lommdvb

$(B)/tunedvb: tunedvb.c $(B)/libommdvb.so
	9c -g -o $(B)/tunedvb.o tunedvb.c
	9l -g -o $(B)/tunedvb build/tunedvb.o -Lbuild -lommdvb
