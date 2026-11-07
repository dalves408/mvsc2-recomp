"""Packs the hi-res texture pack into the .bin parts the installers download.

    python scripts/build_texture_pack.py --source <folder of .dds files> --revision 1

Each part is an ordinary 7z archive (LZMA2, maximum compression) with a texture_pack/ root and a
.bin extension, and is under 2,000,000,000 bytes so GitHub serves it as a release asset. The parts
extract into the same folder in order; the last one carries manifest.txt, which lists every texture
as "<sha256> <bytes> <path>" and is what Setup checks the finished folder against.

Output in out/texture-release/revision-<n>: texture-01.bin [, texture-02.bin ...], SHA256SUMS.txt,
texture-payload.json, and installer/texture_payload.iss with the pins both installers compile in.
The pack is made from the game's own textures, so the parts stay out of git.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEVENZIP = Path(r'C:\Program Files\7-Zip\7z.exe')
REPO = 'dalves408/mvsc2-recomp'
PART_LIMIT = 2_000_000_000          # GitHub's per-asset ceiling is 2 GiB; stay under 2 GB
PACK_GUID = '5C8E2F41-9B37-4D6A-A1E0-7F3B8C42D915'
MAX_PARTS = 4                       # what texture_download_code.iss is written for
SEVENZIP_ARGS = ['-t7z', '-m0=LZMA2', '-mx=9', '-md=256m', '-mfb=273', '-ms=on', '-mmt=8',
                 '-mtc=off', '-mta=off', '-mtm=off', '-bsp0', '-bso0']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def pack(stage, files, out):
    """7z the listed files (paths relative to stage, under texture_pack/) into out."""
    if out.exists():
        out.unlink()
    listfile = out.with_suffix('.list')
    listfile.write_text('\n'.join(files) + '\n', encoding='utf-8')
    tmp = out.with_suffix('.7z')
    if tmp.exists():
        tmp.unlink()
    subprocess.run([str(SEVENZIP), 'a', *SEVENZIP_ARGS, str(tmp), f'@{listfile}'], cwd=stage, check=True)
    tmp.rename(out)
    listfile.unlink()
    return out.stat().st_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', default=str(ROOT / 'out' / 'texture-pack' / 'texture_pack_dds'))
    ap.add_argument('--revision', type=int, default=1)
    args = ap.parse_args()
    src = Path(args.source)
    textures = sorted(p for p in src.rglob('*') if p.is_file() and p.suffix.lower() in ('.dds', '.png'))
    if not textures:
        sys.exit(f'no .dds or .png files in {src}')
    out = ROOT / 'out' / 'texture-release' / f'revision-{args.revision}'
    stage = out / 'stage'
    if stage.exists():
        shutil.rmtree(stage)
    pack_root = stage / 'texture_pack'

    entries = []
    for p in textures:
        rel = p.relative_to(src).as_posix()
        link_or_copy(p, pack_root / rel)
        entries.append((sha256(p), p.stat().st_size, rel))
    total = sum(e[1] for e in entries)
    manifest = ''.join(f'{h} {n} {rel}\n' for h, n, rel in entries)
    (pack_root / 'manifest.txt').write_text(manifest, encoding='ascii', newline='\n')
    (pack_root / 'README.txt').write_text(
        f'Marvel vs. Capcom 2 hi-res texture pack, revision {args.revision}.\n'
        f'{len(entries)} textures. Turn on Hi-res textures in the game (F1) or in config.exe.\n'
        'Made for the PC recompilation at https://github.com/' + REPO + '\n',
        encoding='ascii', newline='\n')
    print(f'{len(entries)} textures, {total:,} bytes', flush=True)

    # One part if it fits; otherwise split the texture list by size into as few parts as fit.
    names = [f'texture_pack/{rel}' for _, _, rel in entries]
    sizes = [n for _, n, _ in entries]
    extras = ['texture_pack/manifest.txt', 'texture_pack/README.txt']
    for count in range(1, MAX_PARTS + 1):
        groups, budget, cur, acc = [], total / count, [], 0
        for name, n in zip(names, sizes):
            if cur and acc + n > budget and len(groups) < count - 1:
                groups.append(cur)
                cur, acc = [], 0
            cur.append(name)
            acc += n
        groups.append(cur)
        groups[-1] = groups[-1] + extras
        for old in out.glob('texture-*.bin'):
            old.unlink()
        parts = []
        for i, g in enumerate(groups, 1):
            path = out / f'texture-{i:02d}.bin'
            size = pack(stage, g, path)
            print(f'  {path.name}: {len(g)} files, {size:,} bytes', flush=True)
            parts.append(path)
        if all(p.stat().st_size < PART_LIMIT for p in parts):
            break
    else:
        sys.exit(f'the pack does not fit in {MAX_PARTS} parts under {PART_LIMIT:,} bytes')

    # Every part on its own, then all of them together, must give back exactly the staged pack.
    check = out / 'check'
    if check.exists():
        shutil.rmtree(check)
    for p in parts:
        subprocess.run([str(SEVENZIP), 'x', '-t7z', '-y', '-bsp0', '-bso0', f'-o{check}', str(p)], check=True)
    got = check / 'texture_pack'
    if (got / 'manifest.txt').read_text(encoding='ascii') != manifest:
        sys.exit('manifest.txt did not survive the round trip')
    for h, n, rel in entries:
        f = got / rel
        if not f.is_file() or f.stat().st_size != n or sha256(f) != h:
            sys.exit(f'{rel} did not survive the round trip')
    shutil.rmtree(check)
    shutil.rmtree(stage)
    print('round trip: every texture matches', flush=True)

    tag = f'textures-{args.revision}'
    info = {
        'revision': args.revision,
        'tag': tag,
        'textures': len(entries),
        'extracted_bytes': total + len((manifest).encode()) ,
        'manifest_sha256': hashlib.sha256(manifest.encode('ascii')).hexdigest(),
        'parts': [{'name': p.name, 'bytes': p.stat().st_size, 'sha256': sha256(p),
                   'url': f'https://github.com/{REPO}/releases/download/{tag}/{p.name}'} for p in parts],
    }
    (out / 'texture-payload.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    (out / 'SHA256SUMS.txt').write_text(''.join(f"{x['sha256']}  {x['name']}\n" for x in info['parts']),
                                        encoding='ascii', newline='\n')

    lines = [
        f'; Hi-res texture pack revision {args.revision}: what both installers download, check and extract.',
        '; Written by scripts/build_texture_pack.py; do not edit by hand. Each define can be overridden for tests.',
    ]

    def define(name, value):
        lines.extend([f'#ifndef {name}', f'#define {name} {value}', '#endif'])

    define('TextureVersion', f'"{args.revision}"')
    define('TextureDisplayVersion', f'"Revision {args.revision}"')
    define('PackGuid', f'"{PACK_GUID}"')
    define('TextureExtractedBytes', info['extracted_bytes'])
    define('TextureManifestSHA256', f'"{info["manifest_sha256"]}"')
    define('TextureFileCount', len(entries))
    define('TextureCacheRoot', '"{localappdata}\\MvC2Recomp\\Downloads"')
    define('TexturePartCount', len(parts))
    define('TextureDownloadBytes', sum(x['bytes'] for x in info['parts']))
    for i, x in enumerate(info['parts'], 1):
        define(f'TextureURL{i}', f'"{x["url"]}"')
        define(f'TextureSHA256{i}', f'"{x["sha256"]}"')
        define(f'TextureBytes{i}', x['bytes'])
    (ROOT / 'installer' / 'texture_payload.iss').write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
