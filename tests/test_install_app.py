#!/usr/bin/env python3
"""GUI installation contract: temporary homes, no system services or hardware."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class InstallAppTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='n14-install-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / 'project'
        self.home = self.root / 'home'
        self.home.mkdir()
        self.scratch = self.root / 'tmp'
        self.scratch.mkdir()
        self.installer = self.project / 'scripts/install-app.sh'
        self.installer.parent.mkdir(parents=True)
        shutil.copy2(ROOT / 'scripts/install-app.sh', self.installer)
        shutil.copytree(ROOT / 'resources', self.project / 'resources')
        self.binary = self.project / 'build/app/n14-control'
        self.binary.parent.mkdir(parents=True)
        self.binary.write_text('#!/bin/sh\nprintf "fixture GUI\\n"\n')
        self.binary.chmod(0o755)
        self.env = {**os.environ, 'HOME': str(self.home), 'TMPDIR': str(self.scratch)}

    def run_installer(self, *arguments):
        return subprocess.run(['sh', str(self.installer), *arguments],
                              env=self.env, text=True, capture_output=True, timeout=15)

    def assert_installed(self):
        destination = self.home / '.local'
        binary = destination / 'bin/n14-control'
        desktop = destination / 'share/applications/n14-control.desktop'
        icon = destination / 'share/icons/hicolor/scalable/apps/n14-control.svg'
        self.assertEqual(binary.read_bytes(), self.binary.read_bytes())
        self.assertTrue(os.access(binary, os.X_OK))
        self.assertEqual(icon.read_bytes(), (ROOT / 'resources/n14-control.svg').read_bytes())
        self.assertIn(f'Exec="{binary}"', desktop.read_text())
        self.assertIn('Icon=n14-control\n', desktop.read_text())
        subprocess.run(['desktop-file-validate', str(desktop)], check=True,
                       text=True, capture_output=True, timeout=10)
        self.assertFalse(list(self.scratch.iterdir()), 'Installer leaked a temporary file')

    def test_user_install_creates_binary_launcher_and_icon(self):
        result = self.run_installer('--user')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installed()

    def test_installation_does_not_depend_on_working_directory(self):
        result = subprocess.run(['sh', str(self.installer), '--user'], cwd='/',
                                env=self.env, text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installed()

    def test_reinstall_updates_existing_binary(self):
        self.assertEqual(self.run_installer('--user').returncode, 0)
        self.binary.write_text('#!/bin/sh\nprintf "updated fixture\\n"\n')
        result = self.run_installer('--user')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installed()

    def test_spaces_and_sed_metacharacters_in_home(self):
        self.home = self.root / 'Home & Work | profile'
        self.home.mkdir()
        self.env['HOME'] = str(self.home)
        result = self.run_installer('--user')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installed()

    def test_bundle_source_layout_installs_without_build_tree(self):
        bundle = self.root / 'bundle'
        (bundle / 'source/scripts').mkdir(parents=True)
        (bundle / 'bin').mkdir()
        shutil.copy2(self.installer, bundle / 'source/scripts/install-app.sh')
        shutil.copytree(self.project / 'resources', bundle / 'source/resources')
        shutil.copy2(self.binary, bundle / 'bin/n14-control')
        self.installer = bundle / 'source/scripts/install-app.sh'
        result = self.run_installer('--user')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installed()

    def test_missing_binary_installs_nothing(self):
        self.binary.unlink()
        result = self.run_installer('--user')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.home / '.local').exists())

    def test_invalid_arguments_install_nothing(self):
        result = self.run_installer('--unknown')
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.home / '.local').exists())

    @unittest.skipIf(os.getuid() == 0, 'Refusal applies only to an unprivileged installer')
    def test_system_install_requires_root(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 2)
        self.assertIn('requires root', result.stderr)
        self.assertFalse((self.home / '.local').exists())

    def test_version_and_system_desktop_match_release(self):
        bundle = (ROOT / 'scripts/bundle.py').read_text()
        version = re.search(r"^VERSION = '([^']+)'", bundle, re.MULTILINE).group(1)
        self.assertIn(f'setApplicationVersion("{version}")', (ROOT / 'src/main.cpp').read_text())
        template = (ROOT / 'resources/n14-control.desktop.in').read_text()
        self.assertEqual(template.replace('@BINDIR@', '/usr/local/bin'),
                         (ROOT / 'resources/n14-control.desktop').read_text())


if __name__ == '__main__':
    unittest.main()