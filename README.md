# PCSX ReARMed 3DS

A standalone PlayStation 1 emulator for Nintendo 3DS / New3DS, built
around the [PCSX ReARMed](https://github.com/libretro/pcsx_rearmed)
libretro core with a native citro2d/citro3d/NDSP frontend written from
scratch for this project -- no RetroArch install required, just a
single CIA.

## Install

Download the latest `.cia` from
[Releases](https://github.com/gammermanbob-droid/pcsx-rearmed-3ds/releases)
and install it like any other homebrew CIA -- through
[Azahar](https://github.com/azahar-emu/azahar)'s File > Install CIA,
FBI, or any other CIA installer, on real hardware or in a 3DS emulator
that supports homebrew titles.

## Usage

Put PS1 discs under `sdmc:/roms/psx/` -- `.cue`/`.bin`, `.chd`,
`.pbp`, `.iso`, and `.img` are all supported, and both a flat dump and
one-folder-per-game layouts work (scans a few levels deep either way).

No BIOS file is required -- the emulator falls back to a built-in HLE
BIOS automatically. If you have a real BIOS dump, drop it in
`sdmc:/3ds/pcsx_rearmed_3ds/system/` and set BIOS mode to "Auto" in
the in-app settings screen to use it instead.

## Features

- Save states (4 slots, per game)
- A real memory card, shared and persisted across games
- An on-screen ANALOG toggle for games that support DualShock analog
  mode -- instant, no input delay
- A boot chime + looping menu music
- An in-game settings screen: BIOS mode, which screen the game renders
  on, a performance/quality toggle (skip-line rendering), and an
  experimental alternate renderer
- New3DS-specific performance tuning: dual-core rendering, CPU
  overclock, and auto frameskip tied to real audio-buffer occupancy

## Building from source

Requires [devkitPro](https://devkitpro.org/) (devkitARM + libctru +
citro2d/citro3d) and [makerom](https://github.com/profi200/Project_CTR)
+ [bannertool](https://github.com/Steveice10/bannertool) for CIA
packaging.

```sh
git submodule update --init --recursive
make
python3 package_cia.py
```

`package_cia.py --help` covers the packaging tool paths it expects
(`--makerom`, `--bannertool`, `--rsf-template`) if yours aren't in the
default locations. **The RSF template must declare the `mvd:STD`
service** under `AccessControlInfo/ServiceAccessControl` (and
`mvd: 0x0004013020004102` under `SystemControlInfo/Dependency`) for
the New3DS hardware video decode path (`source/intro_video.c`) to work
-- without it, the app can hang on boot instead of falling back
cleanly. See that file's own comments for the full story.

## Status / known limitations

- Old 3DS runs it too, just slower -- several optimizations
  (dual-core rendering, CPU overclock) are New3DS-only by hardware
  necessity.
- Heavy 3D games (racing, fighting, RPG battle screens) are the
  roughest going -- the GPU plugin is a full software rasterizer
  running entirely on the console's own CPU, with no hardware 3D
  acceleration to fall back on. 2D-heavy games run much closer to
  full speed.
- The boot intro plays the real PS1 startup video on New3DS via the
  console's own MVD hardware decoder, falling back to an animated
  logo everywhere else (Old 3DS, or if decode setup fails). This is
  the newest, least-hardware-tested part of the app -- an earlier
  version of this hung the whole app on boot instead of falling back,
  since fixed (RSF service permissions) and further hardened (the risk
  service call now runs under a timeout so a repeat failure can't take
  the app down again), but real-hardware confirmation is still
  ongoing. If the app doesn't boot at all on your device, that's the
  first thing to report.

Bug reports, compatibility notes, and performance feedback --
especially from New3DS/New2DS testers -- are welcome as
[Issues](https://github.com/gammermanbob-droid/pcsx-rearmed-3ds/issues).

## License

GPLv2 or later, matching upstream PCSX ReARMed (see `LICENSE`). The
`external/pcsx_rearmed` submodule is upstream's own repository,
unmodified.
