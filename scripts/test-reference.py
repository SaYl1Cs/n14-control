#!/usr/bin/env python3
"""Run the unmodified, pinned reference tests; never use the source working tree.

This is a build-time oracle, NOT a runtime cooling controller and NOT proof that
the native port is equivalent. No source checkout, network, install or EC writes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REPOSITORY = ROOT.parent / "colorful-n14-coolercontrol"


def git(repository, *args):
    return subprocess.check_output(
        ["/usr/bin/git", "--no-replace-objects", "-C", str(repository), *args],
        env={**os.environ, "GIT_NO_REPLACE_OBJECTS": "1", "GIT_TERMINAL_PROMPT": "0"},
    )


def materialize(repository, lock, destination):
    source = lock["integration"]
    commit = source["commit"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Invalid pinned commit")
    actual = git(repository, "rev-parse", "--verify", commit + "^{commit}").decode().strip()
    tree = git(repository, "rev-parse", "--verify", commit + "^{tree}").decode().strip()
    if actual != commit or tree != source["tree"]:
        raise ValueError("Reference commit/tree mismatch; no tests executed")
    entries = git(repository, "ls-tree", "-rz", "--full-tree", commit).split(b"\0")
    manifest = {}
    for entry in filter(None, entries):
        metadata, raw_path = entry.split(b"\t", 1)
        mode, kind, oid = metadata.decode("ascii").split()
        path = PurePosixPath(raw_path.decode("utf-8"))
        if kind != "blob" or mode not in ("100644", "100755"):
            raise ValueError("Unsupported reference entry: " + str(path))
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("Invalid reference path")
        data = git(repository, "cat-file", "blob", oid)
        # Check every extracted blob, not just a mutable ref name or working file.
        actual_oid = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if actual_oid != oid:
            raise ValueError("Reference blob mismatch: " + str(path))
        target = destination.joinpath(*path.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        # Source scripts are not executed; only explicitly selected test modules run.
        target.chmod(0o400)
        manifest[str(path)] = {"git_blob": oid, "sha256": hashlib.sha256(data).hexdigest()}
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=DEFAULT_REPOSITORY)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Reference tests must run as an ordinary user, never root")
    lock = json.loads((ROOT / "reference/source-lock.json").read_text())
    if lock["schema"] != 1:
        raise ValueError("Unknown source lock schema")
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    report = build / "reference-verification.json"
    # A failed rerun cannot leave an earlier successful receipt.
    report.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="reference-", dir=build) as directory:
        source = Path(directory)
        manifest = materialize(args.repository, lock, source)
        print("Reference:", lock["integration"]["repository"], flush=True)
        print("Commit:", lock["integration"]["commit"], flush=True)
        print("Verified immutable blobs:", len(manifest), flush=True)
        code = 0
        if not args.verify_only:
            modules = lock["reference_test_modules"]
            if not modules or any(not re.fullmatch(r"test_[a-z0-9_]+", item) for item in modules):
                raise ValueError("Invalid reference test module list")
            # Avoid importing user modules or contacting a real desktop bus.
            home = source / "test-home"
            home.mkdir()
            env = {
                "PATH": "/usr/bin:/bin", "HOME": str(home), "LANG": "C.UTF-8",
                "PYTHONPATH": str(source) + os.pathsep + str(source / "tests"),
                "PYTHONDONTWRITEBYTECODE": "1", "QT_QPA_PLATFORM": "offscreen",
                "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent-n14-reference-bus",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent-n14-reference-bus",
            }
            code = subprocess.run(
                [sys.executable, "-B", "-s", "-m", "unittest", "-v", *modules],
                cwd=source, env=env, check=False, timeout=180,
            ).returncode
        for path, expected in manifest.items():
            if hashlib.sha256((source / path).read_bytes()).hexdigest() != expected["sha256"]:
                raise ValueError("Reference file changed during validation: " + path)
        result = {
            "integration": lock["integration"], "files": manifest,
            "tests_executed": not args.verify_only, "test_exit_code": code,
            "native_equivalence_proven": False,
        }
        report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("Reference validation failed:", error, file=sys.stderr)
        sys.exit(1)