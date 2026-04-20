#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

APP="scumm-game.app"
SRC_ICON="guybrush_sprites_v3/sprite_249.png"
BG_COLOR="0A0E1E"

if [[ ! -f scumm-game ]]; then
    echo "building binary..."
    make
fi
[[ -f "$SRC_ICON" ]] || { echo "missing $SRC_ICON" >&2; exit 1; }

tmp=$(mktemp -d)
trap "rm -rf '$tmp'" EXIT

sips -Z 880 "$SRC_ICON" --out "$tmp/scaled.png" > /dev/null
sips --padToHeightWidth 1024 1024 --padColor "$BG_COLOR" \
     "$tmp/scaled.png" --out "$tmp/icon_1024.png" > /dev/null

iconset="$tmp/scumm-game.iconset"
mkdir -p "$iconset"
for pair in "16 icon_16x16" "32 icon_16x16@2x" "32 icon_32x32" "64 icon_32x32@2x" \
            "128 icon_128x128" "256 icon_128x128@2x" "256 icon_256x256" \
            "512 icon_256x256@2x" "512 icon_512x512" "1024 icon_512x512@2x"; do
    size="${pair%% *}"
    name="${pair#* }"
    sips -z "$size" "$size" "$tmp/icon_1024.png" --out "$iconset/${name}.png" > /dev/null
done

iconutil -c icns "$iconset" -o "$tmp/scumm-game.icns"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$tmp/scumm-game.icns" "$APP/Contents/Resources/scumm-game.icns"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>scumm-game</string>
    <key>CFBundleDisplayName</key>       <string>scumm-game</string>
    <key>CFBundleIdentifier</key>        <string>com.binrick.scumm-game</string>
    <key>CFBundleVersion</key>           <string>1</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleSignature</key>         <string>????</string>
    <key>CFBundleExecutable</key>        <string>scumm-game</string>
    <key>CFBundleIconFile</key>          <string>scumm-game</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>LSMinimumSystemVersion</key>    <string>10.13</string>
</dict>
</plist>
PLIST

cat > "$APP/Contents/MacOS/scumm-game" <<'LAUNCH'
#!/bin/bash
set -e
HERE="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$HERE"
exec ./scumm-game "$@"
LAUNCH
chmod +x "$APP/Contents/MacOS/scumm-game"

touch "$APP"
echo "built $APP"
