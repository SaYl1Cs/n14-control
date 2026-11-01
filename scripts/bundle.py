#!/usr/bin/env python3
"""Build-time portable bundle. Requires no third-party Python packages.

Libraries stay dynamically replaceable; host libc and graphics drivers are not
bundled. This is a target-system archive, not a universal Linux binary.
"""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.2.1'
DEST = ROOT / 'dist' / f'n14-control-{VERSION}'
binary = ROOT / 'build/app/n14-control'
if not binary.is_file():
    raise SystemExit('Run scripts/build.sh first')
if DEST.is_symlink():
    raise SystemExit('Refusing a symlink bundle directory')
if DEST.exists():
    shutil.rmtree(DEST)
for name in ('bin', 'lib', 'plugins', 'licenses', 'source', 'share/applications',
             'share/icons/hicolor/scalable/apps'):
    (DEST / name).mkdir(parents=True)
shutil.copy2(binary, DEST / 'bin/n14-control')
shutil.copy2(ROOT / 'resources/n14-control.desktop', DEST / 'share/applications/n14-control.desktop')
shutil.copy2(ROOT / 'resources/n14-control.svg', DEST / 'share/icons/hicolor/scalable/apps/n14-control.svg')
plugins = Path(subprocess.check_output(['qmake6', '-query', 'QT_INSTALL_PLUGINS'], text=True).strip())
queue = [DEST / 'bin/n14-control']
for group, selected in {
    'platforms': ['libqwayland.so', 'libqxcb.so', 'libqoffscreen.so'],
    'iconengines': ['libqsvgicon.so'],
    'imageformats': ['libqsvg.so', 'libqjpeg.so'],
    'wayland-shell-integration': None,
    'wayland-decoration-client': None,
    'wayland-graphics-integration-client': None,
}.items():
    directory = plugins / group
    for file in sorted(directory.glob('*.so')):
        if selected is not None and file.name not in selected:
            continue
        target = DEST / 'plugins' / group / file.name
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(file, target)
        queue.append(target)

skip = re.compile(r'^(?:ld-linux.*|lib(?:c|m|dl|pthread|rt|resolv|util|anl)\.so(?:\..*)?|libnss_.*|libnvidia.*|libcuda.*)$')
seen = set()
originals = set()
while queue:
    file = queue.pop()
    output = subprocess.check_output(['ldd', str(file)], text=True)
    if 'not found' in output:
        raise SystemExit(f'Unresolved dependencies for {file}:\n{output}')
    for line in output.splitlines():
        match = re.search(r'=> (/\S+)', line)
        if not match:
            continue
        source = Path(match.group(1))
        name = source.name
        if name in seen or skip.match(name):
            continue
        seen.add(name)
        originals.add(source)
        target = DEST / 'lib' / name
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | 0o100)
        queue.append(target)

(DEST / 'bin/qt.conf').write_text('[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\n')
(DEST / 'AppRun').write_text('''#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"
exec "$HERE/bin/n14-control" "$@"
''')
(DEST / 'AppRun').chmod(0o755)
(DEST / 'install.sh').write_text('''#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$HERE/source/scripts/install-app.sh" "$@"
''')
(DEST / 'install.sh').chmod(0o755)
for file in ('README.md', 'VALIDATION.md', 'REQUIREMENTS.md'):
    shutil.copy2(ROOT / file, DEST / file)
for directory in ('src', 'tests', 'resources', 'scripts', 'service', 'reference'):
    shutil.copytree(ROOT / directory, DEST / 'source' / directory, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
for file in ('n14-control.pro', 'AGENTS.md'):
    shutil.copy2(ROOT / file, DEST / 'source' / file)

# Retain installed license notices for the bundled dependencies, plus common licenses.
packages = set()
for file in originals:
    found = subprocess.run(['pacman', '-Qqo', str(file)], text=True, capture_output=True)
    if found.returncode == 0:
        packages.update(found.stdout.splitlines())
for package in sorted(packages):
    directory = Path('/usr/share/licenses') / package
    if directory.is_dir():
        shutil.copytree(directory, DEST / 'licenses' / package, symlinks=False)
common = Path('/usr/share/licenses/common')
if common.is_dir():
    shutil.copytree(common, DEST / 'licenses/common', symlinks=False)
build_info = subprocess.check_output(['uname', '-srmo'], text=True)
build_info += '\nQt: ' + subprocess.check_output(['qmake6', '-query', 'QT_VERSION'], text=True)
build_info += '\nBundled package versions:\n'
for package in sorted(packages):
    build_info += subprocess.check_output(['pacman', '-Q', package], text=True)
build_info += '''
Host glibc, graphics drivers, power-profiles-daemon and KDE KScreen are required.
No universal compatibility with older glibc distributions is claimed.
Qt libraries/plugins are unmodified shared libraries and can be replaced in lib/.
Upstream sources for exact Arch/CachyOS package builds are available through their
package source repositories and Qt's official source archives (qt.io).
This local development archive has not been cleared for public redistribution;
review licenses and corresponding-source obligations before publishing it.
'''
(DEST / 'BUILD-INFO.txt').write_text(build_info)
manifest = []
for file in sorted(DEST.rglob('*')):
    if file.is_file():
        manifest.append(hashlib.sha256(file.read_bytes()).hexdigest() + '  ' + str(file.relative_to(DEST)))
(DEST / 'SHA256SUMS').write_text('\n'.join(manifest)+'\n')
archive = ROOT / 'dist' / (DEST.name + '-linux-x86_64.tar.gz')
with tarfile.open(archive, 'w:gz', compresslevel=5) as tar:
    tar.add(DEST, arcname=DEST.name)
(archive.with_name(archive.name + '.sha256')).write_text(
    hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n'
)
print(f'Bundle: {DEST / "AppRun"}')
print(f'Archive: {archive} ({archive.stat().st_size / 1024**2:.1f} MiB)')