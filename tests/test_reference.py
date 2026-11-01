"""Source-lock reader tests: private temporary git databases, never hardware."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("n14_reference", ROOT / "scripts/test-reference.py")
REFERENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REFERENCE)


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repo"
        self.repository.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Reference test")
        self.git("config", "user.email", "test@invalid.example")
        self.git("config", "commit.gpgsign", "false")
        self.source = self.repository / "module.py"
        self.source.write_text("ORIGINAL = 1\n")
        self.git("add", "module.py")
        self.git("commit", "-qm", "Pinned original")
        self.commit = self.git("rev-parse", "HEAD").strip()
        self.lock = {"integration": {"commit": self.commit, "tree": self.git("rev-parse", "HEAD^{tree}").strip()}}
        self.destination = self.root / "extracted"
        self.destination.mkdir()

    def git(self, *args):
        return subprocess.check_output(["/usr/bin/git", "-C", str(self.repository), *args], stderr=subprocess.PIPE).decode()

    def test_extracts_content_addressed_original(self):
        manifest = REFERENCE.materialize(self.repository, self.lock, self.destination)
        content = (self.destination / "module.py").read_bytes()
        self.assertEqual(content, b"ORIGINAL = 1\n")
        self.assertEqual(manifest["module.py"]["sha256"], hashlib.sha256(content).hexdigest())
        self.assertEqual(manifest["module.py"]["git_blob"], self.git("rev-parse", "HEAD:module.py").strip())
        self.assertFalse((self.destination / "module.py").stat().st_mode & 0o222)

    def test_ignores_uncommitted_and_untracked_files(self):
        self.source.write_text("MODIFIED = True\n")
        (self.repository / "unexpected.py").write_text("LOCAL = True\n")
        manifest = REFERENCE.materialize(self.repository, self.lock, self.destination)
        self.assertEqual(set(manifest), {"module.py"})
        self.assertEqual((self.destination / "module.py").read_text(), "ORIGINAL = 1\n")

    def test_ignores_unpublished_head_commit(self):
        self.source.write_text("UNPUBLISHED = True\n")
        self.git("add", "module.py")
        self.git("commit", "-qm", "Local-only change")
        REFERENCE.materialize(self.repository, self.lock, self.destination)
        self.assertEqual((self.destination / "module.py").read_text(), "ORIGINAL = 1\n")
        self.assertNotEqual(self.git("rev-parse", "HEAD").strip(), self.commit)

    def test_rejects_wrong_tree_before_extraction(self):
        self.lock["integration"]["tree"] = "0" * 40
        with self.assertRaisesRegex(ValueError, "mismatch"):
            REFERENCE.materialize(self.repository, self.lock, self.destination)
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_rejects_mutable_branch_instead_of_commit(self):
        self.lock["integration"]["commit"] = "HEAD"
        with self.assertRaisesRegex(ValueError, "Invalid pinned commit"):
            REFERENCE.materialize(self.repository, self.lock, self.destination)

    def test_ignores_git_replace_objects(self):
        self.source.write_text("REPLACEMENT = True\n")
        self.git("add", "module.py")
        self.git("commit", "-qm", "Replacement object")
        self.git("replace", self.commit, "HEAD")
        REFERENCE.materialize(self.repository, self.lock, self.destination)
        self.assertEqual((self.destination / "module.py").read_text(), "ORIGINAL = 1\n")

    def test_rejects_symlink_blobs(self):
        (self.repository / "link.py").symlink_to("/etc/passwd")
        self.git("add", "link.py")
        self.git("commit", "-qm", "Symlink is not a reference module")
        lock = {"integration": {"commit": self.git("rev-parse", "HEAD").strip(), "tree": self.git("rev-parse", "HEAD^{tree}").strip()}}
        with self.assertRaisesRegex(ValueError, "Unsupported reference entry"):
            REFERENCE.materialize(self.repository, lock, self.destination)


if __name__ == "__main__":
    unittest.main()