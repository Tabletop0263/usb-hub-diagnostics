# Video-output feasibility report (Wii -> USB hub -> HDMI)

Status: **analysis only. No video code was written or tested.** Items marked
[unverified] are from general knowledge and must be confirmed.

## 1. Can the hub's HDMI port take video over the Wii's USB-A plug?
The decisive question is *what chip drives HDMI* and *which plug it is wired to*.
* **DisplayPort Alt Mode** runs over the USB-C connector's high-speed pairs with CC/SBU
  negotiation. A USB-A plug has none of that. So if the hub's HDMI depends on DP Alt Mode,
  it **cannot** receive video from the Wii at all, regardless of software. [unverified,
  standard USB-C behaviour]
* **DisplayLink-class (USB graphics adapter)** chips receive video as USB bulk traffic
  and work from a plain USB-A host. If your hub's HDMI works on a normal laptop USB-A port,
  it is this kind. You would see a vendor-specific (class FF) device, not a hub child
  with class 08.
* Many USB-C hubs expose HDMI only on the C plug. A "built-in USB-A" variant may carry
  only the USB data functions (hub + readers).
**Test:** the app's enumeration screen. Look for a separate device (or interface) beside the
hub/reader that is class FF or class 0E/video-related. If only a hub + mass-storage
appears, HDMI is not reachable from the Wii. Also: does HDMI work from the USB-A plug on
a PC? If not, stop.

## 2. What the Wii side provides  [src unless noted]
* libogc exposes arbitrary-device control, bulk, interrupt and iso transfers through
  `USB_OpenDevice` + `USB_Write*Msg`. No filtering by class in the API.
* IOS IPC adds per-call overhead; large buffers must be 32-byte aligned, cache flushed.
  Sustained throughput is **unmeasured**; the benchmark (raw path) gives a first
  data point for bulk **IN**, not OUT.
* Wii XFB format is YCbYCr 4:2:2 (2 B/px) [knowledge]. RGB565 / RGB888 would need CPU
  conversion from XFB or use of EFB peeks/copies; RGB888 costs 50% more bandwidth.

## 3. Bandwidth arithmetic (computed, not measured)
| Mode | Bytes/frame | MB/s @30 | MB/s @60 |
|---|---|---|---|
| 640x480 RGB565 | 614,400 | 18.4 | 36.9 |
| 640x480 RGB888 | 921,600 | 27.6 | 55.3 |
| 640x480 YUV422 (raw XFB) | 614,400 | 18.4 | 36.9 |
USB 2.0 high-speed bulk can carry roughly 30-40 MB/s in good conditions [unverified];
the Wii's IOS path is the unknown. 480p30 raw 16-bit is the only borderline-plausible
target without compression.

## 4. Options
| Path | Needs | Verdict |
|---|---|---|
| Hub HDMI via DP Alt Mode | USB-C source | Impossible from USB-A |
| Hub HDMI is DisplayLink | Driver for its protocol | Possible in principle; custom driver |
| Older DisplayLink (DL-1xx/3xx) | Linux `udl`/`udlfb` shows the open protocol | Port to libogc bulk writes [unverified details] |
| Newer DisplayLink (DL-5xxx/6xxx) | Not covered by the open drivers [unverified] | Likely blocked |
| Other USB-HDMI (FL2000-style) | Open driver exists on Linux [unverified] | Possible, custom driver |
| USB capture devices (MS2130 etc.) | They are INPUT only | Wrong direction |
| FPGA/MCU (FTDI sync FIFO, Cypress FX2/FX3) + HDMI TX | Custom hardware + gateware | Most controllable; you define the protocol |
| Existing Wii HDMI adapters on the AV port | Off-the-shelf | Solves the real need without USB |

## 5. Verdict
* Works today: nothing is proven. Mass-storage on the hub appears to work.
* Needs custom driver: any DisplayLink/FL2000-type adapter, if the hub contains one on the
  USB-A path.
* Needs different hardware: DP-Alt-Mode HDMI, or an FPGA/MCU bridge.
* Next concrete step: run the diagnostic and read the enumeration screen.
