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
`mvd: 0x0004013020004102` under `SystemControlInfo/Dependency`) if
you ever re-enable the New3DS hardware video decode path in
`source/intro_video.c` -- see that file's own comments for why it's
currently disabled.

## Status / known limitations

- Old 3DS runs it too, just slower -- several optimizations
  (dual-core rendering, CPU overclock) are New3DS-only by hardware
  necessity.
- Heavy 3D games (racing, fighting, RPG battle screens) are the
  roughest going -- the GPU plugin is a full software rasterizer
  running entirely on the console's own CPU, with no hardware 3D
  acceleration to fall back on. 2D-heavy games run much closer to
  full speed.
- A hardware-decoded boot intro (via the New3DS MVD video service) was
  attempted and hung the app on real hardware for reasons not fully
  root-caused; the code is still present in `source/intro_video.c` but
  disabled (`introVideoInit()` returns `false` unconditionally).

Bug reports, compatibility notes, and performance feedback --
especially from New3DS/New2DS testers -- are welcome as
[Issues](https://github.com/gammermanbob-droid/pcsx-rearmed-3ds/issues).

## License

GPLv2 or later, matching upstream PCSX ReARMed (see `LICENSE`). The
`external/pcsx_rearmed` submodule is upstream's own repository,
unmodified.
