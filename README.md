# mvsc2-recomp

A static recompilation of **Marvel vs. Capcom 2 (Original Xbox, NTSC-U)** to a native Windows x64 program.

The game's Xbox executable is disassembled and translated to C, then built with MSVC against a small runtime that stands in for the Xbox kernel, the NV2A GPU, the sound hardware and the USB pads. It is not an emulator: the game's code runs directly on your CPU.

> **Status: playtest. There is no release yet.** The game plays from the title screen through the menus and character select into fights. A full Arcade run, two-player play and saves across sessions have not been fully tested.

**This repo has no game data.** It holds only the project's own source code and what is needed to build it. It does not contain the disc image, the game's files, `default.xbe`, the C code generated from the game, or a built game. You need your own copy of the game, serial CC-007.

## Features

- Direct3D 11 rendering, with render scale from 1x to 5x (640x480 to 3200x2400).
- An upscaler (Lanczos resampling and contrast-adaptive sharpening) for when the picture is shown larger than it is drawn.
- Support for a hi-res texture pack for stages and menus. No pack is included; put DDS or PNG files named by hashes of their colors and transparency in `%LOCALAPPDATA%\MvC2Recomp\texture_pack`.
- Borderless fullscreen, windowed, and a choice of 4:3 or stretched picture.
- Game speed locked at 60 FPS, because MvC2's speed is tied to its frame rate.
- Xbox One, Series and 360 controllers through XInput: up to 4 players, hot-plug and rumble.
- A Setup that copies the game files from your own disc image or folder, and [RecompLauncher](https://recomplauncher.com/sdk) support.

## Controls

| Key | In the game | | Key | In the game |
|---|---|---|---|---|
| Arrow keys | D-pad | | Q / E | White / Black |
| Enter | Start | | 1 / 3 | Left / Right trigger |
| Backspace | Back | | W A S D | Left stick |
| Z / X / C / V | A / B / X / Y | | Left Shift / Left Ctrl | Left / Right thumb click |

The keyboard always counts as player 1, alongside any controller. PlayStation and Switch pads need Steam Input or DS4Windows to appear as XInput. The game's own CONFIG menu rebinds its buttons.

| Key | What it does |
|---|---|
| F1, or Back + Start on controller 1 | Settings menu |
| Esc | "Do you want to quit?" |
| F11 or Alt+Enter | Switch fullscreen; the choice is remembered |

The game pauses while a menu is open. In a menu, Up/Down chooses, Left/Right changes, Enter or A confirms, and Esc or B closes. Render scale and hi-res textures apply when the menu closes.

Settings are saved in `%LOCALAPPDATA%\MvC2Recomp\settings.ini`, and saves in `SaveData` beside it. `RECOMPLAUNCHER_MODE=bigpicture` starts fullscreen without changing the saved setting.

## Building

You need Windows 10 or 11 x64, Visual Studio 2022 C++ Build Tools, CMake, Python 3.12 with `capstone`, and your own copy of the game. To build the installer you also need Inno Setup 6 and Pillow.

The scripts assume this layout. If yours differs, edit the paths in `scripts/regen.ps1`.

```
C:\Recomp\mvc2_pc              this repo
C:\Recomp\tools\xboxrecomp     the toolkit
C:\Recomp\MvsC2\extracted      your extracted game files (default.xbe and data)
```

1. **Toolkit.** Clone [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), check out commit `f0f8a46`, and apply every file in `patches/` in order. There is no 0006.

   ```powershell
   git -C C:\Recomp\tools\xboxrecomp checkout f0f8a46
   Get-ChildItem C:\Recomp\mvc2_pc\patches\*.patch | Sort-Object Name | ForEach-Object { git -C C:\Recomp\tools\xboxrecomp apply --ignore-whitespace --whitespace=nowarn $_.FullName }
   ```

2. **Game files.** Extract your own copy of the game to `C:\Recomp\MvsC2\extracted`.
3. **Generate the code.** Run `.\scripts\regen.ps1`, which takes about 15 minutes. It translates the game into `src/recomp/gen/`, which stays out of git, and stops with `FATAL` if a step fails.
4. **Build.**

   ```powershell
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release -j 12
   ```

5. **Run.** Put a folder or junction named `game` next to `build\Release\mvc2_recomp.exe`, pointing at your extracted files. This developer build needs the Visual C++ runtime and shows FPS in the title bar.

### Installer

```powershell
python installer\gen_manifest.py C:\Recomp\MvsC2\extracted
python scripts\build_installer.py --version 1.0.0
```

This builds the player version in `build_release`, with the C runtime linked in and developer switches ignored. It writes `MvC2-Setup-<version>.exe` and a RecompLauncher ZIP to `installer\Output`. Both contain code translated from the game, so they must not be committed.

## How it works

1. The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit parses the XBE, finds its functions and translates them to C.
2. The game writes NV2A pushbuffers directly. The toolkit's executor decodes them on its own thread and runs the game's vertex programs, and `src/gpu_backend.c` draws the result with Direct3D 11.
3. The window presents each frame on its own thread, so the display cannot hold up the game.
4. Input is an emulated Xbox USB gamepad, fed from XInput and the keyboard.
5. Guest threads share one host core, like the console's one CPU, and code that relied on that is handled explicitly: raised IRQL exclusion, thread priorities, and read-modify-write instructions as one atomic step.

| Path | Contents |
|---|---|
| `src/` | Entry point, renderer, ESC and F1 overlays, hand-written overrides |
| `patches/` | Changes to the xboxrecomp toolkit, applied in order |
| `config/` | Extra function entry points for the recompiler |
| `installer/` | Setup script, disc importer, settings program |
| `scripts/` | Code generation and release build |
| `resources/` | Icon and version information |

## Known issues

- **Not fully tested:** a full Arcade run, two-player Versus, saves across sessions, memory units, and AMD or Intel GPUs on the current build.
- **Slow PCs:** an i7-2860QM with a GTX 560M runs fights at about 58 FPS at 1x. Higher render scales may drop below 60, and the game then runs slower, because its speed is tied to its frame rate.
- **Renderer gaps:** no register combiners, and texture stages 1-3 are missing.
- **Crashes:** rare crashes and freezes stopped reproducing in repeated scripted runs after the threading fixes, but their exact cause was not proven.
- **Windows N** may need the Media Feature Pack.

## Credits and legal

- [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) provides the recompilation toolkit, under the MIT license. Its MCPX APU sources and `nv2a_regs.h` come from [xemu](https://github.com/xemu-project/xemu) and are under LGPL-2.1-or-later. Patches to those files stay under that license; see the toolkit's `NOTICE`.
- This project's own code is under the [MIT license](LICENSE). That license does not cover the game or anything generated from it.
- Marvel vs. Capcom 2 is the property of Capcom and Marvel. This project is unofficial and is not affiliated with either company.
