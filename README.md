# USB Hub Diagnostics (Wii, libogc, IOS58)

Read-only diagnostic for a USB hub / SD reader on the Wii. Writes nothing anywhere
(no NAND, no SD, no USB, no log file); all results are on screen.

## STATUS (be aware)
* Source compiles clean (`-fsyntax-only -Wall -Wextra`) against the **current libogc
  (master, Oct 2026) and libfat headers** using a PowerPC cross compiler.
* **It has NOT been linked into a boot.dol and NOT been run on a Wii.** The sandbox
  that produced this had no devkitPPC (pkg.devkitpro.org unreachable). So
  `apps/usb-hub-diagnostics/boot.dol` does not exist yet; build it with one of the
  methods below. Makefile/linking is unverified until you do.
* No benchmark numbers exist yet. Run it on hardware to get them.

## Build
### A) GitHub Actions (no local toolchain)
Push this folder to a repo. `.github/workflows/build.yml` builds in `devkitpro/devkitppc`
and uploads `out/apps/usb-hub-diagnostics/` (boot.dol + meta.xml + icon.png) as an artifact.

### B) Local devkitPro
    dkp-pacman -S wii-dev          # devkitPPC, libogc, libfat
    export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
    make
    cp boot.dol apps/usb-hub-diagnostics/boot.dol

## Install
Copy `apps/usb-hub-diagnostics/` to `sd:/apps/` on the Wii's own SD card (or
`make install SDROOT=/path/to/sd`). Launch from the Homebrew Channel.

## Use
1. Put the hub in the Wii **before** launching. Keep the Wii's own SD for loading HBC.
2. Pages: overview -> USB enumeration -> LUN probe -> read-only mount -> benchmarks.
   A = next page, HOME = exit.
3. Photograph/note each screen; the reports in `docs/` say how to interpret them.

## Safety properties
* Mounts through a wrapper `DISC_INTERFACE` with no `FEATURE_MEDIUM_CANWRITE`;
  libfat then sets `partition->readOnly` (libfat/source/partition.c) and the wrapper's
  `writeSectors` always returns false.
* Never opens files for writing; never calls format/ES/NAND APIs; no IOS reload.
