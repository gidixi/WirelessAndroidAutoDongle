.PHONY: clean

EXTRA_CXXFLAGS += $(shell $(PKG_CONFIG) --cflags openssl)
EXTRA_LIBS += $(shell $(PKG_CONFIG) --libs openssl) -lpthread

# Live rendering needs an encoder; when it is missing the daemon still builds
# and only replays pre-encoded streams (--h264).
X264_CFLAGS := $(shell $(PKG_CONFIG) --cflags x264 2>/dev/null)
X264_LIBS := $(shell $(PKG_CONFIG) --libs x264 2>/dev/null)
ifneq ($(strip $(X264_LIBS)),)
EXTRA_CXXFLAGS += -DAA_SRCD_HAVE_X264=1 $(X264_CFLAGS)
EXTRA_LIBS += $(X264_LIBS)
endif

aa-srcd: src/pbwire.o src/transport.o src/messenger.o src/framesource.o src/srcd.o
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o '$@' $^ $(EXTRA_LIBS)

src/%.o: src/%.cpp $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -std=c++17 -c -o '$@' '$<'

clean:
	-rm -f aa-srcd src/*.o
