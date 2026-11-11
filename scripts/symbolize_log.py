"""Names the code offsets in a player's log: mvc2_recomp+0x1A2B3C becomes mvc2_recomp+0x1A2B3C [sub_002000A0+0x12].

    python scripts/symbolize_log.py <log> [--symbols out/symbols] [--out <file>]

A release prints no function names (its .pdb is not shipped), only offsets, in the [FREEZE] and [CRASH] reports. The
[BUILD] line at the top of every log gives the version and the id of the .pdb it was linked with; build_installer.py
keeps each release's mvc2_recomp.exe and .pdb in out/symbols/<version>/. This finds the matching pair (by that id, so a
rebuilt exe with the same version number is not mistaken for it) and asks Windows' dbghelp.dll for each name.
Generated functions are named after the guest address they were translated from (sub_XXXXXXXX), so a named stack is the
game's own call chain.
"""
import argparse
import ctypes
import re
import struct
import sys
from ctypes import wintypes as W
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x140000000
OFFSET = re.compile(r'\bmvc2_recomp\+0x([0-9A-Fa-f]+)')


def pdb_id(exe):
    """The CodeView id (GUID + age, as the game prints it) of the .pdb an exe was linked with."""
    data = exe.read_bytes()
    at = data.find(b'RSDS')
    while at >= 0:
        guid = data[at + 4:at + 20]
        age = struct.unpack_from('<I', data, at + 20)[0]
        d1, d2, d3 = struct.unpack_from('<IHH', guid)
        text = f'{d1:08X}{d2:04X}{d3:04X}' + ''.join(f'{b:02X}' for b in guid[8:]) + f'{age:X}'
        if data[at + 24:at + 60].split(b'\0')[0].lower().endswith(b'.pdb'):
            return text
        at = data.find(b'RSDS', at + 4)
    return None


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', W.ULONG), ('TypeIndex', W.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', W.ULONG), ('Size', W.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', W.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', W.ULONG), ('Scope', W.ULONG),
                ('Tag', W.ULONG), ('NameLen', W.ULONG), ('MaxNameLen', W.ULONG), ('Name', ctypes.c_char * 256)]


class Symbols:
    def __init__(self, folder):
        exe = folder / 'mvc2_recomp.exe'
        self.dbg = ctypes.WinDLL('dbghelp.dll')
        self.dbg.SymLoadModuleEx.restype = ctypes.c_uint64
        self.dbg.SymLoadModuleEx.argtypes = [W.HANDLE, W.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                             W.DWORD, ctypes.c_void_p, W.DWORD]
        self.dbg.SymFromAddr.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                         ctypes.POINTER(SYMBOL_INFO)]
        self.proc = W.HANDLE(0x4D564332)              # any unique value: no live process is read
        self.dbg.SymSetOptions(0x2 | 0x40000)         # UNDNAME | FAIL_CRITICAL_ERRORS
        if not self.dbg.SymInitialize(self.proc, str(folder).encode(), False):
            sys.exit('dbghelp could not start')
        size = exe.stat().st_size * 4
        if not self.dbg.SymLoadModuleEx(self.proc, None, str(exe).encode(), None, BASE, size, None, 0):
            sys.exit(f'dbghelp could not load {exe}')

    def name(self, rva):
        info = SYMBOL_INFO()
        info.SizeOfStruct = 88                        # sizeof(SYMBOL_INFO) in C, Name[1] padded
        info.MaxNameLen = 255
        disp = ctypes.c_uint64(0)
        if self.dbg.SymFromAddr(self.proc, BASE + rva, ctypes.byref(disp), ctypes.byref(info)):
            return f'{info.Name.decode(errors="replace")}+0x{disp.value:X}'
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path)
    parser.add_argument('--symbols', type=Path, default=ROOT / 'out' / 'symbols')
    parser.add_argument('--out', type=Path, help='write here instead of <log>.named.log')
    args = parser.parse_args()

    text = args.log.read_text(encoding='utf-8', errors='replace')
    build = re.search(r'\[BUILD\] mvc2_recomp (\S+), symbols (\w+)', text)
    if not build:
        sys.exit('no [BUILD] line: this log is from a build before 1.0.2, which printed no offsets')
    version, wanted = build.group(1).rstrip(','), build.group(2)
    folders = [p.parent for p in args.symbols.glob('*/mvc2_recomp.exe') if pdb_id(p) == wanted]
    if not folders:
        sys.exit(f'no symbols for version {version} (id {wanted}) in {args.symbols}')
    symbols = Symbols(folders[0])
    names = {}

    def named(m):
        rva = int(m.group(1), 16)
        if rva not in names:
            names[rva] = symbols.name(rva)
        return m.group(0) + (f' [{names[rva]}]' if names[rva] else '')

    out = args.out or args.log.with_suffix('.named.log')
    out.write_text(OFFSET.sub(named, text), encoding='utf-8')
    print(f'{version} ({folders[0]}): {len(names)} addresses named -> {out}')


if __name__ == '__main__':
    main()
