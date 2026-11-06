#!/usr/bin/env python3
"""Convert WVR1 (Wii Video Record) files to MP4.  Needs: python3, numpy, ffmpeg.
   python3 wvr2mp4.py in.wvr out.mp4 [--crf 18] [--upscale 2] [--raw out.yuv] [--info]
Output is constant frame rate; frames are repeated to follow the recorded timestamps."""
import argparse, struct, subprocess, sys
import numpy as np

def read_wvr(path):
    d = open(path, 'rb').read()
    if d[:4] != b'WVR1': sys.exit('not a WVR1 file')
    ver, flags, w, h, tile, scale, fn, fd = struct.unpack('>8H', d[4:20])
    return d, dict(w=w, h=h, tile=tile, scale=scale, fn=fn, fd=fd)

def frames(d, hd):
    w, h = hd['w'], hd['h']; tx, ty = w // 16, h // 16; nt = tx * ty; nbm = (nt + 7) // 8
    Y = np.zeros((h, w), np.uint8); U = np.zeros((h // 2, w // 2), np.uint8); V = U.copy()
    Yt = Y.reshape(ty, 16, tx, 16).transpose(0, 2, 1, 3)
    Ut = U.reshape(ty, 8, tx, 8).transpose(0, 2, 1, 3)
    Vt = V.reshape(ty, 8, tx, 8).transpose(0, 2, 1, 3)
    pos, n = 32, len(d)
    while pos + 16 <= n:
        magic, typ, nch, plen, ts, fno = struct.unpack('>BBHIII', d[pos:pos + 16])
        if magic != 0xF1 or pos + 16 + plen > n:
            break                                   # truncated/corrupt tail
        p = d[pos + 16: pos + 16 + plen]; pos += 16 + plen
        if typ == 0:
            idx = np.arange(nt); body = p
        else:
            bits = np.unpackbits(np.frombuffer(p[:nbm], np.uint8))[:nt]
            idx = np.nonzero(bits)[0]; body = p[nbm:]
        if len(idx):
            t = np.frombuffer(body, np.uint8, count=len(idx) * 384).reshape(len(idx), 384)
            ys, xs = idx // tx, idx % tx
            Yt[ys, xs] = t[:, :256].reshape(-1, 16, 16)
            Ut[ys, xs] = t[:, 256:320].reshape(-1, 8, 8)
            Vt[ys, xs] = t[:, 320:384].reshape(-1, 8, 8)
        yield ts, fno, Y, U, V

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('out', nargs='?')
    ap.add_argument('--crf', type=int, default=18); ap.add_argument('--upscale', type=int, default=1)
    ap.add_argument('--raw'); ap.add_argument('--info', action='store_true'); ap.add_argument('--ffmpeg', default='ffmpeg')
    a = ap.parse_args()
    d, hd = read_wvr(a.inp)
    fps = hd['fn'] / hd['fd']; step = 1000.0 / fps
    if a.info:
        n = last = 0; first = None; keys = 0
        for ts, fno, *_ in frames(d, hd):
            n += 1; first = ts if first is None else first; last = ts
        dur = (last - (first or 0)) / 1000.0
        print(f"{hd['w']}x{hd['h']} scale={hd['scale']} nominal {fps:.2f} fps, {n} frames, {dur:.2f}s, "
              f"{len(d)/1048576:.2f} MiB, avg {len(d)/max(dur,1e-9)/1048576:.2f} MiB/s, "
              f"actual {n/max(dur,1e-9):.2f} fps"); return
    if not a.out and not a.raw: sys.exit('need output file')
    sink = open(a.raw, 'wb') if a.raw else None
    proc = None
    if a.out:
        vf = ['-vf', f'scale=iw*{a.upscale}:ih*{a.upscale}:flags=neighbor'] if a.upscale > 1 else []
        cmd = [a.ffmpeg, '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'yuv420p',
               '-s', f"{hd['w']}x{hd['h']}", '-r', f'{fps}', '-i', '-', *vf, '-c:v', 'libx264',
               '-crf', str(a.crf), '-pix_fmt', 'yuv420p', '-colorspace', 'smpte170m',
               '-color_primaries', 'smpte170m', '-color_trc', 'smpte170m', '-color_range', 'tv', a.out]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    def emit(Y, U, V):
        b = Y.tobytes() + U.tobytes() + V.tobytes()
        if sink: sink.write(b)
        if proc: proc.stdin.write(b)
    t0 = None; next_t = 0.0; prev = None; count = 0
    for ts, fno, Y, U, V in frames(d, hd):
        if t0 is None: t0 = ts
        rel = ts - t0
        if prev is not None:
            while next_t + step / 2 <= rel:          # fill timeline with previous image
                emit(*prev); next_t += step; count += 1
        prev = (Y.copy(), U.copy(), V.copy())
    if prev is not None: emit(*prev); count += 1
    if sink: sink.close()
    if proc: proc.stdin.close(); proc.wait()
    print(f'wrote {count} output frames at {fps:.2f} fps')

if __name__ == '__main__': main()
