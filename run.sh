#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make
if [[ ! -d scumm-game.app ]]; then
    ./build_app.sh
fi
exec open -W scumm-game.app
