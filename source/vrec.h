/* WVR1 - Wii Video Record format (big-endian, planar YUV 4:2:0, BT.601 limited)
 *
 * File   : 32-byte header, then frame records until EOF (truncated tail is OK).
 * Header : "WVR1" u16 version(1) u16 flags(0) u16 w u16 h u16 tile(16)
 *          u16 scale(1|2, informational) u16 fps_num u16 fps_den, 12 bytes zero
 * Frame  : u8 0xF1, u8 type(0=key,1=delta), u16 ntiles, u32 payload_len,
 *          u32 ts_ms, u32 frame_no, then payload.
 * Payload: delta -> ceil(tx*ty/8) byte bitmap (MSB first, raster order) then data
 *          for set tiles; key -> data for ALL tiles, no bitmap.
 * Tile   : 16x16 Y (256 B) + 8x8 U (64 B) + 8x8 V (64 B) = 384 B, row-major.
 * Tiles not sent keep the previous decoded content (encoder tracks the decoded
 * state, so no drift). Plain C, no libogc dependency.
 */
#ifndef VREC_H
#define VREC_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int src_w, src_h, scale;
    int ow, oh, cw, ch, tx, ty;
    uint32_t thresh;            /* 0 = send tile on any change; >0 = SAD threshold (lossy) */
    int fps_num, fps_den;
    uint8_t *cur[3], *prev[3];
    uint32_t frame_no;
    int have_prev;
} vrec_enc;

int    vrec_init(vrec_enc *e, int src_w, int src_h, int scale, uint32_t thresh, int fps_num, int fps_den);
void   vrec_free(vrec_enc *e);
size_t vrec_write_header(const vrec_enc *e, uint8_t *out);               /* returns 32 */
/* xfb: YCbYCr (YUY2) frame, stride in bytes. Returns bytes written to out, 0 if cap too small. */
size_t vrec_encode(vrec_enc *e, const uint8_t *xfb, int stride, uint32_t ts_ms,
                   int force_key, uint8_t *out, size_t cap);
size_t vrec_max_frame_bytes(const vrec_enc *e);
#endif
