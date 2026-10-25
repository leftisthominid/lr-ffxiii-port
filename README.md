# Lightning Returns: Final Fantasy XIII - Triumfulgurate Edition

A builder for a native Windows and Android port, made from three copies of the game: Xbox 360, PS3, and Steam.

This builds a native Windows and/or Android version of **Lightning Returns: Final Fantasy XIII**
from **your own copy of the Xbox 360 game**, with what your PS3 and Steam copies add. The Xbox 360 code is recompiled to C++ on your
machine (with [ReXGlue](https://github.com/rexglue/rexglue-sdk)) and built into a normal game
program. Nothing from the game is in this repository: no code, no data, no art. Without your own
copy of the game there is nothing to build.

## How this was made

This decompilation / recompilation project was principally coded by an LLM (Claude, by
Anthropic). Another LLM (ChatGPT, by OpenAI) did the initial setup. The person who steered it
has nowhere near the coding experience it would have taken to do this without generative AI.

## What you need

- **The Xbox 360 game** (required): a disc image (.iso), the extracted game folder, or the copy
  an Xbox 360 wrote to a USB drive (Games on Demand / installed). It has to be the release as it
  is on the disc, without a title update (the builder checks `default.xex`).
- Optional, each adds something:
  - **The PS3 disc image** (USA, BLUS31311): the PS3 cinematics and the PS3 button / UI set
    (Settings > Button Prompts).
  - **The Steam version**: Japanese voices, lip-sync, and text (Settings > Voice / Text Language),
    and your Steam saves are imported.
  - **Xbox 360 DLC** (bought on a 360 or an Xbox One / Series console and downloaded on a 360,
    copied off it to USB): the DLC garbs and weapons.
  - **PS3 saves** (copied off a PS3, or from RPCS3): put the save folders in
    `Documents\lightningreturns\ps3_saves`; they are imported when the game starts (experimental).
- Windows 10 or 11, 64-bit, about 30 GB free, and an internet connection for the build tools.
- For Android: a phone or handheld with a Snapdragon (Adreno) GPU, Android 10 or newer, and USB
  debugging on. The builder downloads Google's Android tools itself, adb included.

## Status

Experimental. Built and played on Windows from the hand-made setup this builder automates; the
builder's own Windows build (360 disc image, PS3 disc image, Steam, 360 DLC) has run through on
one PC. Its Android build has so far only run through on Linux; the first Windows run is under way.
Known: the PS3 version of one movie (the ending, `ENA_us`) needs the latest builder.

## Use

Get the builder: **Code > Download ZIP** on this page (or `git clone`), and unpack it anywhere.

Double-click **`Build Lightning Returns.bat`** and answer the questions. The builder finds your
game files where it can, checks each one, downloads the build tools it needs into the output
folder (pinned versions, checked by SHA-256), and does the rest. A stopped build carries on from
where it stopped when you run it again. With a finished build, the same file offers to add the
360 DLC, the PS3 disc, or the Steam version later, or to install on a phone.

Your game files are only read, never changed. Microsoft's C++ build tools (Visual Studio 2022
Build Tools, "Desktop development with C++") are needed on Windows; if they are missing the
builder offers to install them (Windows asks for permission once).

To play on Windows: double-click **`lightningreturns.exe`** in the `Lightning Returns` folder of
the output folder (or the desktop shortcut the builder offers). Everything else is in the game's
Settings menu; Settings > Frame Rate offers 30 fps, 60 fps, and (on Windows, with a 120 Hz display)
120 fps. The exe and the Android app get their icon from your own game: Lightning's face on the title
screen.

## What is in this repository

| Folder | What |
|---|---|
| `builder.ps1`, `builder\` | the builder (PowerShell) |
| `project\` | the ReXGlue project: CMake files, the recompiler manifest and function hints (addresses), and the port's own code (`src\`) |
| `plugin\` | the renderer (the ReXGlue SDK's graphics code with the port's changes) |
| `android\` | the Android app (Java, resources), the CMake wrappers, and the SDK's Android changes |
| `converters\` | the 360 unpacker, the PS3 UI converter, the Japanese voice converter, and the icon maker |
| `scripts\` | the 360 DLC / save importer |

## Credits

- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)**: the recompiler, runtime, and renderer
  this port is built on (BSD-3-Clause), itself derived from **[Xenia](https://github.com/xenia-project/xenia)**.
- **[bucanero](https://github.com/bucanero)**: the FF XIII-family save cipher (ff13-decrypter), the PS3
  save key derivation (apollo-lib), and the PS3 save tools (Apollo Save Tool, pfd_sfo_tools), which the
  Steam and PS3 save import follow.
- **flatz**: pfdtool, the PS3 `PARAM.PFD` format, and the save file decryption.
- **cp25stack**: Lightning Returns' PS3 secure file ID (via Apollo's `games.conf`).
- **Qualcomm**: Snapdragon Game Super Resolution 1, the Android upscaler (BSD-3-Clause).
- **AMD**: the DXBC checksum code (via Xenia).
- **Microsoft**: the GameInput header (MIT), for the controller's Share button.
- **Sean Barrett**: stb_image and stb_image_write (public domain / MIT).
- **Baldur Karlsson**: the RenderDoc API header (MIT).
- Downloaded by the builder, not part of this repository: LLVM, CMake, Ninja,
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (the icon upscale), ffmpeg (Gyan's Windows
  builds), Eclipse Temurin Java, MinGit, Google's Android SDK and NDK, and the third-party libraries
  the ReXGlue SDK uses (SDL3, FFmpeg, Vulkan headers, glslang, SPIRV-Tools, and others), each under
  its own license.

## Licenses

The port's own code: GNU General Public License v3.0 (`LICENSE`); the save import follows
bucanero's GPL-3.0 code. The ReXGlue SDK's license is in `LICENSES\`; every other third-party file
keeps its own license, stated at its top. Lightning Returns: Final Fantasy XIII is © SQUARE ENIX;
this project is not affiliated with or endorsed by Square Enix or Microsoft.
