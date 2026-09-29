#!/usr/bin/env bash
#
# Encode /tmp/offbeat-demo/tour.mkv (see record_demo.sh) into a size-capped GIF.
#   tools/make_gif.sh [out.gif]      env: WIDTH=720 FPS=12 COLORS=128 DENOISE=2:2:8:8
#
# hqdn3d removes the shader's animated film grain (which would otherwise change
# every pixel every frame); ordered (bayer) dither keeps static areas stable.
set -euo pipefail
OUT="${1:-docs/offbeat-demo.gif}"
WIDTH="${WIDTH:-720}" FPS="${FPS:-12}" COLORS="${COLORS:-128}" DENOISE="${DENOISE:-2:2:8:8}"
mkdir -p "$(dirname "$OUT")"
ffmpeg -y -loglevel error -i /tmp/offbeat-demo/tour.mkv -filter_complex \
"color=c=0x0d0b17:s=1376x972:r=60[bg];[bg][0:v]overlay=shortest=1,fps=$FPS,scale=$WIDTH:-2:flags=lanczos,hqdn3d=$DENOISE,format=rgb24,split[a][b];\
[a]palettegen=max_colors=$COLORS:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" \
-loop 0 "$OUT"
ls -l "$OUT" | awk '{printf "%.1f MB  %s\n", $5/1048576, $9}'
