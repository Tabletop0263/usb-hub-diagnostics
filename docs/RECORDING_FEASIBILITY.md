# Recording Wii video to the hub's SD card

Status key: [tested here] = run in the build sandbox; [Wii test] = page 5 of the app,
needs your console; [unverified] = reasoning only.

## What exists
* WVR1 format + portable C encoder (`source/vrec.[ch]`) - tested here: encode ->
  Python decode is bit-exact for scale 1/2 and lossy tile-skip; MP4 output verified with
  ffmpeg; dropped-frame timing preserved (converter repeats frames to follow timestamps).
* `tools/wvr2mp4.py in.wvr out.mp4 [--upscale 2] [--crf 18] [--info] [--raw f.yuv]`
  (needs python3, numpy, ffmpeg). No audio.
* App page 5 measures, on the console: real write MiB/s + worst stall, encoder ms/frame
  and KB/frame (box-only vs full-change content, scale 1 and 2), and an end-to-end
  300-frame record to a real `.wvr` you can convert.

## Size of the problem [tested here, synthetic worst case]
Full-screen moving gradient (every tile changes): 4:2:0 only saves 25% (447 vs 600 KB at
640x480). Half-res (scale 2) = 112 KB/frame = 3.3 MiB/s at 30 fps. Games with static HUD
or menus will do better; fast full-screen motion will not. Measured card read speed is
~7 MiB/s; write speed is unknown until page 5 runs.

## The hard part: capturing a RETAIL game from RiftWii [unverified]
Page 5 only proves storage + encoder. A running game owns the console, so recording needs:
1. A hook injected into the game that grabs the framebuffer each frame (the game calls
   libogc-style VIDEO set-framebuffer routines; patchable at runtime, which RiftWii
   already does for other patches - not yet read).
2. A way to WRITE to the card from game context: the game has no FAT driver. Options:
   raw sector writes to a reserved region via IOS, or a small resident writer.
   Raw writes to an unused card region are safe only if the region is truly outside the
   filesystem - needs a dedicated partition.
3. CPU/RAM headroom beside the game: unknown; encode cost is what page 5 measures.
Next step after page 5 numbers: read RiftWii's patch/hook code to see what the hook
and storage path would take.
