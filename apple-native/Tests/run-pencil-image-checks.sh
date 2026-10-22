#!/usr/bin/env bash
# Focused reference-image checks. Only loopback listeners; no Host/device/Keychain writes.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "usage: $0 <verified-macos-recipe-work> <fresh-output-directory>" >&2
    exit 2
fi
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(cd -- "$1" && pwd)
output=$2
[[ ! -e "$output" ]] || { echo "Use a fresh output directory" >&2; exit 2; }
python3 "$repo/scripts/apple-native/build.py" verify --platform macos --work "$work"
mkdir -p "$output"
output=$(cd -- "$output" && pwd)
shared="$repo/apple-native/Shared"
raw="$work/git/raw/src"
prefix="$work/install"
clang=(xcrun --sdk macosx clang -I"$shared" -I"$raw" -I"$prefix/include")
"${clang[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$shared/PlankPencilCrypto.c" "$shared/PlankPencilImageCrypto.c" "$raw/noise.c" \
    "$repo/apple-native/Tests/test_pencil_image_crypto.c" "$prefix/lib/libsodium.a" \
    -o "$output/crypto-checks"
"$output/crypto-checks"
xcrun --sdk macosx swiftc -swift-version 6 -strict-concurrency=complete -parse-as-library \
    "$shared/PlankPencilImageWire.swift" "$repo/apple-native/Tests/PencilImageWireChecks.swift" \
    -o "$output/wire-checks"
"$output/wire-checks"
for part in PlankPencilCrypto PlankPencilImageCrypto; do
    "${clang[@]}" -c "$shared/$part.c" -o "$output/$part.o"
done
"${clang[@]}" -c "$raw/noise.c" -o "$output/noise.o"
printf '#include "PlankPencilCrypto.h"\n#include "PlankPencilImageCrypto.h"\n' > "$output/Bridge.h"
xcrun --sdk macosx swiftc -swift-version 6 -strict-concurrency=complete -parse-as-library \
    -I"$shared" -import-objc-header "$output/Bridge.h" \
    "$repo/visionos-native/Sources/Services/PlankInputQueue.swift" \
    "$shared/PlankControlKeyOwnership.swift" "$shared/PlankPencilModifiers.swift" "$shared/PlankPencilRelayWire.swift" \
    "$shared/PlankPencilRelayKeys.swift" "$shared/PlankPencilRelayPeer.swift" \
    "$shared/PlankPencilImageWire.swift" "$shared/PlankPencilImagePeer.swift" \
    "$repo/apple-native/Tests/PencilImageSocketChecks.swift" \
    "$output/PlankPencilCrypto.o" "$output/PlankPencilImageCrypto.o" "$output/noise.o" \
    "$prefix/lib/libsodium.a" -framework Security -o "$output/socket-checks"
"$output/socket-checks"
# Extract the exact production field-only frame model, avoiding UI/Host model dependencies.
python3 - "$repo" "$output" <<'PY'
from pathlib import Path
import sys
repo, output = map(Path, sys.argv[1:])
source = (repo / 'visionos-native/Sources/Models/HostBookmark.swift').read_text()
start = source.index('struct PlankRenderedFrame: @unchecked Sendable {')
end = source.index('\n}', start) + 2
body = source[start:end]
assert body.count('{') == 1 and body.count('}') == 1
(output / 'Frame.swift').write_text('import Foundation\nimport CoreVideo\n' + body + '\n')
PY
# Only this native conversion component is optimized; Swift stays in its normal
# Debug configuration. Exercise direct C admission and unaligned/endian storage.
conversion="$repo/visionos-native/Bridge"
xcrun --sdk macosx clang -O2 -fno-fast-math -ffp-contract=off \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$conversion" \
    "$conversion/PlankPencilImageConversion.c" \
    "$repo/visionos-native/Tests/test_pencil_image_conversion.c" -o "$output/conversion-checks"
"$output/conversion-checks"
xcrun --sdk macosx clang -O2 -fno-fast-math -ffp-contract=off -fsanitize=address \
    -c "$conversion/PlankPencilImageConversion.c" -o "$output/PlankPencilImageConversion.o"
xcrun --sdk macosx swiftc -swift-version 6 -strict-concurrency=complete -sanitize=address \
    -import-objc-header "$repo/visionos-native/Bridge/PlankPencilImageConversion.h" \
    -parse-as-library "$output/Frame.swift" \
    "$repo/visionos-native/Sources/Services/PlankPencilReferenceImageCodec.swift" \
    "$repo/visionos-native/Tests/PlankPencilReferenceImageCodecTests.swift" \
    "$output/PlankPencilImageConversion.o" -o "$output/codec-checks"
"$output/codec-checks"
