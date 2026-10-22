"""Failure-boundary tests; no network, signing or Apple SDK required."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("native_build", Path(__file__).with_name("build.py"))
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class TargetIntegrationTests(unittest.TestCase):
    def test_ipad_build_keeps_separate_metadata_and_release_configuration(self):
        source = Path(build.__file__).read_text()
        self.assertIn('f"-DPLANK_IPAD_BUILD_NUMBER={args.build_number}"', source)
        self.assertIn('f"-DPLANK_IPAD_VERSION={args.version}"', source)
        self.assertIn('f"-DPLANK_IPAD_BUNDLE_ID={args.bundle_id}"', source)
        self.assertIn('f"-DPLANK_BUILD_BRANCH={args.branch}"', source)
        self.assertIn('f"{args.configuration}-{sdk}"', source)
        self.assertIn('CARGO_PROFILE_RELEASE_STRIP="none"', source)
        plist = (build.REPOSITORY / "ios-native/Info.plist.in").read_text()
        self.assertIn("$(PLANK_BUILD_BRANCH)", plist)
        self.assertIn("$(CURRENT_PROJECT_VERSION)", plist)

    def test_both_simulators_exclude_only_presentation_timestamp_callback(self):
        source = (build.REPOSITORY / "visionos-native/Sources/Views/PlankMetalVideoView.swift").read_text()
        self.assertIn("#if !targetEnvironment(simulator)", source)
        self.assertNotIn("#if !os(iOS) || !targetEnvironment(simulator)", source)
        self.assertIn("drawable.addPresentedHandler", source)


class SourceVerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.archive = self.work / "fixture.tar.gz"
        self.entry = {"directory": "fixture"}
        with tarfile.open(self.archive, "w:gz") as archive:
            for name, data in (("a.c", b"original\n"), ("b.c", b"untouched\n")):
                info = tarfile.TarInfo("fixture/" + name)
                info.size = len(data)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(data))
        with tarfile.open(self.archive) as archive:
            archive.extractall(self.work, filter="data")
        self.source = self.work / "fixture"

    def test_exact_patch_accepted_and_unrelated_change_rejected(self):
        data = b"patched\n"
        (self.source / "a.c").write_bytes(data)
        changed = {"a.c": hashlib.sha256(data).hexdigest()}
        build.verify_source(self.archive, self.source, self.entry, changed)
        (self.source / "b.c").write_text("unexpected change")
        with self.assertRaisesRegex(RuntimeError, "b.c"):
            build.verify_source(self.archive, self.source, self.entry, changed)

    def test_backup_and_reject_residue_rejected(self):
        for suffix in ("orig", "rej"):
            residue = self.source / ("a.c." + suffix)
            residue.write_text("residue")
            with self.assertRaisesRegex(RuntimeError, suffix):
                build.verify_source(self.archive, self.source, self.entry)
            residue.unlink()

    def test_missing_file_rejected(self):
        (self.source / "b.c").unlink()
        with self.assertRaisesRegex(RuntimeError, "b.c"):
            build.verify_source(self.archive, self.source, self.entry)

    def test_symlink_substitution_rejected(self):
        (self.source / "b.c").unlink()
        (self.source / "b.c").symlink_to("a.c")
        with self.assertRaisesRegex(RuntimeError, "b.c"):
            build.verify_source(self.archive, self.source, self.entry)

    def test_executable_bit_change_rejected(self):
        (self.source / "b.c").chmod(0o755)
        with self.assertRaisesRegex(RuntimeError, "b.c"):
            build.verify_source(self.archive, self.source, self.entry)

    def test_cached_archive_mismatch_fails_without_download(self):
        entry = {"name": self.archive.name, "sha256": "0" * 64}
        with patch.object(build, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                build.verified_archive(self.work, entry)
            run.assert_not_called()

    def test_git_wrong_commit_and_dirty_source_rejected(self):
        with patch.object(build, "output", return_value="other"):
            with self.assertRaisesRegex(RuntimeError, "commit mismatch"):
                build.verify_git(self.work, "expected")
        with patch.object(build, "output", side_effect=["expected", " M tracked.c"]):
            with self.assertRaisesRegex(RuntimeError, "dirty"):
                build.verify_git(self.work, "expected")


if __name__ == "__main__":
    unittest.main()
