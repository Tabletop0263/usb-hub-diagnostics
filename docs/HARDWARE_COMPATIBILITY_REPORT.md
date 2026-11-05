# Hardware compatibility & Phase 1 research report

Labels: **[src]** verified by reading libogc/libfat source cloned 2026-11-04;
**[knowledge]** general knowledge, not verified here; **[test]** only the Wii run can tell.

## Environment found
* Sandbox: no devkitPro, no DEVKITPPC, no project files. devkitPro package server
  unreachable, so devkitPPC/gcc version **could not be identified**. libogc master was
  cloned from GitHub (HEAD 2026-10-29); libfat HEAD is 2024-01-23.
* libogc now builds with Catnip/CMake; the 3.x tag line is current. Your installed
  versions are unknown until you run `dkp-pacman -Q devkitPPC libogc libfat`.

## How the Wii exposes USB  [src: libogc/usb.c]
* libogc supports three IOS backends: `/dev/usb/oh0|oh1` (OHCI, "V0"), `/dev/usb/ven`
  and `/dev/usb/hid` ("V5"). It does not implement the V4 `hid` or IOS57 `msc` nodes.
* `USB_Initialize()` opens `ven` and `hid`; it keeps `ven` only if GETVERSION returns
  0x50001. `USB_GetDeviceList()` falls back to `oh0` if there is no `ven`, in which case
  **device_id is 0 and only VID/PID are returned.**
* IOS58 is the usual IOS that provides `ven` (this is general knowledge **[knowledge]**; the app prints the IOS version and
  whether device_ids are non-zero so you can see which backend you got **[test]**).
* Public API includes control, bulk, interrupt **and isochronous** read/write
  (`USB_Write/ReadIsoMsg`) **[src: usb.h]**. Whether IOS58 honours iso OUT is **[test]**.

## USB mass storage in libogc  [src: libogc/usbstorage.c]
* Matches interface class 0x08 with protocol 0x50 (Bulk-Only). UAS / USB3-only
  protocols are not used. Subclass is not checked.
* `__io_usbstorage.startup()` lists class-0x08 devices, opens each in order, reads
  `GET_MAX_LUN`, and mounts **the first LUN that answers**, then stays on that one.
  **Implication for your hub:** with a 2-slot reader (SD + microSD) only one slot is
  reachable through `__io_usbstorage`, and which one depends on enumeration order and
  which slots contain cards. The app's LUN probe (own handle, all LUNs) shows both.
* Device ID 0x0b95:0x7720 (a USB LAN adapter) is explicitly skipped.
* Sector read size: the disc interface uses 512 B unless the device reports otherwise.

## Homebrew Channel on USB  [knowledge]
HBC scans `apps/` on SD and USB FAT volumes via the same libogc/libfat path. Your result
("HBC listed apps from the hub's card") strongly implies the hub's reader enumerates as
Bulk-Only mass storage on a FAT volume. **Caveat [test]:** confirm the Wii's front SD
slot was empty, otherwise HBC may have listed that card.

## Is the Vivitar SD reader a standard MSC device?
Likely yes given the above, but the **actual descriptor is unverified** until the app
runs. What to read off screen 1 and 2:
* Interface class 08 / subclass 06 / protocol 50 -> standard Bulk-Only MSC.
* Protocol 62 -> UAS (libogc will not use it).
* One device with 2 LUNs, or separate devices/interfaces, per slot.
* `USB x.yy` in the device line: 2.00/2.10 = high-speed capable; 3.x descriptors can show
  up if the hub's USB3 side is a separate hub device (Wii host is USB 2.0 only).

## Multiple interfaces / independent access
Separate USB devices (the usual case behind an internal hub) get their own device_id and
can be opened independently via `USB_OpenDevice`. Multiple interfaces on **one** device
share one fd; libogc exposes `USB_SetAlternativeInterface`/`USB_SetConfiguration` and
endpoint-addressed transfers, so interfaces are addressable by endpoint number **[src]**.
The app lists every config/interface/endpoint to settle this empirically.

## Power / physical  [knowledge, unmeasured]
Wii ports are specified at 500 mA class. A multiport adapter that also drives HDMI and
USB3 may brown out or reset. If devices drop mid-benchmark, suspect power first.

## Test plan to fill in this report
1. Screen 1: record every VID:PID, bcdUSB, class, interface table.
2. Screen 2: LUN count and which LUN has media.
3. Screen 3: fs type, capacity, free space, read-only mount success.
4. Screen 4: raw MiB/s vs file MiB/s; sustained min/max; checksum mismatches.
Do not trust any speed figure until both raw and file results are on screen.
