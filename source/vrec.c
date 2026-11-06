#include <stdlib.h>
#include <string.h>
#include "vrec.h"

static void p16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int vrec_init(vrec_enc *e, int src_w, int src_h, int scale, uint32_t thresh, int fps_num, int fps_den)
{
    memset(e, 0, sizeof(*e));
    if (scale != 1 && scale != 2) return -1;
    e->src_w = src_w; e->src_h = src_h; e->scale = scale;
    e->ow = (src_w / scale) & ~15;
    e->oh = (src_h / scale) & ~15;
    if (e->ow <= 0 || e->oh <= 0) return -1;
    e->cw = e->ow / 2; e->ch = e->oh / 2;
    e->tx = e->ow / 16; e->ty = e->oh / 16;
    e->thresh = thresh; e->fps_num = fps_num; e->fps_den = fps_den;
    size_t ys = (size_t)e->ow * e->oh, cs = (size_t)e->cw * e->ch;
    for (int i = 0; i < 3; i++) {
        size_t n = i ? cs : ys;
        e->cur[i] = malloc(n); e->prev[i] = calloc(1, n);
        if (!e->cur[i] || !e->prev[i]) { vrec_free(e); return -1; }
    }
    return 0;
}

void vrec_free(vrec_enc *e)
{
    for (int i = 0; i < 3; i++) { free(e->cur[i]); free(e->prev[i]); e->cur[i] = e->prev[i] = NULL; }
}

size_t vrec_max_frame_bytes(const vrec_enc *e)
{
    return 16 + ((size_t)e->tx * e->ty + 7) / 8 + (size_t)e->tx * e->ty * 384;
}

size_t vrec_write_header(const vrec_enc *e, uint8_t *o)
{
    memset(o, 0, 32);
    memcpy(o, "WVR1", 4);
    p16(o + 4, 1); p16(o + 6, 0);
    p16(o + 8, e->ow); p16(o + 10, e->oh); p16(o + 12, 16);
    p16(o + 14, e->scale); p16(o + 16, e->fps_num); p16(o + 18, e->fps_den);
    return 32;
}

/* XFB (YUY2) -> planar 4:2:0, optional 2x box downscale */
static void convert(vrec_enc *e, const uint8_t *xfb, int stride)
{
    int s = e->scale;
    uint8_t *Y = e->cur[0], *U = e->cur[1], *V = e->cur[2];
    for (int oy = 0; oy < e->oh; oy++) {
        for (int ox = 0; ox < e->ow; ox++) {
            if (s == 1) {
                Y[oy * e->ow + ox] = xfb[oy * stride + ox * 2];
            } else {
                const uint8_t *a = xfb + (oy * 2) * stride + ox * 4;
                const uint8_t *b = a + stride;
                Y[oy * e->ow + ox] = (a[0] + a[2] + b[0] + b[2] + 2) >> 2;
            }
        }
    }
    int rows = 2 * s, pairs = s, cnt = rows * pairs;
    for (int cy = 0; cy < e->ch; cy++) {
        for (int cx = 0; cx < e->cw; cx++) {
            uint32_t su = 0, sv = 0;
            for (int r = 0; r < rows; r++) {
                const uint8_t *row = xfb + (cy * rows + r) * stride + (cx * pairs) * 4;
                for (int q = 0; q < pairs; q++) { su += row[q * 4 + 1]; sv += row[q * 4 + 3]; }
            }
            U[cy * e->cw + cx] = (su + cnt / 2) / cnt;
            V[cy * e->cw + cx] = (sv + cnt / 2) / cnt;
        }
    }
}

static uint32_t tile_diff(const vrec_enc *e, int tx, int ty, uint32_t limit)
{
    uint32_t sad = 0;
    for (int r = 0; r < 16; r++) {
        const uint8_t *a = e->cur[0] + (ty * 16 + r) * e->ow + tx * 16;
        const uint8_t *b = e->prev[0] + (ty * 16 + r) * e->ow + tx * 16;
        for (int c = 0; c < 16; c++) sad += a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];
        if (sad > limit) return sad;
    }
    for (int pl = 1; pl < 3; pl++)
        for (int r = 0; r < 8; r++) {
            const uint8_t *a = e->cur[pl] + (ty * 8 + r) * e->cw + tx * 8;
            const uint8_t *b = e->prev[pl] + (ty * 8 + r) * e->cw + tx * 8;
            for (int c = 0; c < 8; c++) sad += a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];
        }
    return sad;
}

static uint8_t *put_tile(vrec_enc *e, int tx, int ty, uint8_t *o)
{
    for (int r = 0; r < 16; r++) {
        size_t off = (size_t)(ty * 16 + r) * e->ow + tx * 16;
        memcpy(o, e->cur[0] + off, 16);
        memcpy(e->prev[0] + off, e->cur[0] + off, 16);
        o += 16;
    }
    for (int pl = 1; pl < 3; pl++)
        for (int r = 0; r < 8; r++) {
            size_t off = (size_t)(ty * 8 + r) * e->cw + tx * 8;
            memcpy(o, e->cur[pl] + off, 8);
            memcpy(e->prev[pl] + off, e->cur[pl] + off, 8);
            o += 8;
        }
    return o;
}

size_t vrec_encode(vrec_enc *e, const uint8_t *xfb, int stride, uint32_t ts_ms,
                   int force_key, uint8_t *out, size_t cap)
{
    if (cap < vrec_max_frame_bytes(e)) return 0;
    convert(e, xfb, stride);
    int key = force_key || !e->have_prev;
    int nt = e->tx * e->ty, nbm = (nt + 7) / 8, changed = 0;
    uint8_t *bm = out + 16, *o = out + 16 + (key ? 0 : nbm);
    if (!key) memset(bm, 0, nbm);
    for (int ty = 0; ty < e->ty; ty++)
        for (int tx = 0; tx < e->tx; tx++) {
            int idx = ty * e->tx + tx;
            int send = key || tile_diff(e, tx, ty, e->thresh) > e->thresh;
            if (!send) continue;
            if (!key) bm[idx >> 3] |= 0x80 >> (idx & 7);
            o = put_tile(e, tx, ty, o);
            changed++;
        }
    out[0] = 0xF1; out[1] = key ? 0 : 1; p16(out + 2, changed);
    p32(out + 4, (uint32_t)(o - (out + 16))); p32(out + 8, ts_ms); p32(out + 12, e->frame_no++);
    e->have_prev = 1;
    return (size_t)(o - out);
}
