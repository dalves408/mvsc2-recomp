"""Builds the Marvel vs. Capcom 2 player release: the exe, importer and settings program, then the Setup and the launcher ZIP.

    python scripts/build_installer.py --version 1.0.0

Output in installer/Output: MvC2-Setup-<version>.exe, MvC2-v<version>-Windows-x64.zip and
MvC2-HiRes-Textures-<revision>-Setup.exe, each with a .sha256, and the texture pack's texture-*.bin parts.
Both contain code translated from the game, so they stay out of git.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build_release'
INSTALLER = ROOT / 'installer'
OUTPUT = INSTALLER / 'Output'
WIZARD_SIZES = (55, 64, 80, 96, 128)


def find_iscc():
    """Inno Setup 6's compiler: --iscc or ISCC_EXE if given, else a per-user install (the default for a non-admin
    install), else a machine-wide one, else ISCC on the PATH."""
    places = [os.environ.get('ISCC_EXE', '')]
    if os.environ.get('LOCALAPPDATA'):
        places.append(str(Path(os.environ['LOCALAPPDATA']) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'))
    places.append(str(Path.home() / 'AppData' / 'Local' / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'))
    for env in ('ProgramFiles(x86)', 'ProgramFiles'):
        if os.environ.get(env):
            places.append(str(Path(os.environ[env]) / 'Inno Setup 6' / 'ISCC.exe'))
    places.append(shutil.which('ISCC') or '')
    for place in places:
        if place and Path(place).is_file():
            return Path(place)
    sys.exit('Inno Setup 6 (ISCC.exe) was not found. Install Inno Setup 6.4 or newer, or pass --iscc <path to ISCC.exe> '
             '(or set ISCC_EXE). Looked in:\n  ' + '\n  '.join(p for p in places if p))


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
    parser.add_argument('--iscc', help="Inno Setup 6's ISCC.exe, if it is not found by itself")
    args = parser.parse_args()
    if args.iscc:
        os.environ['ISCC_EXE'] = args.iscc
    ISCC = find_iscc()
    print(f'Inno Setup: {ISCC}', flush=True)

    for rc in ('mvc2.rc', 'config.rc'):
        set_rc_version(ROOT / 'resources' / rc, args.version)
    if not args.skip_build:
        run('cmake', '-S', ROOT, '-B', BUILD, '-DMVC2_RELEASE=ON')
        run('cmake', '--build', BUILD, '--config', 'Release', '-j', '12', '--target', 'mvc2_recomp', 'mvc2_import', 'mvc2_config',
            'mvc2_texture_download')
    wizard_images()
    OUTPUT.mkdir(exist_ok=True)
    run(ISCC, f'/DAppVersion={args.version}', INSTALLER / 'mvc2.iss')
    setup = OUTPUT / f'MvC2-Setup-{args.version}.exe'
    print(f'Setup: {setup}\nSHA-256: {write_sha256(setup)}')
    run(sys.executable, ROOT / 'scripts' / 'recomplauncher_manifest.py', '--version', args.version)

    # The hi-res texture addon and the pack's .bin parts (made by scripts/build_texture_pack.py), so a copy of
    # this folder installs offline: both Setups use the parts beside them before downloading.
    payload = (INSTALLER / 'texture_payload.iss').read_text(encoding='ascii')
    revision = re.search(r'#define TextureVersion "(\d+)"', payload).group(1)
    run(ISCC, INSTALLER / 'mvc2_textures.iss')
    addon = OUTPUT / f'MvC2-HiRes-Textures-{revision}-Setup.exe'
    print(f'Texture addon: {addon}')
    print(f'SHA-256: {write_sha256(addon)}')
    parts = sorted((ROOT / 'out' / 'texture-release' / f'revision-{revision}').glob('texture-*.bin'))
    if not parts:
        sys.exit(f'no texture-*.bin for revision {revision}: run scripts/build_texture_pack.py first')
    for part in parts:
        shutil.copyfile(part, OUTPUT / part.name)
        print(f'{part.name}: {part.stat().st_size:,} bytes')


if __name__ == '__main__':
    main()
