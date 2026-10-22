#!/usr/bin/env python3
"""Compile exact production diagnostic ledgers with the existing input queue.

No app, device, Host, network, GUI, signing or dependency operation occurs.
These fixtures check local bookkeeping, not physical latency or Host receipt.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def declaration(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, cursor = 1, opening + 1
    while depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    return source[start:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    output = args.output or pathlib.Path(tempfile.mkdtemp(prefix="plank-pencil-flow-"))
    output.mkdir(parents=True, exist_ok=True)
    receiver = "visionos-native/Sources/Services/PlankPencilRelayReceiver.swift"
    sender = "visionos-native/Sources/Services/PlankSessionEngine.swift"
    checks = "visionos-native/Tests/PlankPencilFlowDiagnosticsChecks.swift"
    inputs = [receiver, sender, checks, "visionos-native/Sources/Services/PlankInputQueue.swift",
              "apple-native/Shared/PlankControlKeyOwnership.swift"]
    extracted = "import Foundation\n"
    extracted += declaration((REPO / receiver).read_text(), "private final class PlankPencilReceiverFlowDiagnostics:") + "\n"
    extracted += declaration((REPO / sender).read_text(), "private final class PlankInputSenderState:") + "\n"
    extracted += (REPO / checks).read_text()
    fixture = output / "ExtractedDiagnosticsChecks.swift"
    fixture.write_text(extracted)
    binary = output / "PlankPencilFlowDiagnosticsChecks"
    command = ["xcrun", "--sdk", "macosx", "swiftc", "-swift-version", "6", "-strict-concurrency=complete",
               "-Onone", "-parse-as-library", "-DPLANK_PENCIL_RELAY_RECEIVER", str(fixture),
               str(REPO / inputs[3]), str(REPO / inputs[4]), "-o", str(binary)]
    compiled = subprocess.run(command, text=True, capture_output=True)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr)
    ran = subprocess.run([str(binary)], text=True, capture_output=True) if compiled.returncode == 0 else None
    if ran:
        (output / "run.log").write_text(ran.stdout + ran.stderr)
    passed = ran is not None and ran.returncode == 0
    receipt = {
        "status": "PASS" if passed else "FAIL", "compileExitCode": compiled.returncode,
        "runExitCode": ran.returncode if ran else None, "command": command,
        "sources": {path: hashlib.sha256((REPO / path).read_bytes()).hexdigest() for path in inputs},
        "runnerSHA256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        "limitations": ["Exact diagnostic classes and queue are exercised, not production actor/socket scheduling.",
                        "No physical slowdown cause, repair, native QUIC backlog or Host receipt is established.",
                        "No app/device/Host/network/dependency mutation occurs."],
    }
    (output / "verification.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print((ran.stdout + ran.stderr) if ran else compiled.stdout + compiled.stderr, end="")
    print(f"Evidence: {output / 'verification.json'}")
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
