#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 ]]; then echo "usage: $0 <output-dir>" >&2; exit 2; fi
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$1"
out=$(cd -- "$1" && pwd)
cd "$root"
xcrun swiftc -swift-version 6 -Onone -parse-as-library -DPLANK_NATIVE_MAC_WACOM \
    apple-native/Sources/PlankMacInputPolicy.swift visionos-native/Sources/Services/PlankInputQueue.swift \
    visionos-native/Sources/Services/PlankWacomPreflight.swift apple-native/Tests/PlankMacPolicyTests.swift -o "$out/mac-policy"
"$out/mac-policy"
xcrun swiftc -swift-version 6 -Onone -parse-as-library \
    apple-native/Sources/PlankMacCursorOverlay.swift apple-native/Sources/PlankMacWindowPresentation.swift apple-native/Sources/PlankMacInputPolicy.swift \
    apple-native/Sources/PlankMacDisplayGeometry.swift \
    visionos-native/Sources/Models/HostBookmark.swift apple-native/Tests/PlankMacPresentationTests.swift -o "$out/mac-presentation"
"$out/mac-presentation"
xcrun swiftc -swift-version 6 -Onone -parse-as-library \
    apple-native/Sources/PlankMacWindowPresentation.swift apple-native/Sources/PlankMacInput.swift apple-native/Sources/PlankMacLocalControls.swift \
    apple-native/Sources/PlankMacCursorOverlay.swift apple-native/Sources/PlankMacInputPolicy.swift \
    apple-native/Sources/PlankMacDisplayGeometry.swift apple-native/Sources/PlankMacSessionWindows.swift \
    visionos-native/Sources/Models/HostBookmark.swift apple-native/Tests/PlankMacLocalControlsTests.swift -o "$out/mac-local-controls"
"$out/mac-local-controls"
xcrun clang++ -std=c++17 -O0 -DPLANK_NATIVE_MAC_WACOM -Iapple-native/Bridge -Iapp/streaming/input \
    -Imoonlight-common-c/moonlight-common-c/src apple-native/Bridge/PlankMacWacom.cpp \
    apple-native/Tests/PlankMacWacomLifetimeTests.cpp -o "$out/mac-wacom-lifetime"
"$out/mac-wacom-lifetime"
xcrun clang++ -std=c++17 -O0 -DPLANK_NATIVE_MAC_WACOM -Iapple-native/Bridge -Iapp/streaming/input \
    -Imoonlight-common-c/moonlight-common-c/src -c apple-native/Bridge/PlankMacWacom.cpp -o "$out/mac-wacom-wrapper.o"
xcrun clang++ -std=c++17 -O0 -DPLANK_NATIVE_MAC_WACOM -Iapple-native/Bridge -Iapp/streaming/input \
    -Imoonlight-common-c/moonlight-common-c/src -c apple-native/Tests/PlankMacWacomWorkerDriver.cpp -o "$out/mac-wacom-worker.o"
xcrun swiftc -swift-version 6 -Onone -parse-as-library -DPLANK_NATIVE_MAC_WACOM \
    -import-objc-header apple-native/Tests/PlankMacWacomWorkerDriver.h -Iapple-native/Bridge \
    apple-native/Sources/PlankMacWacomSession.swift apple-native/Sources/PlankMacInputPolicy.swift \
    visionos-native/Sources/Services/PlankInputQueue.swift visionos-native/Sources/Services/PlankWacomPreflight.swift \
    visionos-native/Sources/Services/PlankTabletInputPolicy.swift \
    apple-native/Tests/PlankMacWacomWorkerTests.swift "$out/mac-wacom-wrapper.o" "$out/mac-wacom-worker.o" \
    -lc++ -o "$out/mac-wacom-worker"
"$out/mac-wacom-worker"
xcrun swiftc -swift-version 6 -Onone -parse-as-library \
    visionos-native/Sources/Services/PlankWacomPreflight.swift visionos-native/Tests/PlankWacomPreflightTests.swift -o "$out/relay-preflight"
"$out/relay-preflight" > "$out/relay-preflight.log" 2>&1
xcrun swiftc -swift-version 6 -Onone -parse-as-library \
    visionos-native/Sources/Services/PlankHardwareVideoDecoder.swift visionos-native/Sources/Services/PlankVideoDecoderRecovery.swift \
    visionos-native/Tests/PlankVideoDecoderRecoveryTests.swift -o "$out/decoder-recovery"
"$out/decoder-recovery"
xcrun swiftc -swift-version 6 -Onone -parse-as-library \
    visionos-native/Sources/Services/PlankTabletInputPolicy.swift visionos-native/Tests/PlankTabletInputPolicyTests.swift -o "$out/tablet-policy"
"$out/tablet-policy"

xcrun swiftc -swift-version 6 -Onone -parse-as-library \
    apple-native/Sources/PlankMacDisplayGeometry.swift apple-native/Sources/PlankMacInputPolicy.swift \
    apple-native/Sources/PlankMacCursorOverlay.swift apple-native/Sources/PlankMacWindowPresentation.swift \
    visionos-native/Sources/Models/HostBookmark.swift visionos-native/Sources/Models/PlankTopologyDecoder.swift \
    visionos-native/Sources/Services/PlankVideoSurfaces.swift apple-native/Tests/PlankMacMultipleScreensTests.swift -o "$out/mac-multiple-screens"
"$out/mac-multiple-screens" apple-native/Tests/Fixtures/output-topology-v13.json
