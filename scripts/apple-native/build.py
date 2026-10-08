#!/usr/bin/env python3
"""Pinned, unsigned Apple native builds. No system installs or device operations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tarfile

RECIPE = Path(__file__).resolve().parent
REPOSITORY = RECIPE.parent.parent
MANIFEST = RECIPE / "inputs.json"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(args, cwd=None, env=None):
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def output(args, cwd=None):
    return subprocess.check_output([str(a) for a in args], cwd=cwd, text=True).strip()


def verified_archive(cache, entry):
    archive = cache / entry["name"]
    cache.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(archive.suffix + ".partial")
        run(["curl", "--fail", "--location", "--retry", "3", "--connect-timeout",
             "30", "--max-time", "900", "--output", partial, entry["url"]])
        require(digest(partial) == entry["sha256"], f"Checksum mismatch: {entry['name']}")
        partial.rename(archive)
    require(digest(archive) == entry["sha256"], f"Checksum mismatch: {entry['name']}")
    return archive


def tree_inventory(source):
    result = {}
    for path in source.rglob("*"):
        key = path.relative_to(source).as_posix()
        if path.is_symlink():
            result[key] = ("link", os.readlink(path))
        elif path.is_file():
            result[key] = ("file", digest(path), bool(path.stat().st_mode & 0o111))
    return result


def verify_source(archive, source, entry, modified=None):
    """Compare EVERY source file, including unexpected .orig/.rej files and links."""
    modified = modified or {}
    expected = {}
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            parts = Path(member.name).parts
            require(parts[0] == entry["directory"], "Unexpected archive root")
            key = Path(*parts[1:]).as_posix()
            if member.isfile():
                data = bundle.extractfile(member).read()
                expected[key] = ("file", modified.get(key, hashlib.sha256(data).hexdigest()),
                                 bool(member.mode & 0o111))
            elif member.issym():
                expected[key] = ("link", member.linkname)
            else:
                require(member.isdir(), "Unsupported archive entry")
    require(set(modified) <= set(expected), "Patch names absent source files")
    actual = tree_inventory(source)
    changed = sorted(k for k in expected.keys() | actual.keys()
                     if expected.get(k) != actual.get(k))
    require(not changed, "Source differs from pinned archive/patch: " + ", ".join(changed[:8]))


def verify_git(path, commit):
    require(output(["git", "rev-parse", "HEAD"], path) == commit, "Git commit mismatch")
    require(not output(["git", "status", "--porcelain", "--untracked-files=all"], path),
            "Git input is dirty")


def checkout(path, pin):
    require(not path.exists(), "Git destination already exists")
    path.mkdir(parents=True)
    run(["git", "init", "--quiet", path])
    run(["git", "-c", "credential.helper=", "-c", "core.askPass=", "fetch",
         "--no-recurse-submodules", "--depth=1", pin["url"], pin["commit"]], path,
        dict(os.environ, GIT_TERMINAL_PROMPT="0"))
    run(["git", "checkout", "--quiet", "--detach", "FETCH_HEAD"], path)
    verify_git(path, pin["commit"])


def patch_for(inputs, target):
    return inputs["ffmpeg_patches"]["macos" if target == "macos" else "visionos"]


def git_paths(work, target):
    paths = {"root": work / "git/root", "raw": work / "git/raw"}
    if target == "macos":
        paths["managed"] = work / "git/managed"
    paths["kymux"] = paths["root"] / "third_party/kyber-kymux"
    return paths


def prepare(inputs, args):
    require(not args.work.exists(), "Use a fresh work directory; only archive caches are reusable")
    # Verify the recipe patch before any download or mutation.
    patch = patch_for(inputs, args.platform)
    require(digest(REPOSITORY / patch["path"]) == patch["sha256"], "Patch checksum mismatch")
    args.work.mkdir(parents=True)
    for name, entry in inputs["archives"].items():
        archive = verified_archive(args.cache, entry)
        source_parent = args.work / "source"
        source_parent.mkdir(exist_ok=True)
        with tarfile.open(archive) as bundle:
            bundle.extractall(source_parent, filter="data")
        source = source_parent / entry["directory"]
        verify_source(archive, source, entry)
        if name == "ffmpeg":
            if args.platform == "macos":
                # Preserve the maintained patch bytes/attribution. FFmpeg 9.0.1
                # changed surrounding context; the full expected file hash below
                # is the authority, not patch's context matching alone.
                run(["patch", "--batch", "--forward", "-V", "none", "-p1", "-i",
                     REPOSITORY / patch["path"]], source)
            else:
                run(["git", "apply", "--check", REPOSITORY / patch["path"]], source)
                run(["git", "apply", REPOSITORY / patch["path"]], source)
            verify_source(archive, source, entry, patch["modified_files"])
    for name, path in git_paths(args.work, args.platform).items():
        # An uninitialized gitlink is an empty directory after checking out the root.
        if path.is_dir() and not any(path.iterdir()):
            path.rmdir()
        checkout(path, inputs["git"][name])
    run(["git", "submodule", "status", "third_party/kyber-kymux"], args.work / "git/root")
    (args.work / "prepared.json").write_text(json.dumps({
        "manifest_sha256": digest(MANIFEST), "platform": args.platform,
        "cache": str(args.cache)}, indent=2) + "\n")


def check_prepared(inputs, args):
    receipt = json.loads((args.work / "prepared.json").read_text())
    require(receipt["manifest_sha256"] == digest(MANIFEST), "Build-input manifest changed")
    require(receipt["platform"] == args.platform, "Prepared platform mismatch")
    patch = patch_for(inputs, args.platform)
    require(digest(REPOSITORY / patch["path"]) == patch["sha256"], "Patch checksum mismatch")
    cache = Path(receipt["cache"])
    for name, entry in inputs["archives"].items():
        archive = cache / entry["name"]
        require(digest(archive) == entry["sha256"], "Archive checksum mismatch")
        verify_source(archive, args.work / "source" / entry["directory"], entry,
                      patch["modified_files"] if name == "ffmpeg" else None)
    for name, path in git_paths(args.work, args.platform).items():
        verify_git(path, inputs["git"][name]["commit"])
    root = args.work / "git/root"
    pin = output(["git", "rev-parse", "HEAD:third_party/kyber-kymux"], root)
    require(pin == inputs["git"]["kymux"]["commit"], "Root Kymux gitlink mismatch")
    toolchain = (root / "rust-toolchain.toml").read_text()
    require(f'channel = "{inputs["toolchain"]["parent_rust"]}"' in toolchain,
            "Root Rust toolchain mismatch")


def toolchain(inputs, target):
    require(platform.system() == "Darwin" and platform.machine() == "arm64",
            "Native builds require an Apple Silicon Mac")
    sdk = {"macos": "macosx", "device": "xros", "simulator": "xrsimulator"}[target]
    sdk_version = output(["xcrun", "--sdk", sdk, "--show-sdk-version"])
    require(int(sdk_version.split(".")[0]) >= 27, "SDK 27 or newer is required")
    xcode = output(["xcodebuild", "-version"])
    require(int(re.search(r"Xcode (\d+)", xcode).group(1)) >= 27, "Xcode 27 or newer is required")
    rust = inputs["toolchain"]["rust"]
    rust_target = {"macos": "aarch64-apple-darwin", "device": "aarch64-apple-visionos",
                   "simulator": "aarch64-apple-visionos-sim"}[target]
    installed = output(["rustup", "target", "list", "--installed", "--toolchain", rust])
    require(rust_target in installed.splitlines(),
            f"Install the pinned target first: rustup target add --toolchain {rust} {rust_target}")
    minimum = inputs["toolchain"]["macos_deployment" if target == "macos" else "visionos_deployment"]
    triple = {"macos": f"arm64-apple-macos{minimum}", "device": f"arm64-apple-xros{minimum}",
              "simulator": f"arm64-apple-xros{minimum}-simulator"}[target]
    flags = f"-target {triple} -isysroot {output(['xcrun', '--sdk', sdk, '--show-sdk-path'])}"
    return sdk, minimum, rust_target, flags, {"xcode": xcode, "sdk": sdk_version,
        "rust": output(["rustup", "run", rust, "rustc", "--version"]),
        "cmake": output(["cmake", "--version"]).splitlines()[0],
        "ninja": output(["ninja", "--version"])}


def dependencies(inputs, args):
    check_prepared(inputs, args)
    sdk, minimum, rust_target, flags, versions = toolchain(inputs, args.platform)
    build = args.work / "dependencies-build"
    require(not build.exists(), "Use a fresh work directory after a failed dependency build")
    build.mkdir()
    prefix = args.work / "install"
    prefix.mkdir()
    prefix_flags = f"-ffile-prefix-map={args.work}=/build/apple-native -ffile-prefix-map={Path.home()}=/build/user"
    env = dict(os.environ, RUSTUP_TOOLCHAIN=inputs["toolchain"]["rust"])
    for name in ("SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "XROS_DEPLOYMENT_TARGET",
                 "CC", "CXX", "CFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH",
                 "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "PKG_CONFIG_PATH",
                 "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC", "RUSTC_WRAPPER",
                 "RUSTC_WORKSPACE_WRAPPER", "CARGO_BUILD_TARGET", "CARGO_NET_OFFLINE"):
        env.pop(name, None)
    env["CARGO_HOME"] = str(args.work / "cargo")
    transport = args.work / "git/root/protocol/plank-transport"
    # Network acquisition is explicit; the application build below runs Cargo offline.
    run(["cargo", "fetch", "--locked", "--target", rust_target,
         "--manifest-path", transport / "Cargo.toml"], transport, env)
    opus = args.work / "source" / inputs["archives"]["opus"]["directory"]
    opus_build = build / "opus"
    cmake_args = ["cmake", "-S", opus, "-B", opus_build, "-G", "Ninja",
                  "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                  "-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_SYSROOT={sdk}",
                  f"-DCMAKE_OSX_DEPLOYMENT_TARGET={minimum}", f"-DCMAKE_C_FLAGS={prefix_flags}"]
    if args.platform != "macos":
        cmake_args += ["-DCMAKE_SYSTEM_NAME=visionOS"]
    cmake_args += [f"-D{name}=OFF" for name in ("OPUS_BUILD_SHARED_LIBRARY", "OPUS_BUILD_PROGRAMS",
                  "OPUS_BUILD_TESTING", "OPUS_DRED", "OPUS_OSCE", "OPUS_CUSTOM_MODES", "OPUS_FIXED_POINT")]
    run(cmake_args + ["-DOPUS_ENABLE_FLOAT_API=ON"], env=env)
    run(["cmake", "--build", opus_build, "--parallel", args.jobs], env=env)
    run(["cmake", "--install", opus_build], env=env)
    sodium = args.work / "source" / inputs["archives"]["sodium"]["directory"]
    sodium_build = build / "sodium"
    sodium_build.mkdir()
    sodium_env = dict(env, CC=output(["xcrun", "--sdk", sdk, "--find", "clang"]),
                      CFLAGS=f"-O2 {flags} {prefix_flags}", LDFLAGS=flags)
    sodium_args = [sodium / "configure", "--host=aarch64-apple-darwin", f"--prefix={prefix}",
                   "--disable-shared", "--enable-static"]
    if args.platform == "macos":
        sodium_args += ["--disable-asm"]
    run(sodium_args, sodium_build, sodium_env)
    run(["make", f"-j{args.jobs}"], sodium_build, sodium_env)
    run(["make", "install"], sodium_build, sodium_env)
    ffmpeg = args.work / "source" / inputs["archives"]["ffmpeg"]["directory"]
    ffmpeg_build = build / "ffmpeg"
    ffmpeg_build.mkdir()
    ffmpeg_args = [ffmpeg / "configure", f"--prefix={prefix}", "--target-os=darwin", "--arch=arm64",
                   "--cc=" + output(["xcrun", "--sdk", sdk, "--find", "clang"]),
                   "--disable-doc", "--disable-debug", f"--extra-cflags={flags} {prefix_flags}",
                   f"--extra-ldflags={flags}"]
    # FFmpeg executes generators on the build Mac. Never let a target deployment
    # environment or target SDK turn those executables into visionOS binaries.
    host_cc = output(["xcrun", "--sdk", "macosx", "--find", "clang"])
    host_flags = "-target arm64-apple-macos15.0 -isysroot " + output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"])
    ffmpeg_args += [f"--host-cc={host_cc}", f"--host-ld={host_cc}",
                   f"--host-cflags={host_flags}", f"--host-ldflags={host_flags}"]
    if args.platform == "macos":
        ffmpeg_args += ["--enable-shared", "--disable-static", "--disable-autodetect",
                       "--enable-videotoolbox", "--enable-audiotoolbox", "--enable-neon"]
    else:
        ffmpeg_args += ["--enable-cross-compile", "--sysroot=" + output(["xcrun", "--sdk", sdk, "--show-sdk-path"]),
                       "--disable-programs", "--disable-shared", "--enable-static"]
        ffmpeg_args += [f"--disable-{name}" for name in ("avdevice", "avformat", "avfilter", "network",
                       "encoders", "muxers", "demuxers", "protocols", "bsfs", "filters", "devices", "indevs", "outdevs")]
        ffmpeg_args += ["--enable-decoder=h264,hevc,av1", "--enable-parser=h264,hevc,av1",
                       "--enable-hwaccel=h264_videotoolbox,hevc_videotoolbox"]
    run(ffmpeg_args, ffmpeg_build, env)
    # FFmpeg stores configure arguments in config.h; remove builder paths there too.
    config = ffmpeg_build / "config.h"
    config.write_text(config.read_text().replace(str(args.work), "/build/apple-native")
                      .replace(str(Path.home()), "/build/user"))
    run(["make", f"-j{args.jobs}"], ffmpeg_build, env)
    run(["make", "install"], ffmpeg_build, env)
    check_prepared(inputs, args)
    (args.work / "dependencies.json").write_text(json.dumps({
        "manifest_sha256": digest(MANIFEST), "platform": args.platform,
        "recipe_sha256": digest(Path(__file__)),
        "toolchain": versions, "prefix_inventory": tree_inventory(prefix)}, indent=2) + "\n")


def application(inputs, args):
    check_prepared(inputs, args)
    sdk, minimum, _, _, versions = toolchain(inputs, args.platform)
    receipt = json.loads((args.work / "dependencies.json").read_text())
    require(receipt["manifest_sha256"] == digest(MANIFEST) and receipt["platform"] == args.platform,
            "Dependency receipt mismatch")
    require(receipt["recipe_sha256"] == digest(Path(__file__)), "Dependency build recipe changed")
    inventory = json.loads(json.dumps(tree_inventory(args.work / "install")))
    require(inventory == receipt["prefix_inventory"], "Installed dependency contents changed")
    require(receipt["toolchain"] == versions, "Dependency/application toolchain changed")
    client = args.client.resolve()
    commit = output(["git", "rev-parse", "HEAD"], client)
    verify_git(client, commit)
    common = client / "moonlight-common-c/moonlight-common-c"
    common_commit = output(["git", "rev-parse", "HEAD:moonlight-common-c/moonlight-common-c"], client)
    allowed_common = inputs["git"]["common_c"]
    require(common_commit in (allowed_common["commit"], allowed_common["integration_commit"]),
            "Client common-C gitlink is not a declared checkpoint/integration input")
    verify_git(common, common_commit)
    source = client / ("apple-native" if args.platform == "macos" else "visionos-native")
    require((source / "CMakeLists.txt").exists(), "Selected Client has no native target for this platform")
    build = args.work / "app-build"
    require(not build.exists(), "Use a fresh app build directory")
    prefix = args.work / "install"
    env = dict(os.environ, RUSTUP_TOOLCHAIN=inputs["toolchain"]["rust"], CARGO_NET_OFFLINE="true",
               CARGO_HOME=str(args.work / "cargo"), CARGO_PROFILE_RELEASE_STRIP="none")
    # SDK27's strip can make host proc-macro dylibs unloadable when targeting
    # macOS15. A profile override also covers build-host crates when --target is
    # explicit; target-only RUSTFLAGS alone does not. Match shipping Apple CI.
    for name in ("RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC", "RUSTC_WRAPPER",
                 "RUSTC_WORKSPACE_WRAPPER", "CARGO_BUILD_TARGET", "SDKROOT",
                 "CC", "CXX", "CFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH", "CPATH"):
        env.pop(name, None)
    command = ["cmake", "-S", source, "-B", build, "-G", "Xcode",
               f"-DCMAKE_OSX_SYSROOT={sdk}", "-DCMAKE_OSX_ARCHITECTURES=arm64",
               f"-DCMAKE_OSX_DEPLOYMENT_TARGET={minimum}", "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
               f"-DPLANK_TRANSPORT_DIR={args.work / 'git/root/protocol/plank-transport'}",
               f"-DPLANK_RELAY_SOURCE_DIR={args.work / 'git/raw'}",
               f"-DPLANK_RELAY_SODIUM_PREFIX={prefix}", f"-DPLANK_OPUS_DIR={prefix}",
               f"-DPLANK_FFMPEG_DIR={prefix}"]
    if args.platform == "macos":
        command += [f"-DPLANK_MANAGED_RELAY_SOURCE_DIR={args.work / 'git/managed'}",
                    f"-DPLANK_APPLE_BUILD_NUMBER={args.build_number}",
                    f"-DPLANK_BUILD_BRANCH={args.branch}"]
    else:
        command += ["-DCMAKE_SYSTEM_NAME=visionOS",
                    f"-DPLANK_VISION_BUILD_NUMBER={args.build_number}",
                    f"-DPLANK_VISION_VERSION={args.version}",
                    f"-DPLANK_VISION_BUNDLE_ID={args.bundle_id}",
                    f"-DPLANK_BUILD_BRANCH={args.branch}"]
    run(command, env=env)
    run(["cmake", "--build", build, "--config", args.configuration, "--parallel", args.jobs,
         "--", "CODE_SIGNING_ALLOWED=NO", "CODE_SIGN_IDENTITY=", "DEVELOPMENT_TEAM="], env=env)
    app = build / (args.configuration if args.platform == "macos" else f"{args.configuration}-{sdk}") / (
        "PLANK Native Pilot.app" if args.platform == "macos" else "PLANK.app")
    require(app.is_dir(), "Expected app bundle is absent")
    executable = app / "Contents/MacOS/PLANK Native Pilot" if args.platform == "macos" else app / "PLANK"
    require(executable.is_file(), "Expected app executable is absent")
    signature = subprocess.run(["codesign", "-dv", "--verbose=4", str(app)], capture_output=True, text=True)
    # Apple Silicon linkers may emit ad-hoc Mach-O signatures without using a
    # signing identity. Those are permitted; developer/distribution signing is not.
    require(signature.returncode != 0 or ("Signature=adhoc" in signature.stderr
            and "Authority=" not in signature.stderr), "Public build used a signing identity")
    require(not (app / "_CodeSignature").exists() and not (app / "Contents/_CodeSignature").exists(),
            "Public build unexpectedly has a signed resource envelope")
    (args.work / "application.json").write_text(json.dumps({
        "client_commit": commit, "common_c_commit": common_commit,
        "manifest_sha256": digest(MANIFEST), "recipe_sha256": digest(Path(__file__)), "toolchain": versions,
        "platform": args.platform, "configuration": args.configuration,
        "build_number": args.build_number, "branch": args.branch,
        "signed": False, "executable_sha256": digest(executable)}, indent=2) + "\n")
    print(f"Unsigned compile completed: {app}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("prepare", "verify", "deps", "build"))
    parser.add_argument("--platform", required=True, choices=("device", "simulator", "macos"))
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--cache", type=Path, help="Reusable checksum-verified release archives")
    parser.add_argument("--client", type=Path, default=REPOSITORY)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--build-number", default="1")
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--bundle-id", default="la.instinctual.PLANK.Vision")
    parser.add_argument("--branch", default="local")
    args = parser.parse_args()
    require(args.jobs > 0, "jobs must be positive")
    require(re.fullmatch(r"[1-9][0-9]{0,3}(\.[0-9]{1,2}){0,2}", args.build_number),
            "Build number must fit Apple's numeric version fields")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version), "Invalid marketing version")
    require(re.fullmatch(r"[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+", args.bundle_id), "Invalid bundle identifier")
    require(re.fullmatch(r"[a-z0-9][a-z0-9-]*", args.branch), "Use a lowercase kebab-case branch")
    args.work = args.work.expanduser().resolve()
    args.cache = args.cache.expanduser().resolve() if args.cache else args.work.parent / "downloads"
    inputs = json.loads(MANIFEST.read_text())
    {"prepare": prepare, "verify": check_prepared, "deps": dependencies,
     "build": application}[args.stage](inputs, args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Build stopped: {error}")
