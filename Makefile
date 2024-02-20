RELEASE      = 0

WD           = $(shell pwd)
# B            = $(WD)/build
# EXT          = $(WD)/ext
SYS          = $(WD)/sys
# P            = $(SYS)/pkg
# DVB          = $(WD)/dvb
B            = build
DVB          = dvb
EXT          = ext
# SYS          = sys
P            = $(SYS)/pkg

9PLIBS       = -l9 -l9p -l9pclient -lthread -lsec -lmp -lauth -lbio
9PCLIENTLIBS = -l9 -l9pclient -lbio -lsec -lauth -lthread
AVLIBS       = -lavutil -lavformat -lavcodec -lswscale -lswresample
POCOCFLAGS   = -DPOCO_VERSION_HEADER_FOUND
POCOLIBS     = -lPocoFoundation -lPocoUtil -lPocoXML -lPocoZip
SDL2LIBS     = $(shell pkg-config --libs sdl2)
SDL2CFLAGS   = $(shell pkg-config --cflags sdl2)
SQLITE3LIBS  = $(shell pkg-config --libs sqlite3)

# CFLAGS       = -std=c99 -Wall -Wextra -Wpedantic -Wno-sizeof-array-div
DVBCXXFLAGS  = -g -Wno-format-security -Wno-return-type
DVBLIBS      = $(POCOLIBS) -ludev
PKG_CONFIG   = pkg-config
VLC_PLUGIN_CFLAGS = $(shell $(PKG_CONFIG) --cflags vlc-plugin)
VLC_PLUGIN_LIBS   = $(shell $(PKG_CONFIG) --libs vlc-plugin)
VLCFLAGS     = $(shell $(PKG_CONFIG) --cflags libvlc)
VLCLIBS      = $(shell $(PKG_CONFIG) --libs libvlc)

ifeq ($(RELEASE), 1)
CFLAGS       = -O2 -DNDEBUG
LDFLAGS      = # -static
else
CFLAGS       = -std=c99 -Wall -g -D__DEBUG__ -Wno-sizeof-array-div # -fsanitize=address -fsanitize=undefined # -fsanitize=thread
LDFLAGS      = -Wl,-rpath,$(B):$(LD_RUN_PATH) # -lasan -lubsan # -fsanitize=thread
endif

## Honor local include and linker paths
export $(CPATH)
export $(LIBRARY_PATH)
## If -rpath is not used as a linker flag, LD_RUN_PATH is honored (we use -rpath for $(B))
# export $(LD_RUN_PATH)

.PHONY: clean sloc transponder.zip

$(EXT)/%: $(B)
	cd $(EXT) && git clone $($*_url)

$(P)/%.make: # $(EXT)/%
	cd $(EXT)/$* && make # CFLAGS=-fPIC
	cd $(EXT)/$* && make PREFIX=$(SYS) install && make clean
	touch $@

libixp_url = https://github.com/0intro/libixp.git

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

all: cscope.out $(B) $(B)/ommrender $(B)/render $(B)/ommserve $(B)/tunedvbcpp $(B)/scandvbcpp $(B)/tunedvb $(B)/libvlc_access9P_plugin.so $(B)/libvlc_control9P_plugin.so

$(B):
	mkdir -p $(B) $(EXT) $(SYS) $(P)

clean:
	rm -rf $(B)

$(B)/ommrender: renderer.c
	$(CC) $(CFLAGS) $(SDL2CFLAGS) $(LDFLAGS) -o $@ $< $(9PLIBS) $(AVLIBS) $(SDL2LIBS) -lz -lm

$(B)/render: render.c
	$(CC) $(CFLAGS) $(SDL2CFLAGS) $(VLCFLAGS) $(LDFLAGS) -o $@ $< $(SDL2LIBS) $(VLCLIBS) -lz -lm

$(B)/ommserve: server.c $(B)/libommdvb.so # $(B)/libommdvb.a
	$(CC) $(CFLAGS) $(SDL2CFLAGS) $(LDFLAGS) -o $(B)/ommserve $< $(9PLIBS) $(SQLITE3LIBS) -L$(B) -lommdvb -lm

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

$(B)/tunedvb: $(DVB)/tunedvb.c $(B)/libommdvb.so # $(B)/libommdvb.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -L$(B) -lommdvb -lm

$(B)/libvlc_access9P_plugin.so: vlc_access9P_plugin.c $(P)/libixp.make
	$(CC) -I$(SYS)/include $(VLC_PLUGIN_CFLAGS) $(LDFLAGS) -shared -fPIC -o $@ $< -L$(SYS)/lib -lixp $(VLC_PLUGIN_LIBS)

$(B)/libvlc_control9P_plugin.so: vlc_control9P_plugin.c $(P)/libixp.make
	$(CC) -I$(SYS)/include $(VLC_PLUGIN_CFLAGS) $(LDFLAGS) -shared -fPIC -o $@ $< -L$(SYS)/lib -lixp $(VLC_PLUGIN_LIBS)

sloc:
	cloc *.c *.h $(DVB)/*.cpp $(DVB)/*.h

cscope.out: *.c *.h $(DVB)/*.cpp $(DVB)/*.h
	cscope -b *.c *.h $(DVB)/*.cpp $(DVB)/*.h
