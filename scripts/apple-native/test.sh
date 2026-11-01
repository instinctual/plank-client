#!/usr/bin/env bash
# Run pure native policies on the build Mac; does not connect to a Host/Relay.
set -euo pipefail
if [[ $# != 1 ]]; then echo "usage: $0 <fresh-test-output-directory>" >&2; exit 2; fi
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
output=$1
[[ ! -e "$output" ]] || { echo "Use a fresh test output directory" >&2; exit 2; }
mkdir -p "$output"
python3 "$repo/scripts/apple-native/test_build.py"
vision="$repo/visionos-native"
for name in WacomPreflight TabletInputPolicy SessionFocus VideoDecoderRecovery RelayApprovalRoutes; do
    command=(xcrun --sdk macosx swiftc -Onone -parse-as-library
        "$vision/Sources/Services/Plank${name}.swift")
    if [[ $name == VideoDecoderRecovery ]]; then
        command+=("$vision/Sources/Services/PlankHardwareVideoDecoder.swift")
    fi
    command+=("$vision/Tests/Plank${name}Tests.swift" -o "$output/$name")
    "${command[@]}"
    "$output/$name"
done
for name in AudioFormat AudioLevel; do
    xcrun --sdk macosx swiftc -Onone -parse-as-library \
        "$vision/Sources/Models/Plank${name}.swift" \
        "$vision/Tests/Plank${name}Tests.swift" -o "$output/$name"
    "$output/$name"
done
xcrun --sdk macosx swiftc -swift-version 6 -Onone -parse-as-library \
    "$vision/Sources/Models/HostBookmark.swift" \
    "$vision/Tests/PlankBookmarkSessionPolicyTests.swift" -o "$output/bookmark-session"
"$output/bookmark-session"
xcrun --sdk macosx swiftc -swift-version 6 -Onone -parse-as-library \
    "$vision/Sources/Models/HostBookmark.swift" \
    "$vision/Sources/Models/PlankDesktopCanvas.swift" \
    "$vision/Sources/Models/PlankTopologyDecoder.swift" \
    "$vision/Sources/Models/PlankStreamRequest.swift" \
    "$vision/Sources/Services/HostStore.swift" \
    "$vision/Tests/PlankDesktopCanvasTests.swift" -o "$output/desktop-canvas"
"$output/desktop-canvas" "$repo/apple-native/Tests/Fixtures/output-topology-v13.json"
xcrun --sdk macosx clang "$vision/Bridge/PlankRawHidFrame.c" \
    "$vision/Tests/test_raw_hid_frame.c" -o "$output/raw-hid-frame"
"$output/raw-hid-frame"
printf 'Native pure policy and raw frame checks passed.\n'
