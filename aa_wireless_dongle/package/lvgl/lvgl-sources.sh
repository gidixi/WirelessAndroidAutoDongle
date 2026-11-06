#!/bin/sh
# The LVGL translation units we compile, relative to the LVGL source tree.
# Kept in one place because both the bench Makefile and the Buildroot package
# use it, and because every excluded directory must have its LV_USE_* switch
# turned off in third_party/lv_conf.h.
#
# Excluded on purpose:
#   drivers                        - display/touch drivers, we render offscreen
#   libs except bin_decoder/fsdrv  - third-party codecs we do not use
#   draw/<hardware backends>       - only the software renderer is enabled
#   draw/convert/{neon,helium}     - optional asm, enabled by other switches
#   osal except lv_os.c/lv_os_none.c - LV_USE_OS is LV_OS_NONE
#   widgets/{lottie,3dtexture}     - need ThorVG / GL
#   others                         - every one of them is behind an LV_USE_* switch we leave off

LVGL_DIR="${1:?usage: lvgl_sources.sh <lvgl-dir>}"

cd "$LVGL_DIR" || exit 1

find src \
    -path 'src/drivers' -prune -o \
    -path 'src/libs' -prune -o \
    -path 'src/others' -prune -o \
    -path 'src/osal' -prune -o \
    -path 'src/draw/opengles' -prune -o \
    -path 'src/draw/sdl' -prune -o \
    -path 'src/draw/dma2d' -prune -o \
    -path 'src/draw/nxp' -prune -o \
    -path 'src/draw/renesas' -prune -o \
    -path 'src/draw/espressif' -prune -o \
    -path 'src/draw/eve' -prune -o \
    -path 'src/draw/nema_gfx' -prune -o \
    -path 'src/draw/vg_lite' -prune -o \
    -path 'src/draw/convert/neon' -prune -o \
    -path 'src/draw/convert/helium' -prune -o \
    -path 'src/widgets/lottie' -prune -o \
    -path 'src/widgets/3dtexture' -prune -o \
    -path 'src/stdlib/builtin' -prune -o \
    -path 'src/stdlib/micropython' -prune -o \
    -path 'src/stdlib/rtthread' -prune -o \
    -name '*.c' -print | sort

# Pulled back in from pruned directories: the pieces lv_init() calls
# unconditionally or that our lv_conf.h turns on.
find src/osal/lv_os.c src/osal/lv_os_none.c src/libs/bin_decoder src/libs/fsdrv \
    -name '*.c' -print | sort
