#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 ]]; then echo "usage: $0 <build-dir>" >&2; exit 2; fi
for name in PLANK_TRANSPORT_DIR PLANK_FFMPEG_DIR PLANK_OPUS_DIR PLANK_RELAY_SOURCE_DIR PLANK_RELAY_SODIUM_PREFIX PLANK_MANAGED_RELAY_SOURCE_DIR; do
    if [[ -z ${!name:-} ]]; then echo "missing $name" >&2; exit 2; fi
done
source_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$source_root/apple-native" -B "$1" -G Xcode \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT=macosx -DPLANK_APPLE_BUILD_NUMBER="${PLANK_APPLE_BUILD_NUMBER:-16}" \
    -DPLANK_TRANSPORT_DIR="$PLANK_TRANSPORT_DIR" -DPLANK_FFMPEG_DIR="$PLANK_FFMPEG_DIR" \
    -DPLANK_OPUS_DIR="$PLANK_OPUS_DIR" -DPLANK_RELAY_SOURCE_DIR="$PLANK_RELAY_SOURCE_DIR" \
    -DPLANK_RELAY_SODIUM_PREFIX="$PLANK_RELAY_SODIUM_PREFIX" \
    -DPLANK_MANAGED_RELAY_SOURCE_DIR="$PLANK_MANAGED_RELAY_SOURCE_DIR"
cmake --build "$1" --config Debug
