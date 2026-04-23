#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ACTOR="${1:-guybrush}"
SRC="${ACTOR}_sprites_v3"
DST="actors/$ACTOR"
META="$DST/sprites.txt"

# legacy fallback: some older layouts used a flat guybrush_sprites_v3/
[[ -d "$SRC" ]] || SRC="guybrush_sprites_v3"

if [[ ! -f "$META" ]]; then
    echo "missing $META (save from browser mode first)" >&2
    exit 1
fi

rm -rf "$DST"
mkdir -p "$DST"

while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    name="${line%% *}"
    rest="${line#"$name"}"
    rest="${rest# }"
    mkdir -p "$DST/$name"
    [[ -z "$rest" ]] && continue
    idx=1
    for frame in $rest; do
        src="$SRC/sprite_$(printf "%03d" "$frame").png"
        dst="$DST/$name/$(printf "%02d" "$idx").png"
        if [[ ! -f "$src" ]]; then
            echo "  missing: $src" >&2
            continue
        fi
        cp "$src" "$dst"
        idx=$((idx + 1))
    done
    echo "  $name: $((idx - 1)) frame(s)"
done < "$META"

if compgen -G "$DST/walk_right/*.png" > /dev/null; then
    mkdir -p "$DST/walk_left"
    rm -f "$DST/walk_left"/*.png
    idx=1
    for src in "$DST/walk_right"/*.png; do
        dst="$DST/walk_left/$(printf "%02d" "$idx").png"
        sips -f horizontal "$src" --out "$dst" > /dev/null
        idx=$((idx + 1))
    done
    echo "  walk_left: mirrored $((idx - 1)) frame(s) from walk_right"
fi

echo "exported to $DST/"
