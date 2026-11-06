"""Builds the Marvel vs. Capcom 2 player release: the exe, importer and settings program, then the Setup and the launcher ZIP.

    python scripts/build_installer.py --version 1.0.0

Output in installer/Output: MvC2-Setup-<version>.exe, MvC2-v<version>-Windows-x64.zip, and a .sha256 for each.
Both contain code translated from the game, so they stay out of git.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build_release'
INSTALLER = ROOT / 'installer'
OUTPUT = INSTALLER / 'Output'
ISCC = Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'
WIZARD_SIZES = (55, 64, 80, 96, 128)


def run(*command):
    print('>', ' '.join(str(c) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, cwd=ROOT)


def set_rc_version(path, version):
    """Keep a .rc file's version resource equal to the release number."""
    parts = version.split('.')
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit('version must look like 1.0.0')
    commas, dots = ','.join(parts + ['0']), '.'.join(parts + ['0'])
    text = path.read_text(encoding='utf-8')
    new = re.sub(r'(FILEVERSION|PRODUCTVERSION) [\d,]+', rf'\1 {commas}', text)
    new = re.sub(r'("(?:FileVersion|ProductVersion)", ")[\d.]+', rf'\g<1>{dots}', new)
    if new != text:
        path.write_text(new, encoding='utf-8')


def wizard_images():
    """The picture at the top right of every wizard page is the game's icon, one file per screen scaling."""
    icon = Image.open(ROOT / 'resources' / 'mvc2.png').convert('RGBA')
    for size in WIZARD_SIZES:
        canvas = Image.new('RGBA', (size, size), (255, 255, 255, 255))
        small = icon.resize((size, size), Image.LANCZOS)
        canvas.alpha_composite(small)
        canvas.convert('RGB').save(INSTALLER / f'wizard_small_{size}.bmp')


def write_sha256(path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + '.sha256').write_text(f'{digest}  {path.name}\n')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True)
    parser.add_argument('--skip-build', action='store_true', help='use the programs already in build_release')
    args = parser.parse_args()

    for rc in ('mvc2.rc', 'config.rc'):
        set_rc_version(ROOT / 'resources' / rc, args.version)
    if not args.skip_build:
        run('cmake', '-S', ROOT, '-B', BUILD, '-DMVC2_RELEASE=ON')
        run('cmake', '--build', BUILD, '--config', 'Release', '-j', '12', '--target', 'mvc2_recomp', 'mvc2_import', 'mvc2_config')
    wizard_images()
    OUTPUT.mkdir(exist_ok=True)
    run(ISCC, f'/DAppVersion={args.version}', INSTALLER / 'mvc2.iss')
    setup = OUTPUT / f'MvC2-Setup-{args.version}.exe'
    print(f'Setup: {setup}\nSHA-256: {write_sha256(setup)}')
    run(sys.executable, ROOT / 'scripts' / 'recomplauncher_manifest.py', '--version', args.version)


if __name__ == '__main__':
    main()
