#!/usr/bin/env bash
#
# Stand-in for yt-dlp, for demos and screenshots without a network:
#
#   OFFBEAT_YTDLP=tools/fake_ytdlp.sh OFFBEAT_DL_FAST=1 ./build/offbeat
#
# Understands just what Offbeat asks: a flat search prints result lines, a
# download prints the metadata + progress lines and leaves a real (3 s sine)
# MP3 and a cover image behind, so the whole pipeline (tagging with the real
# ffmpeg, the library picking the files up) can be watched offline.
# DEMO_SPEED=<seconds per download, default 4>.
#
set -u
US=$'\x1f'
SPEED="${DEMO_SPEED:-4}"

TITLES=("Get Lucky" "One More Time" "Harder, Better, Faster, Stronger" "Around the World" "Instant Crush"
        "Da Funk" "Technologic" "Digital Love" "Something About Us" "Veridis Quo" "Lose Yourself to Dance"
        "Robot Rock" "Aerodynamic" "Derezzed" "Giorgio by Moroder" "Voyager" "Fragments of Time" "Touch"
        "Doin' it Right" "Within" "Beyond" "Contact" "Face to Face" "Nightvision" "Burnin'" "Revolution 909"
        "Short Circuit" "Crescendolls" "Superheroes" "Prime Time of Your Life")
VIEWS=(812000000 640000000 410000000 380000000 290000000 240000000 210000000 190000000 170000000 160000000)

id_of() { printf 'DEMOSNG%04d' "$1"; }

args=("$@")
limit=100
for ((i=0;i<${#args[@]};i++)); do [[ "${args[$i]}" == --playlist-end ]] && limit="${args[$((i+1))]}"; done
target="${args[${#args[@]}-1]}"
for a in "$@"; do [[ "$a" == --flat-playlist ]] && search=1; [[ "$a" == --skip-download ]] && lookup=1; done

# ---- artist + real title of one YouTube Music hit ----
if [[ -n "${lookup:-}" ]]; then
    sleep 0.3
    n=$((10#${target##*v=DEMOSNG}))
    printf 'OBE%s%s%sDaft Punk%s200%s%s\n' "$US" "${target##*v=}" "$US" "$US" "$US" "${TITLES[$((n-1))]:-${TITLES[1]}}"
    exit 0
fi

if [[ -n "${search:-}" ]]; then
    sleep 0.6
    if [[ "$target" == *music.youtube.com* ]]; then
        for i in $(seq 1 $((limit < 30 ? limit : 30))); do
            printf '%s%s%s%sNA%sNA%sNA\n' "$(id_of "$i")" "$US" "${TITLES[$((i-1))]}" "$US" "$US" "$US"
            sleep 0.03
        done
    else
        for i in 1 2 3 4 5 6 7; do
            (( i > limit )) && break
            printf '%s%s%s%s%s%s%d%s%d\n' "$(id_of $((100+i)))" "$US" "Daft Punk - ${TITLES[1]} (Official Video)" "$US" \
                "Daft Punk" "$US" $((320+i*9)) "$US" $((${VIEWS[$i]:-90000000} ))
        done
    fi
    exit 0
fi

# ---- download ----
out=""
for ((i=0;i<${#args[@]};i++)); do [[ "${args[$i]}" == -o ]] && out="${args[$((i+1))]}"; done
id="${target##*v=}"
dir="$(dirname "$out")"
n=$((10#${id#DEMOSNG}))
if (( n > 100 )); then title="${TITLES[1]}"; else title="${TITLES[$((n-1))]}"; fi
mkdir -p "$dir"
fields=("$title" "$title" "Daft Punk" "Discovery" 2001 2021 200 NA "Daft Punk" $((n % 14 + 1)) "Daft Punk")
line="OBM"
for f in "${fields[@]}"; do line+="$US$f"; done
echo "$line"
steps=20
for ((s=1;s<=steps;s++)); do
    echo "OBP $((s*500000)) $((steps*500000)) NA"
    sleep "$(awk -v t="$SPEED" -v n="$steps" 'BEGIN{print t/n}')"
done
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=$((220 + n*12)):duration=3" -c:a libmp3lame -q:a 6 "$dir/$id.mp3" || exit 1
hue=$(( (n * 37) % 360 ))
ffmpeg -loglevel error -y -f lavfi -i "gradients=s=1280x720:c0=0x5b3fd0:c1=0xd04fa0:duration=1:speed=0.0$((n%9+1))" -frames:v 1 "$dir/$id.jpg" 2>/dev/null \
    || ffmpeg -loglevel error -y -f lavfi -i "color=c=0x$(printf '%02x%02x%02x' $((60+hue%120)) 50 $((140+hue%100))):s=1280x720" -frames:v 1 "$dir/$id.jpg"
exit 0
