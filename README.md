# mvsc2-recomp

A static recompilation of **Marvel vs. Capcom 2 (Original Xbox, NTSC-U)** to a native Windows x64 program.

The game's Xbox executable is disassembled and translated to C, then built with MSVC against a small runtime that stands in for the Xbox kernel, the NV2A GPU, the sound hardware and the USB pads. It is not an emulator: the game's code runs directly on your CPU.

> **Status: version 1.0.0.** The game plays from the title screen through the menus and character select into fights. A full Arcade run, two-player play and saves across sessions have not been fully tested.

**This repo has no game data.** It holds only the project's own source code and what is needed to build it. It does not contain the disc image, the game's files, `default.xbe`, the C code generated from the game, or a built game. You need your own copy of the game, serial CC-007.

## Features

- Direct3D 11 rendering, with three render scales: Original, Medium and High.
- An upscaler (Lanczos resampling and contrast-adaptive sharpening) for when the picture is shown larger than it is drawn.
- Sprite filters for the characters: xBR, OmniScale or ScaleFX, with optional smoothing and sharpening. Stages are left alone.
- An optional hi-res texture pack for stages and menus, installed by Setup or by its own addon (see Installing). The game also reads DDS or PNG files named by hashes of their colors and transparency from `texture_pack` beside the game or in `%LOCALAPPDATA%\MvC2Recomp\texture_pack`.
- Borderless fullscreen, windowed, and a choice of 4:3 or stretched picture.
- Game speed locked at 60 FPS, because MvC2's speed is tied to its frame rate.
- Xbox One, Series and 360 controllers through XInput: up to 4 players, hot-plug and rumble.
- A Setup that copies the game files from your own disc image or folder, a settings program (`config.exe`) with Video, Controls and Textures tabs, and [RecompLauncher](https://recomplauncher.com/sdk) support up to Level 3.

## Installing

Download from [Releases](https://github.com/dalves408/mvsc2-recomp/releases):

- **`MvC2-Setup-<version>.exe`** installs the game (to `C:\Games\Marvel vs. Capcom 2` by default) and copies the game files from your own disc image (`.iso` or `.xiso`) or folder. Run over an existing install, it updates or repairs the program files and keeps your game files, saves and settings. Its optional **hi-res texture pack** task downloads the pack (634 MB) and installs it.
- **`MvC2-HiRes-Textures-<n>-Setup.exe`**, on the `textures-<n>` release, adds the hi-res texture pack to an installed game.
- **`MvC2-v<version>-Windows-x64.zip`** is for RecompLauncher, which sets the game up from your disc itself.

Both Setups use a `texture-01.bin` placed beside them instead of downloading it, resume an interrupted download, and check the file's size and SHA-256 before installing.

## Controls

| Key | In the game | | Key | In the game |
|---|---|---|---|---|
| Arrow keys | D-pad | | Q / E | White / Black |
| Enter | Start | | 1 / 3 | Left / Right trigger |
| Backspace | Back | | W A S D | Left stick |
| Z / X / C / V | A / B / X / Y | | Left Shift / Left Ctrl | Left / Right thumb click |

The keyboard always counts as player 1, alongside any controller. The keys above are the defaults: the Controls tab of the settings program (`config.exe`, "Marvel vs. Capcom 2 Settings" in the Start menu) gives each Xbox control a key and an alternate key. PlayStation and Switch pads need Steam Input or DS4Windows to appear as XInput. The game's own CONFIG menu rebinds its buttons.

| Key | What it does |
|---|---|
| F1, or Back + Start on controller 1 | Settings menu |
| Esc | "Do you want to quit?" |
| F11 or Alt+Enter | Switch fullscreen; the choice is remembered |

The game pauses while a menu is open. In a menu, Up/Down chooses, Left/Right changes, Enter or A confirms, and Esc or B closes. Render scale and hi-res textures apply when the menu closes.

Settings are saved in `%LOCALAPPDATA%\MvC2Recomp\settings.ini`: the video options and the hi-res texture switch in `[display]`, your keys in `[keys]`. Saves are in `SaveData` beside it. `RECOMPLAUNCHER_MODE=bigpicture` starts fullscreen without changing the saved setting; `desktop` uses the saved setting.

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
python scripts\build_texture_pack.py --source <folder of .dds files> --revision 1
python scripts\build_installer.py --version 1.0.0
```

This builds the player version in `build_release`, with the C runtime linked in and developer switches ignored. `build_texture_pack.py` packs the hi-res textures into 7z `.bin` parts under 2 GB each and writes their sizes, SHA-256 and download links to `installer\texture_payload.iss`. `build_installer.py` then writes `MvC2-Setup-<version>.exe`, the texture addon `MvC2-HiRes-Textures-<n>-Setup.exe`, a RecompLauncher ZIP and the `.bin` parts to `installer\Output`. They contain code or art from the game, so they must not be committed: the `.bin` parts go on a `textures-<n>` release, the rest on the game's release.

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
| `installer/` | Setup scripts, disc importer, texture downloader, settings program |
| `scripts/` | Code generation and release build |
| `resources/` | Icon and version information |

## Known issues

- **Not fully tested:** a full Arcade run, two-player Versus, saves across sessions, memory units, and AMD or Intel GPUs on the current build.
- **Slow PCs:** an i7-2860QM with a GTX 560M runs fights at about 58 FPS at Original. Higher render scales may drop below 60, and the game then runs slower, because its speed is tied to its frame rate.
- **Renderer gaps:** no register combiners, and texture stages 1-3 are missing.
- **Crashes:** rare crashes and freezes stopped reproducing in repeated scripted runs after the threading fixes, but their exact cause was not proven.
- **Windows N** may need the Media Feature Pack.

## Credits and legal

- [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) provides the recompilation toolkit, under the MIT license.
- The sprite filters are ports of OmniScale by Lior Halphon, ScaleFX by Sp00kyFox and xBR by Hyllian, from [libretro/slang-shaders](https://github.com/libretro/slang-shaders), each under MIT; their notices are in `src/sprite_filter.c`.
- This project's own code is under the [MIT license](LICENSE). That license does not cover the game or anything generated from it.
- Marvel vs. Capcom 2 is the property of Capcom and Marvel. This project is unofficial and is not affiliated with either company.
