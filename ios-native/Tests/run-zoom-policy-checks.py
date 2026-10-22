#!/usr/bin/env python3
"""Run native zoom policies and unchanged Fit/Pencil acceptance on the build Mac.

Only local Swift fixtures are compiled. No application, device, GUI, Host,
network, persistent preferences, dependencies or signing are touched.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    output = args.output or pathlib.Path(tempfile.mkdtemp(prefix="plank-zoom-policy-"))
    output.mkdir(parents=True, exist_ok=True)
    shared = ["visionos-native/Sources/Models/HostBookmark.swift",
              "visionos-native/Sources/Services/PlankInputQueue.swift",
              "apple-native/Shared/PlankControlKeyOwnership.swift",
              "ios-native/Sources/PlankIPadInputPolicy.swift",
              "ios-native/Sources/PlankIPadKeys.swift",
              "ios-native/Sources/PlankIPadDisplayOptions.swift",
              "ios-native/Sources/PlankIPadPencilPolicy.swift"]
    cases = {
        "ZoomPolicyChecks": ["ios-native/Sources/PlankIPadZoom.swift", "ios-native/Tests/ZoomPolicyChecks.swift"],
        "InputPolicyChecks": ["ios-native/Tests/InputPolicyChecks.swift"],
        "PencilPolicyChecks": ["ios-native/Tests/PencilPolicyChecks.swift"],
    }
    receipts = []
    for name, extra in cases.items():
        inputs = shared + extra
        binary = output / name
        command = ["xcrun", "--sdk", "macosx", "swiftc", "-swift-version", "6",
                   "-strict-concurrency=complete", "-Onone", "-parse-as-library",
                   *(str(REPO / path) for path in inputs), "-o", str(binary)]
        compiled = subprocess.run(command, text=True, capture_output=True)
        (output / f"{name}-compile.log").write_text(compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], text=True, capture_output=True) if compiled.returncode == 0 else None
        if ran:
            (output / f"{name}-run.log").write_text(ran.stdout + ran.stderr)
        result = {
            "name": name,
            "status": "PASS" if ran and ran.returncode == 0 else "FAIL",
            "compileExitCode": compiled.returncode,
            "runExitCode": ran.returncode if ran else None,
            "command": command,
            "stdout": ran.stdout if ran else compiled.stdout,
            "stderr": ran.stderr if ran else compiled.stderr,
            "sources": {path: hashlib.sha256((REPO / path).read_bytes()).hexdigest() for path in inputs},
        }
        receipts.append(result)
        print(result["stdout"] + result["stderr"], end="")
        if result["status"] != "PASS":
            break
    passed = len(receipts) == len(cases) and all(result["status"] == "PASS" for result in receipts)
    receipt = {
        "status": "PASS" if passed else "FAIL",
        "checks": receipts,
        "runnerSHA256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        "limitations": [
            "Real UIKit gesture delivery/arbitration, Metal/CGImage clipping and physical Pencil alignment are not exercised.",
            "Modeled contact uses production Pencil policy/held-input ledgers; final transport/Host delivery is not established.",
            "No device, application build, signing, dependency change or Host/network action occurs.",
        ],
    }
    (output / "verification.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Zoom policy evidence: {output / 'verification.json'}")
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
