.PHONY: clean all

# Bench (host) build of the daemon. Buildroot drives the same Makefile through
# aa-srcd.mk, passing CC/CXX/CFLAGS plus WITH_X264/WITH_LVGL and the staging
# include/library paths, which turns off the pkg-config probing below.
#
#   make CXX=g++                                                        # replay only
#   make CXX=g++ PKG_CONFIG_PATH=/tmp/x264prefix/lib/pkgconfig          # + live encode
#   make CXX=g++ LVGL_DIR=/tmp/lvgl                                     # + LVGL dashboard

PKG_CONFIG ?= pkg-config
LVGL_DIR ?=
LVGL_PKGDIR := $(abspath $(CURDIR)/../lvgl)
# Out of the repository on purpose: object churn on a Windows mount is slow.
LVGL_BUILD_DIR ?= /tmp/aa-srcd-lvgl-build

# Set by the caller to bypass detection entirely.
WITH_X264 ?=
WITH_LVGL ?=
AA_SRCD_EXTRA_CXXFLAGS ?=
AA_SRCD_EXTRA_LIBS ?=

X264_CFLAGS :=
X264_LIBS :=
ifeq ($(WITH_X264),)
X264_CFLAGS := $(shell $(PKG_CONFIG) --cflags x264 2>/dev/null)
X264_LIBS := $(shell $(PKG_CONFIG) --libs x264 2>/dev/null)
WITH_X264 := $(if $(strip $(X264_LIBS)),1,0)
endif

EXTRA_CXXFLAGS := $(AA_SRCD_EXTRA_CXXFLAGS)
EXTRA_LIBS := $(AA_SRCD_EXTRA_LIBS) -lpthread

ifeq ($(WITH_X264),1)
EXTRA_CXXFLAGS += -DAA_SRCD_HAVE_X264=1 $(X264_CFLAGS)
EXTRA_LIBS += $(X264_LIBS)
endif

# Without explicit libraries from the caller we are on a host: probe OpenSSL.
ifeq ($(strip $(AA_SRCD_EXTRA_LIBS)),)
EXTRA_LIBS += $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo -lssl -lcrypto)
endif

SOURCES := src/pbwire.cpp src/transport.cpp src/messenger.cpp src/framesource.cpp src/srcd.cpp

# LVGL comes either from a checkout (bench, LVGL_DIR) or from the staged
# toolchain (Buildroot passes WITH_LVGL=1 with the include paths).
ifneq ($(strip $(LVGL_DIR)),)
WITH_LVGL := 1
LVGL_LIB := $(LVGL_BUILD_DIR)/liblvgl.a
EXTRA_CXXFLAGS += -DLV_CONF_INCLUDE_SIMPLE -I$(LVGL_DIR) -I$(LVGL_DIR)/src -I$(LVGL_PKGDIR)
EXTRA_LIBS += $(LVGL_LIB)
endif

ifeq ($(WITH_LVGL),1)
EXTRA_CXXFLAGS += -DAA_SRCD_HAVE_LVGL=1
SOURCES += src/lvglrenderer.cpp
endif

OBJECTS := $(SOURCES:.cpp=.o)

all: aa-srcd

aa-srcd: $(OBJECTS) $(LVGL_LIB)
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o '$@' $(OBJECTS) $(EXTRA_LIBS)

src/%.o: src/%.cpp $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -std=c++17 -c -o '$@' '$<'

$(LVGL_BUILD_DIR)/liblvgl.a: $(LVGL_PKGDIR)/lvgl-sources.sh $(LVGL_PKGDIR)/lv_conf.h
	sh $(CURDIR)/tools/build_lvgl.sh '$(LVGL_DIR)' '$(LVGL_BUILD_DIR)'

clean:
	-rm -f aa-srcd src/*.o
