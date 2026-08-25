#!/usr/bin/env python3
"""Packages the built pcsx_rearmed_3ds.elf into an installable 3DS CIA.

Unlike tools/make_ds_forwarder.py (one forwarder per DS ROM, each with
its own generated title ID and the ROM's own icon), this is a single,
fixed application -- reuses the same bannertool/makerom steps, but with
one permanent title ID and the project's own logo as the icon/banner.

Usage:
    package_cia.py [--output DIR]

Requires the same tools as make_ds_forwarder.py (bannertool, makerom) --
see that script's docstring for what each does.

IMPORTANT: the --rsf-template file (shared with make_ds_forwarder.py,
lives outside this repo) must declare "mvd:STD" under
AccessControlInfo/ServiceAccessControl and "mvd: 0x0004013020004102"
under SystemControlInfo/Dependency for intro_video.c's New3DS hardware
video decode to work -- without it, mvdstdInit() hung the app outright
on real hardware instead of failing cleanly (see Core-2-Extreme's
Video_player_for_3DS project, which needs the exact same two entries).
Since that template lives outside version control, a fresh machine
needs this added by hand; there's nothing this script itself can check
or fix at packaging time.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip3 install pillow")

TITLE = "PCSX ReARMed 3DS"
# Fixed, permanent identity for this app (unlike the DS forwarders,
# which derive a per-ROM ID) -- picked arbitrarily out of the same
# 16-bit-safe range make_ds_forwarder.py's own comment documents
# (test-key CIA builds reject UniqueId values much past that).
UNIQUE_ID = 0x5051
PRODUCT_CODE = "CTR-P-PSX3"

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOGO = os.path.expanduser("~/Downloads/sweepds_logo.png")


def _fit_cover(img, size):
    target_w, target_h = size
    src_w, src_h = img.size
    scale = max(target_w / src_w, target_h / src_h)
    new_w, new_h = round(src_w * scale), round(src_h * scale)
    resized = img.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - target_w) // 2
    top = (new_h - target_h) // 2
    return resized.crop((left, top, left + target_w, top + target_h))


def _silent_wav(duration_seconds=1.0, sample_rate=32728):
    num_samples = int(duration_seconds * sample_rate)
    pcm = b"\x00\x00" * num_samples
    data_size = len(pcm)
    fmt_chunk = struct.pack("<HHIIHH", 1, 1, sample_rate, sample_rate * 2, 2, 16)
    riff_size = 4 + (8 + len(fmt_chunk)) + (8 + data_size)
    return (
        b"RIFF" + struct.pack("<I", riff_size) + b"WAVE" +
        b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk +
        b"data" + struct.pack("<I", data_size) + pcm
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", default=os.path.join(HERE, "pcsx_rearmed_3ds.elf"))
    parser.add_argument("--logo", default=DEFAULT_LOGO,
                        help="Icon/banner source image (defaults to the project logo)")
    parser.add_argument("--output-dir", default=HERE)
    parser.add_argument("--bannertool", default=os.path.expanduser(
        "~/Desktop/bannertool/output/mac-x86_64/bannertool"))
    parser.add_argument("--makerom", default=os.path.expanduser(
        "~/Desktop/Project_CTR/makerom/bin/makerom"))
    parser.add_argument("--rsf-template", default=os.path.expanduser(
        "~/Desktop/bannertool/buildtools/3ds/template.rsf"))
    args = parser.parse_args()

    def log(msg):
        print(f"[package_cia] {msg}", file=sys.stderr)

    for path, name in [(args.elf, "ELF"), (args.bannertool, "bannertool"),
                        (args.makerom, "makerom"), (args.rsf_template, "RSF template"),
                        (args.logo, "logo image")]:
        if not os.path.isfile(path):
            sys.exit(f"{name} not found at {path}")

    with tempfile.TemporaryDirectory(prefix="psx3ds_cia_") as work_dir:
        logo_img = Image.open(args.logo).convert("RGBA")

        icon_png = os.path.join(work_dir, "icon.png")
        _fit_cover(logo_img, (48, 48)).save(icon_png)

        smdh = os.path.join(work_dir, "app.smdh")
        log(f"Building SMDH -> {smdh}")
        subprocess.run(
            [
                args.bannertool, "makesmdh",
                "-s", TITLE, "-l", TITLE + " -- PS1 emulation (PCSX ReARMed core)",
                "-p", "SweepDS Emu Project",
                "-i", icon_png, "-o", smdh,
                "-f", "visible,extendedbanner",
            ],
            check=True,
        )

        banner_png = os.path.join(work_dir, "banner.png")
        _fit_cover(logo_img, (256, 128)).save(banner_png)

        silence_wav = os.path.join(work_dir, "silence.wav")
        with open(silence_wav, "wb") as f:
            f.write(_silent_wav())

        banner = os.path.join(work_dir, "app.bnr")
        log(f"Building banner -> {banner}")
        subprocess.run(
            [args.bannertool, "makebanner", "-i", banner_png, "-a", silence_wav, "-o", banner],
            check=True,
        )

        # romfs/ (the ANALOG toggle's PS1-logo badge, see the Makefile's
        # tex3ds rule) -- built by `make` alongside the .elf, must exist
        # by the time this packages the CIA.
        romfs_dir = os.path.join(HERE, "romfs")
        if not os.path.isdir(romfs_dir) or not os.listdir(romfs_dir):
            sys.exit(f"{romfs_dir} is missing or empty -- run `make` first")

        os.makedirs(args.output_dir, exist_ok=True)
        output_cia = os.path.join(args.output_dir, "pcsx_rearmed_3ds.cia")
        log(f"Packaging CIA -> {output_cia}")
        subprocess.run(
            [
                args.makerom, "-f", "cia", "-o", output_cia,
                "-rsf", args.rsf_template,
                "-elf", args.elf, "-icon", smdh, "-banner", banner,
                "-target", "t", "-exefslogo",
                f"-DAPP_TITLE={TITLE}",
                f"-DAPP_PRODUCT_CODE={PRODUCT_CODE}",
                f"-DAPP_ROMFS={romfs_dir}",
                "-DAPP_CATEGORY=Application",
                f"-DAPP_UNIQUE_ID={UNIQUE_ID}",
                "-DAPP_USE_ON_SD=true",
                "-DAPP_ENCRYPTED=false",
                "-DAPP_MEMORY_TYPE=Application",
                # 64MB (this app's original value, copied from
                # tools/make_ds_forwarder.py's do-nothing stub CIA) is
                # nowhere near enough for a real PS1 emulator -- the
                # device log showed pcsx_rearmed reserving a 32MB
                # linear heap alone, leaving barely 30MB for everything
                # else (app code, PS1 RAM emulation, dynarec code
                # cache, GPU command buffers), and pcsx_rearmed's own
                # log_mem_usage() logs a "past OOM detected, expect
                # instability" warning as a result. 96MB is the max
                # SystemMode an Old3DS supports; New3DS gets the full
                # 178MB extended pool via SystemModeExt on top of that.
                "-DAPP_SYSTEM_MODE=96MB",
                "-DAPP_SYSTEM_MODE_EXT=178MB",
                # main.c already calls osSetSpeedupEnable(true) during
                # gameplay to request New3DS's CPU clock boost -- but
                # that request only has anything to boost *to* if the
                # CIA's own exheader actually declares New3DS speed
                # support in the first place. Declaring 804MHz here
                # does nothing on Old3DS (the OS only applies the boost
                # on New3DS hardware regardless of what's declared) but
                # is up to 3x the CPU clock on New3DS -- easily the
                # single biggest lever available for a GPU_UNAI
                # (software-rendered) PS1 core with no hardware 3D
                # acceleration to fall back on.
                "-DAPP_CPU_SPEED=804MHz",
                "-DAPP_ENABLE_L2_CACHE=true",
                "-DAPP_VERSION_MAJOR=1",
            ],
            check=True,
        )

    print(output_cia)
    log("Done. Install through Azahar's File > Install CIA.")


if __name__ == "__main__":
    main()
