import pathlib
import subprocess
import tempfile
import unittest

from scripts.release_metadata import (
    MetadataError,
    prepare_draft,
    render_draft,
    validate_release,
)


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        (self.root / "docs/releases").mkdir(parents=True)
        (self.root / "CHANGELOG.md").write_text(
            "## 0.10.0 — Playback routing\n\n- Add verified PipeWire playback routing.\n",
            encoding="utf-8",
        )
        (self.root / "docs/releases/v0.10.0.md").write_text(
            "# LinuxAPO v0.10.0 Beta | PipeWire playback routing\n\n"
            "## Purpose\n\nProcess application playback through PipeWire.\n\n"
            "## Verified test results\n\nCTest passed 12/12 on the tagged commit.\n\n"
            "## Known issues and limitations\n\nHardware diversity remains limited.\n",
            encoding="utf-8",
        )

    def tearDown(self):
        self.temp.cleanup()

    def test_valid_version_specific_notes(self):
        title = validate_release(self.root, "v0.10.0")
        self.assertEqual(title, "LinuxAPO v0.10.0 Beta | PipeWire playback routing")

    def test_rejects_missing_notes(self):
        with self.assertRaisesRegex(MetadataError, "missing committed"):
            validate_release(self.root, "v0.10.1")

    def test_rejects_empty_changelog_entry(self):
        (self.root / "CHANGELOG.md").write_text("## 0.10.0 — Playback routing\n\n", encoding="utf-8")
        with self.assertRaisesRegex(MetadataError, "at least one release change"):
            validate_release(self.root, "v0.10.0")

    def test_rejects_placeholder_changelog_entry(self):
        (self.root / "CHANGELOG.md").write_text(
            "## 0.10.0 — Playback routing\n\n- TODO: add release changes.\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(MetadataError, "placeholder"):
            validate_release(self.root, "v0.10.0")

    def test_rejects_reused_generic_title_topic(self):
        (self.root / "docs/releases/v0.9.9.md").write_text(
            "# LinuxAPO v0.9.9 Beta | PipeWire playback routing\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(MetadataError, "reused"):
            validate_release(self.root, "v0.10.0")

    def test_rejects_unfinished_notes(self):
        notes = self.root / "docs/releases/v0.10.0.md"
        notes.write_text(notes.read_text(encoding="utf-8") + "\nTODO: add tests\n", encoding="utf-8")
        with self.assertRaisesRegex(MetadataError, "placeholder"):
            validate_release(self.root, "v0.10.0")

    def test_draft_contains_history_and_changed_paths(self):
        draft = render_draft(
            "0.10.0", "v0.9.0", "PipeWire playback routing",
            [("abc1234", "feat(pipewire): add playback graph")],
            ["src/pipewire/Playback.cpp"],
        )
        self.assertIn("LinuxAPO v0.10.0 Beta | PipeWire playback routing", draft)
        self.assertIn("`abc1234` feat(pipewire): add playback graph", draft)
        self.assertIn("`src/pipewire/Playback.cpp`", draft)
        self.assertIn("## Verified test results", draft)

    def test_draft_command_reads_git_history_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "config", "user.name", "Release Test"], cwd=root, check=True)
            subprocess.run(
                ["git", "config", "user.email", "release-test@example.invalid"],
                cwd=root,
                check=True,
            )
            tracked = root / "README.md"
            tracked.write_text("initial\n", encoding="utf-8")
            subprocess.run(["git", "add", "README.md"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "baseline"], cwd=root, check=True)
            subprocess.run(["git", "tag", "-am", "baseline", "v0.9.0"], cwd=root, check=True)
            tracked.write_text("changed\n", encoding="utf-8")
            subprocess.run(["git", "add", "README.md"], cwd=root, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "fix: document a real change"], cwd=root, check=True
            )

            notes = prepare_draft(root, "0.10.0", "Verified desktop routing", "v0.9.0")
            content = notes.read_text(encoding="utf-8")
            self.assertIn("fix: document a real change", content)
            self.assertIn("`README.md`", content)
            with self.assertRaisesRegex(MetadataError, "refusing to overwrite"):
                prepare_draft(root, "0.10.0", "Verified desktop routing", "v0.9.0")

            with self.assertRaisesRegex(MetadataError, "must be newer"):
                prepare_draft(root, "0.9.0", "Older release", "v0.9.0")

            subprocess.run(["git", "tag", "-am", "already released", "v0.10.0"], cwd=root, check=True)
            with self.assertRaisesRegex(MetadataError, "existing tag"):
                prepare_draft(root, "0.10.0", "Verified desktop routing", "v0.9.0")


if __name__ == "__main__":
    unittest.main()
