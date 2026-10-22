#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 ]]; then echo "usage: $0 <output-dir>" >&2; exit 2; fi
for name in PLANK_RELAY_SOURCE_DIR PLANK_MANAGED_RELAY_SOURCE_DIR PLANK_RELAY_SODIUM_PREFIX; do
    if [[ -z ${!name:-} ]]; then echo "missing $name" >&2; exit 2; fi
done
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$1"; out=$(cd -- "$1" && pwd); cd "$root"
raw="$PLANK_RELAY_SOURCE_DIR/src"; managed="$PLANK_MANAGED_RELAY_SOURCE_DIR/src"; sodium="$PLANK_RELAY_SODIUM_PREFIX"
objects=()
for part in protocol session noise cpace confirm link client_link client_pair client_enrollment identity; do
    xcrun clang -O0 -D_DARWIN_C_SOURCE -I"$raw" -I"$sodium/include" -c "$raw/$part.c" -o "$out/raw-$part.o"
    objects+=("$out/raw-$part.o")
done
ar rcs "$out/raw.a" "${objects[@]}"
objects=()
for part in ble_lab protocol session link noise cpace confirm identity pair_budget pairing pair_wire client_link; do
    xcrun clang -O0 -D_DARWIN_C_SOURCE -DPLTR_SOFTWARE_VERSION=\"mac-relay-test\" \
        -include apple-native/Bridge/PlankMacSetupNamespace.h -I"$managed" -I"$sodium/include" -c "$managed/$part.c" -o "$out/setup-$part.o"
    objects+=("$out/setup-$part.o")
done
xcrun clang -O0 -Iapple-native/Bridge -I"$managed" -I"$sodium/include" -c apple-native/Bridge/PlankMacSetup.c -o "$out/setup-wrapper.o"
objects+=("$out/setup-wrapper.o")
ar rcs "$out/setup.a" "${objects[@]}"
xcrun clang -O0 -D_DARWIN_C_SOURCE -Iapple-native/Bridge -I"$managed" -I"$sodium/include" apple-native/Tests/PlankMacSetupTests.c \
    "$out/setup.a" "$sodium/lib/libsodium.a" -o "$out/setup-test"
"$out/setup-test"
cpp=(-std=c++17 -O0 -DPLANK_NATIVE_MAC_WACOM -Iapple-native/Bridge -Iapp/streaming/input -Imoonlight-common-c/moonlight-common-c/src -I"$raw" -I"$sodium/include")
xcrun clang++ "${cpp[@]}" apple-native/Bridge/PlankMacRelay.cpp apple-native/Tests/PlankMacRelayTests.cpp \
    "$out/raw.a" "$sodium/lib/libsodium.a" -o "$out/relay-core"
"$out/relay-core"
xcrun clang++ "${cpp[@]}" -DPLANK_MAC_RELAY_TEST_NO_MAIN -c apple-native/Tests/PlankMacRelayTests.cpp -o "$out/worker.o"
xcrun clang++ "${cpp[@]}" -c apple-native/Bridge/PlankMacRelay.cpp -o "$out/relay.o"
xcrun clang -O0 -Iapple-native/Bridge -c apple-native/Bridge/PlankMacRelayPermissions.c -o "$out/permission.o"
xcrun swiftc -swift-version 6 -Onone -parse-as-library -import-objc-header apple-native/Bridge/PlankMacRelay.h \
    -Xcc -include -Xcc apple-native/Bridge/PlankMacSetup.h \
    -Xcc -include -Xcc apple-native/Tests/PlankMacRelaySocketFixture.h \
    apple-native/Sources/PlankMacRelayHost.swift apple-native/Sources/PlankMacRelayUSBInventory.swift visionos-native/Sources/Models/PlankDrawingHandoff.swift \
    apple-native/Tests/PlankMacRelaySocketTests.swift "$out/worker.o" "$out/relay.o" "$out/permission.o" \
    "$out/setup.a" "$out/raw.a" "$sodium/lib/libsodium.a" -lc++ -framework IOKit -o "$out/relay-socket"
"$out/relay-socket"
