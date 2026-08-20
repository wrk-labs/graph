#!/bin/sh
# Render an SVG into a macOS .icns using only what ships with the OS:
# qlmanage rasterises, sips scales, iconutil packs.
#
# usage: icns.sh <icon.svg> <out.icns>
set -e
svg=$1 out=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

qlmanage -t -s 1024 -o "$tmp" "$svg" >/dev/null 2>&1
png="$tmp/$(basename "$svg").png"
[ -f "$png" ] || { echo "icns.sh: qlmanage produced nothing" >&2; exit 1; }

mkdir "$tmp/g.iconset"
for s in 16 32 128 256 512; do
	sips -z $s $s "$png" --out "$tmp/g.iconset/icon_${s}x${s}.png" >/dev/null
	sips -z $((s*2)) $((s*2)) "$png" --out "$tmp/g.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$tmp/g.iconset" -o "$out"
