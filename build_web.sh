#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

: "${RAYLIB_SRC:=$HOME/Downloads/raylib-5.5/src}"
: "${EMSDK_PYTHON:=/opt/homebrew/opt/python@3.14/bin/python3.14}"

export EMSDK_PYTHON
export PATH="/opt/homebrew/opt/binaryen/bin:$PATH"

if [[ ! -f "$RAYLIB_SRC/libraylib.a" ]]; then
    echo "building raylib for web..."
    (cd "$RAYLIB_SRC" && emmake make PLATFORM=PLATFORM_WEB)
fi

mkdir -p web
emcc main.c \
  -o web/index.html \
  -I "$RAYLIB_SRC" \
  "$RAYLIB_SRC/libraylib.a" \
  -Os -Wall \
  -s USE_GLFW=3 -s ASYNCIFY \
  -s TOTAL_MEMORY=67108864 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","lengthBytesUTF8"]' \
  --preload-file assets \
  --preload-file sprites \
  --preload-file guybrush_sprites_v3 \
  --preload-file walkbox.txt \
  --preload-file fg.txt \
  --preload-file sprites.txt \
  --shell-file "$RAYLIB_SRC/minshell.html" \
  -DPLATFORM_WEB
