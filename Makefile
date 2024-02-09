RELEASE=0

B=build
DVB=dvb

9PLIBS       = -l9 -l9p
9PCLIENTLIBS = $(9PLIBS) -l9pclient
AVLIBS       = -lavutil -lavformat -lavcodec -lswscale -lswresample
POCOCFLAGS   = -DPOCO_VERSION_HEADER_FOUND
POCOLIBS     = -lPocoFoundation -lPocoUtil -lPocoXML -lPocoZip
SDL2LIBS     = $(shell pkg-config --libs sdl2)
SDL2CFLAGS   = $(shell pkg-config --cflags sdl2)
SQLITE3LIBS  = $(shell pkg-config --libs sqlite3)

CFLAGS       = -std=c99 -Wall -Wextra -Wpedantic -Wno-sizeof-array-div
LDFLAGS      = -Wl,-rpath,$(B)
DVBCXXFLAGS  = -g -Wno-format-security -Wno-return-type
DVBLIBS      = $(POCOLIBS) -ludev

ifeq ($(RELEASE), 1)
CFLAGS       = -O2 -DNDEBUG
LDFLAGS      = -static
else
CFLAGS      +=  -g -D__DEBUG__ -fsanitize=address -fsanitize=undefined # -fsanitize=thread
LDFLAGS      = -lasan -lubsan # -fsanitize=thread
endif

## Honor local include and linker paths
export $(CPATH)
export $(LIBRARY_PATH)

.PHONY: clean cscope sloc transponder.zip

DVBOBJS = \
$(B)/dvb.o \
$(B)/Sys.o \
$(B)/AvStream.o \
$(B)/Descriptor.o \
$(B)/Device.o \
$(B)/Log.o \
$(B)/Section.o \
$(B)/Stream.o \
$(B)/Service.o \
$(B)/Transponder.o \
$(B)/Frontend.o \
$(B)/Demux.o \
$(B)/Remux.o \
$(B)/Dvr.o \
$(B)/TransportStream.o \
$(B)/ElementaryStream.o \
$(B)/TransponderData.o

$(B)/%.o: $(DVB)/%.cpp
	$(CXX) -c -o $@ -I$(B) $(POCOCFLAGS) $(CPPFLAGS) -fPIC $(DVBCXXFLAGS) $<

all: cscope $(B) $(B)/ommrender $(B)/ommserve $(B)/tunedvbcpp $(B)/scandvbcpp $(B)/tunedvb

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)

$(B)/ommrender: renderer.c
	9c $(CFLAGS) $(SDL2CFLAGS) -o $(B)/renderer.o renderer.c
	9l $(LDFLAGS) -o $(B)/ommrender $(B)/renderer.o $(LDFLAGS) $(9PLIBS) $(AVLIBS) $(SDL2LIBS) -lz -lm

$(B)/ommserve: server.c $(B)/libommdvb.so # $(B)/libommdvb.a
	9c $(CFLAGS) -o $(B)/server.o server.c
	9l $(LDFLAGS) -o $(B)/ommserve $(B)/server.o $(LDFLAGS) $(9PLIBS) $(SQLITE3LIBS) -L$(B) -lommdvb

$(B)/resgen: $(B)/resgen.o
	$(CXX) -o $(B)/resgen $< $(POCOLIBS) -lm

$(B)/TransponderData.h: $(B)/resgen transponder.zip
	$(B)/resgen --output-directory=$(DVB) --resource-name=TransponderData $(DVB)/transponder.zip

$(B)/libommdvb.so: $(DVB)/TransponderData.h $(DVBOBJS)
	$(CXX) -shared -o $@ $(DVBOBJS) $(DVBLIBS) -lm

# $(B)/libommdvb.a: $(DVB)/TransponderData.h $(DVBOBJS)
	# $(CXX) -static -o $@ $(DVBOBJS) $(DVBLIBS) -lm

$(B)/tunedvbcpp: $(B)/TuneDvb.o $(B)/libommdvb.so # $(B)/libommdvb.a
	$(CXX) -o $(B)/tunedvbcpp $< -Wl,--copy-dt-needed-entries $(DVBLIBS) -L$(B) -lommdvb -lm

$(B)/scandvbcpp: $(B)/ScanDvb.o $(B)/libommdvb.so # $(B)/libommdvb.a
	$(CXX) -o $(B)/scandvbcpp $< -Wl,--copy-dt-needed-entries $(DVBLIBS) -L$(B) -lommdvb -lm

$(B)/tunedvb: $(B)/libommdvb.so # $(B)/libommdvb.a
	9c $(CFLAGS) -o $(B)/tunedvb.o $(DVB)/tunedvb.c
	9l $(LDFLAGS) -o $(B)/tunedvb $(B)/tunedvb.o $(LDFLAGS) -L$(B) -lommdvb -lm

sloc:
	cloc *.c *.h $(DVB)/*.cpp $(DVB)/*.h

cscope:
	cscope -b *.c *.h $(DVB)/*.cpp $(DVB)/*.h
