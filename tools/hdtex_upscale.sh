#!/bin/sh
# Upscale world textures for HD replacement with Real-ESRGAN (realesrgan-ncnn-vulkan from the official
# release at github.com/xinntao/Real-ESRGAN, model realesrgan-x4plus).
# usage: tools/hdtex_upscale.sh <realesrgan folder> <game folder>
#        tools/hdtex_upscale.sh <realesrgan folder> <folder of dumped textures> <output folder>
# With a game folder it reads "HD Textures/new" (the game writes each world texture it shows that has
# no HD version yet), writes the results to "HD Textures" and removes what it upscaled from "new".
# The game loads them on its next start.
set -e
ESR=${1:?realesrgan folder}
SRC=${2:?game folder or folder of dumped textures}
if [ -d "$SRC/HD Textures/new" ]; then
    IN="$SRC/HD Textures/new"; OUT="$SRC/HD Textures"; CLEAN=1
else
    IN=$SRC; OUT=${3:?output folder}; CLEAN=
fi
TMP=$(mktemp -d)
"$ESR/realesrgan-ncnn-vulkan" -i "$IN" -o "$TMP" -n realesrgan-x4plus -s 4 -f png -m "$ESR/models"
mkdir -p "$OUT"
n=0
for f in "$TMP"/*.png; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    mv "$f" "$OUT/$b"
    if [ -n "$CLEAN" ]; then rm -f "$IN/$b"; fi
    n=$((n + 1))
done
rmdir "$TMP"
echo "$n files upscaled into $OUT"
