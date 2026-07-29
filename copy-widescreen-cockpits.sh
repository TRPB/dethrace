#!/usr/bin/env bash
set -euo pipefail

SRC="/games/SteamLibrary/steamapps/common/Carmageddon1/MELD/MELDPACK/DATA"
DST="/games/SteamLibrary/steamapps/common/Carmageddon1/CARMA/DATA"

RESOLUTIONS=(
    "32X20X8"   # 320x200 lowres
    "64X48X8"   # 640x480 hires
    "11X48X8"   # 1120x480 ultrawide
)

for res in "${RESOLUTIONS[@]}"; do
    src_dir="$SRC/$res/PIXELMAP"
    dst_dir="$DST/$res/PIXELMAP"

    if [ ! -d "$src_dir" ]; then
        echo "SKIP: $src_dir not found"
        continue
    fi

    mkdir -p "$dst_dir"

    # CKPT* covers all cars in hires/ultrawide
    # FRANK*/FRNK2* covers Eagle/New Eagle in lowres
    files=$(find "$src_dir" -maxdepth 1 -iname "CKPT*.PIX" -o -iname "FRANK*.PIX" -o -iname "FRNK2*.PIX")

    if [ -z "$files" ]; then
        echo "SKIP: no cockpit files in $src_dir"
        continue
    fi

    count=$(echo "$files" | wc -l)
    echo "Copying $count cockpit files from $res..."
    echo "$files" | xargs cp -t "$dst_dir"
done

echo "Done."
