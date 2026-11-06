#!/bin/sh
# Host-side helper: builds the same LVGL subset the Buildroot package builds, so
# the bench binary exercises the real dashboard code path.
#   sh tools/build_lvgl.sh <lvgl-checkout> <output-dir>
set -e

LVGL_DIR="${1:?usage: build_lvgl.sh <lvgl-dir> <out-dir>}"
OUT_DIR="${2:?usage: build_lvgl.sh <lvgl-dir> <out-dir>}"
CC="${CC:-gcc}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
PKGDIR="$(cd "$(dirname "$0")/../.." && pwd)"

mkdir -p "$OUT_DIR"

COMMANDS="$OUT_DIR/compile_commands.txt"
: > "$COMMANDS"

for source_file in $(sh "$PKGDIR/lvgl/lvgl-sources.sh" "$LVGL_DIR"); do
    object_file="$OUT_DIR/$(echo "$source_file" | tr '/' '_').o"
    if [ ! -f "$object_file" ] || [ "$LVGL_DIR/$source_file" -nt "$object_file" ]; then
        printf '%s -O2 -std=gnu11 -w -DLV_CONF_INCLUDE_SIMPLE -I%s -I%s/src -I%s/lvgl -c %s -o %s\n' \
            "$CC" "$LVGL_DIR" "$LVGL_DIR" "$PKGDIR" "$LVGL_DIR/$source_file" "$object_file" >> "$COMMANDS"
    fi
done

if [ -s "$COMMANDS" ]; then
    xargs -P "$JOBS" -d '\n' -I COMMAND sh -c COMMAND < "$COMMANDS"
fi

rm -f "$OUT_DIR/liblvgl.a"
ar rcs "$OUT_DIR/liblvgl.a" "$OUT_DIR"/*.o
