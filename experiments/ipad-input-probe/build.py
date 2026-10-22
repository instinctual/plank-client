#!/usr/bin/env python3
"""Build an unsigned standalone probe. Never install or launch an app."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess

SOURCE = Path(__file__).resolve().parent


def run(arguments):
    subprocess.run(arguments, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=["device", "simulator"], required=True)
    parser.add_argument("--work", type=Path, required=True,
                        help="New build directory outside the source checkout")
    args = parser.parse_args()
    work = args.work.expanduser().resolve()
    root = SOURCE.parents[1]
    if work == root or root in work.parents or work.exists():
        parser.error("--work must be a new directory outside the source checkout")
    work.mkdir(parents=True)
    sdk = "iphoneos" if args.platform == "device" else "iphonesimulator"
    run(["cmake", "-S", str(SOURCE), "-B", str(work / "build"), "-G", "Xcode",
         "-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={sdk}",
         "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=18.0"])
    run(["cmake", "--build", str(work / "build"), "--config", "Release",
         "--", "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"])
    bundles = list((work / "build").glob("Release-*/*.app"))
    if len(bundles) != 1:
        raise RuntimeError(f"Expected one probe bundle, found {len(bundles)}")
    app = bundles[0]
    with (app / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleIdentifier") != "com.instinctual.plank.inputprobe":
        raise RuntimeError("Unexpected bundle identity")
    if info.get("UIApplicationSupportsIndirectInputEvents") is not True:
        raise RuntimeError("Indirect-pointer input is not enabled")
    binary = app / info["CFBundleExecutable"]
    inputs = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(SOURCE.iterdir()) if p.is_file()}
    receipt = {
        "schema": 1, "platform": args.platform, "deploymentTarget": "18.0",
        "sourceCommit": subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "sourceFilesSHA256": inputs, "bundle": str(app), "signed": False,
        "installed": False, "executableSHA256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "bundleIdentifier": info["CFBundleIdentifier"], "build": info["CFBundleVersion"],
        "xcode": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(),
        "sdk": subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-version"], text=True).strip(),
    }
    (work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Unsigned probe: {app}\nReceipt: {work / 'receipt.json'}")


if __name__ == "__main__":
    main()
