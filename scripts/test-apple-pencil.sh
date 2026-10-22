#!/usr/bin/env bash
# Pure input policies and authenticated loopback checks; no physical devices,
# Keychain access, application launch, or Host connections.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "usage: $0 <verified-macos-recipe-work> <fresh-output-directory>" >&2
    exit 2
fi
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(cd -- "$1" && pwd)
[[ ! -e "$2" ]] || { echo "Use a fresh output directory" >&2; exit 2; }
python3 "$root/scripts/apple-native/build.py" verify --platform macos --work "$work"
mkdir -p "$2"
out=$(cd -- "$2" && pwd)
cd "$root"
shared=apple-native/Shared
raw="$work/git/raw/src"
prefix="$work/install"
queue=visionos-native/Sources/Services/PlankInputQueue.swift
common=("$queue" "$shared/PlankControlKeyOwnership.swift"
    "$shared/PlankPencilModifiers.swift" "$shared/PlankPencilRelayWire.swift")
swift=(xcrun --sdk macosx swiftc -swift-version 6 -strict-concurrency=complete -Onone -parse-as-library)
for name in PencilRelayChecks PencilModifierChecks; do
    "${swift[@]}" "${common[@]}" "ios-native/Tests/$name.swift" -o "$out/$name"
    "$out/$name"
done
"${swift[@]}" "${common[@]}" apple-native/Tests/PencilRelayDiagnosticChecks.swift -o "$out/diagnostics"
"$out/diagnostics"

clang=(xcrun --sdk macosx clang -I"$shared" -I"$raw" -I"$prefix/include")
"${clang[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$shared/PlankPencilCrypto.c" "$raw/noise.c" ios-native/Tests/test_pencil_crypto.c \
    "$prefix/lib/libsodium.a" -o "$out/crypto"
"$out/crypto"
"${clang[@]}" -c "$shared/PlankPencilCrypto.c" -o "$out/crypto.o"
"${clang[@]}" -c "$raw/noise.c" -o "$out/noise.o"
"${swift[@]}" "${common[@]}" "$shared/PlankPencilRelayKeys.swift" "$shared/PlankPencilRelayPeer.swift" \
    -import-objc-header "$shared/PlankPencilCrypto.h" \
    ios-native/Tests/PencilRelaySocketChecks.swift "$out/crypto.o" "$out/noise.o" \
    "$prefix/lib/libsodium.a" -framework Security -o "$out/socket"
"$out/socket"
