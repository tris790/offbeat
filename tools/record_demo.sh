#!/usr/bin/env bash
#
# Record the scripted feature tour headlessly (see "Headless recording" in
# src/main.c) into a lossless intermediate: /tmp/offbeat-demo/tour.mkv.
# Uses throwaway cache/config dirs seeded from the real cache, so the user's
# state is never touched. Frames are 60 fps simulated time, real-time paced.
#
#   tools/record_demo.sh            then: tools/make_gif.sh
#
set -euo pipefail
cd "$(dirname "$0")/.."

W="${W:-1376}" H="${H:-972}"
OUT=/tmp/offbeat-demo
rm -rf "$OUT/config" "$OUT/cache" "$OUT/rec.fifo"
mkdir -p "$OUT/config"
cp -r "${XDG_CACHE_HOME:-$HOME/.cache}/offbeat" "$OUT/cache"

# One token per beat of the tour; "@N" waits until frame N (60 per second).
TOUR="tab=0;find=adieu;seek=48"
TOUR+=";@150;vol=0.5"                              # volume toast
TOUR+=";@210;like"                                 # heart
TOUR+=";@270;tab=1;@330;tab=2;@360;scroll=2:700"   # genres, artists
TOUR+=";@420;artist=8;@480;tab=3;@500;scroll=3:2500"   # an artist's songs, Songs
TOUR+=";@570;tab=0"
TOUR+=";@600;open;@620;type=decode;@720;enter"     # Ctrl+K search, play a hit
TOUR+=";@830;vis=bars;@930;vis=silk"               # visualizers
TOUR+=";@960;theme=ocean;@1030;theme=rose;@1100;theme=ember;@1170;theme=violet"
TOUR+=";@1230;menu;@1330;close"                    # context menu
TOUR+=";@1360;settings;@1440;browse;@1560;close"   # settings + folder browser
TOUR+=";@1590;debug"                               # memory overlay
FRAMES=1740

mkfifo "$OUT/rec.fifo"
ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s "${W}x${H}" -framerate 60 \
    -i "$OUT/rec.fifo" -c:v ffv1 -pix_fmt bgra "$OUT/tour.mkv" &
FF=$!

OFFBEAT_CACHE_DIR="$OUT/cache" OFFBEAT_CONFIG_DIR="$OUT/config" \
OFFBEAT_REC="$OUT/rec.fifo" OFFBEAT_SIZE="${W}x${H}" OFFBEAT_SHOT_FRAMES="$FRAMES" \
OFFBEAT_DEMO="$TOUR" ./build/offbeat

wait "$FF"
ls -lh "$OUT/tour.mkv"
