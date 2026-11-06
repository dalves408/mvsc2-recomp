"""The RecompLauncher manifest (recomplauncher.json) for the Marvel vs. Capcom 2 Windows release, and the SDK ZIP that carries it.

    python scripts/recomplauncher_manifest.py --version 1.0.0 [--build build_release/Release] [--out installer/Output]

writes MvC2-v<version>-Windows-x64.zip (player, importer, settings program, recomplauncher.json) and its .sha256.
Level 3 of the RecompLauncher guide: prepare (headless mvc2_import --prepare) and game.dumps (Redump).
The Inno Setup installer stays the choice for players who do not use the launcher.
"""
import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

# Redump, Microsoft Xbox, Marvel vs. Capcom 2, serial CC-007 (http://redump.org/disc/54506/): the image this build was made
# from, checked by CRC-32. Discs 21820 and 33860 have the same serial and size but another CRC-32; their default.xbe has
# not been checked, so they are not listed.
DUMPS = [{'name': 'Marvel vs. Capcom 2: New Age of Heroes', 'size': 7825162240, 'crc32': '8caa7541'}]

DATA = '{local}/MvC2Recomp'     # %LOCALAPPDATA%\MvC2Recomp: settings.ini and SaveData (the game's own choice)


def manifest():
    def setting(identifier, label, key, choices, default, help_text=None):
        row = {
            'id': identifier, 'label': label,
            'choices': [{'label': name, 'set': {key: str(value)}} for name, value in choices],
            'defaults': {key: str(default)},
        }
        if help_text:
            row['help'] = help_text
        return row

    toggle = [('Off', 0), ('On', 1)]
    settings = [
        setting('scale', 'Render scale', 'display/resolution_scale', [(f'{n}x', n) for n in range(1, 6)], 1,
                '1x is the original 640x480; higher draws the game sharper but needs a faster graphics card.'),
        setting('upscaling', 'Upscaling', 'display/upscaling', toggle, 0,
                'Sharper resampling and sharpening when the picture is shown larger than it is drawn.'),
        setting('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 1,
                'Big Picture mode always starts fullscreen.'),
        setting('window-size', 'Window size', 'display/window_scale',
                [('1x 640x480', 1), ('2x 1280x960', 2), ('3x 1920x1440', 3), ('4x 2560x1920', 4)], 2,
                'A size too large for your screen is reduced to fit.'),
        setting('stretch', 'Stretch image', 'display/stretch', [('Off (4:3)', 0), ('On (fills window)', 1)], 0),
        setting('vsync', 'VSync', 'display/vsync', toggle, 1),
        setting('hires-textures', 'Hi-res textures', 'display/hires_textures', toggle, 1,
                'Only has an effect when a texture pack is installed.'),
    ]
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'Marvel vs. Capcom 2',
        'repo': 'dalves408/mvsc2-recomp',
        'game': {
            'kind': 'iso_or_folder',
            'label': 'Your Marvel vs. Capcom 2 Xbox disc image (serial CC-007) or the folder with its files (the one that holds default.xbe).',
            'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'},
            'dumps': DUMPS,
        },
        'start': {'program': ['mvc2_recomp.exe']},
        'prepare': {
            'program': ['mvc2_import.exe'],
            'args': ['--prepare', '{game}', '{install}/game_files'],
            'label': 'Game files from your disc',
            'timeout': 900,
        },
        'config': [{
            'format': 'ini',
            'file': [{'path': DATA + '/settings.ini'}],
            'new': {key: value for row in settings for key, value in row['defaults'].items()},
        }],
        'settings': settings,
        'saves': [DATA + '/SaveData'],
        'exit_codes': {
            '2': 'The disc image or folder could not be found.',
            '3': 'That file is not an Xbox disc image.',
            '4': 'That is not the supported version of Marvel vs. Capcom 2.',
            '5': 'The disc image is damaged or incomplete.',
            '6': 'The game files could not be written to the install folder.',
        },
        'looks': {'graphics': 'd3d11', 'lines': 480},
    }


def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True, help='release version, e.g. 1.0.0')
    parser.add_argument('--build', type=Path, default=here / 'build_release' / 'Release')
    parser.add_argument('--out', type=Path, default=here / 'installer' / 'Output')
    parser.add_argument('--json-only', action='store_true', help='print recomplauncher.json and exit')
    args = parser.parse_args()
    text = json.dumps(manifest(), indent=2) + '\n'
    if args.json_only:
        sys.stdout.write(text)
        return
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / f'MvC2-v{args.version}-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        for name in ('mvc2_recomp.exe', 'mvc2_import.exe', 'config.exe'):
            package.write(args.build / name, name)
        package.writestr('recomplauncher.json', text)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'RecompLauncher ZIP: {archive}\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
