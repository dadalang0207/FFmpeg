/*
 * Apple Pixlet decoder
 * Copyright (c) 2016 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>

#include "libavutil/imgutils.h"
#include "libavutil/intmath.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "bytestream.h"
#include "get_bits.h"
#include "unary.h"
#include "internal.h"
#include "thread.h"

#define NB_LEVELS 4

#define H 0
#define V 1

typedef struct SubBand {
    unsigned width, height;
    unsigned size;
    unsigned x, y;
} SubBand;

typedef struct PixletContext {
    AVClass *class;

    GetByteContext gb;
    GetBitContext gbit;

    int levels;
    int depth;
    int h, w;

    int16_t *filter[2];
    int16_t *prediction;
    float scaling[4][2][NB_LEVELS];
    SubBand band[4][NB_LEVELS * 3 + 1];
} PixletContext;

static int init_decoder(AVCodecContext *avctx)
{
    PixletContext *ctx = avctx->priv_data;
    int i, plane;

    ctx->filter[0]  = av_malloc_array(ctx->h, sizeof(int16_t));
    ctx->filter[1]  = av_malloc_array(FFMAX(ctx->h, ctx->w) + 16, sizeof(int16_t));
    ctx->prediction = av_malloc_array((ctx->w >> NB_LEVELS), sizeof(int16_t));
    if (!ctx->filter[0] || !ctx->filter[1] || !ctx->prediction)
        return AVERROR(ENOMEM);

    for (plane = 0; plane < 3; plane++) {
        unsigned shift = plane > 0;
        unsigned w = ctx->w >> shift;
        unsigned h = ctx->h >> shift;

        ctx->band[plane][0].width  = w >> NB_LEVELS;
        ctx->band[plane][0].height = h >> NB_LEVELS;
        ctx->band[plane][0].size = (w >> NB_LEVELS) * (h >> NB_LEVELS);

        for (i = 0; i < NB_LEVELS * 3; i++) {
            unsigned scale = ctx->levels - (i / 3);

            ctx->band[plane][i + 1].width  = w >> scale;
            ctx->band[plane][i + 1].height = h >> scale;
            ctx->band[plane][i + 1].size = (w >> scale) * (h >> scale);

            ctx->band[plane][i + 1].x = (w >> scale) * (((i + 1) % 3) != 2);
            ctx->band[plane][i + 1].y = (h >> scale) * (((i + 1) % 3) != 1);
        }
    }

    return 0;
}

static void free_buffers(AVCodecContext *avctx)
{
    PixletContext *ctx = avctx->priv_data;

    av_freep(&ctx->filter[0]);
    av_freep(&ctx->filter[1]);
    av_freep(&ctx->prediction);
}

static av_cold int pixlet_close(AVCodecContext *avctx)
{
    PixletContext *ctx = avctx->priv_data;
    free_buffers(avctx);
    ctx->w = 0;
    ctx->h = 0;
    return 0;
}

static av_cold int pixlet_init(AVCodecContext *avctx)
{
    avctx->pix_fmt = AV_PIX_FMT_YUV420P16;
    avctx->color_range = AVCOL_RANGE_JPEG;
    return 0;
}

static int read_low_coeffs(AVCodecContext *avctx, int16_t *dst, int size, int width, ptrdiff_t stride)
{
    PixletContext *ctx = avctx->priv_data;
    GetBitContext *b = &ctx->gbit;
    unsigned cnt1, nbits, k, j = 0, i = 0;
    int64_t value, state = 3;
    int rlen, escape, flag = 0;

    while (i < size) {
        nbits = FFMIN(ff_clz((state >> 8) + 3) ^ 0x1F, 14);

        cnt1 = get_unary(b, 0, 8);
        if (cnt1 < 8) {
            value = show_bits(b, nbits);
            if (value <= 1) {
                skip_bits(b, nbits - 1);
                escape = ((1 << nbits) - 1) * cnt1;
            } else {
                skip_bits(b, nbits);
                escape = value + ((1 << nbits) - 1) * cnt1 - 1;
            }
        } else {
            escape = get_bits(b, 16);
        }

        value = -((escape + flag) & 1) | 1;
        dst[j++] = value * ((escape + flag + 1) >> 1);
        i++;
        if (j == width) {
            j = 0;
            dst += stride;
        }
        state = 120 * (escape + flag) + state - (120 * state >> 8);
        flag = 0;

        if (state * 4 > 0xFF || i >= size)
            continue;

        nbits = ((state + 8) >> 5) + (state ? ff_clz(state) : 32) - 24;
        escape = av_mod_uintp2(16383, nbits);
        cnt1 = get_unary(b, 0, 8);
        if (cnt1 > 7) {
            rlen = get_bits(b, 16);
        } else {
            value = show_bits(b, nbits);
            if (value > 1) {
                skip_bits(b, nbits);
                rlen = value + escape * cnt1 - 1;
            } else {
                skip_bits(b, nbits - 1);
                rlen = escape * cnt1;
            }
        }

        if (i + rlen > size)
            return AVERROR_INVALIDDATA;
        i += rlen;

        for (k = 0; k < rlen; k++) {
            dst[j++] = 0;
            if (j == width) {
                j = 0;
                dst += stride;
            }
        }

        state = 0;
        flag = rlen < 0xFFFF ? 1 : 0;
    }

    align_get_bits(b);
    return get_bits_count(b) >> 3;
}

static int read_high_coeffs(AVCodecContext *avctx, uint8_t *src, int16_t *dst, int size,
                            int c, int a, int d,
                            int width, ptrdiff_t stride)
{
    PixletContext *ctx = avctx->priv_data;
    GetBitContext *b = &ctx->gbit;
    unsigned cnt1, shbits, rlen, nbits, length, i = 0, j = 0, k;
    int ret, escape, pfx, value, yflag, xflag, flag = 0;
    int64_t state = 3, tmp;

    if ((ret = init_get_bits8(b, src, bytestream2_get_bytes_left(&ctx->gb))) < 0)
      return ret;

    if ((a >= 0) + (a ^ (a >> 31)) - (a >> 31) != 1) {
        nbits = 33 - ff_clz((a >= 0) + (a ^ (a >> 31)) - (a >> 31) - 1);
        if (nbits > 16)
            return AVERROR_INVALIDDATA;
    } else {
        nbits = 1;
    }

    length = 25 - nbits;

    while (i < size) {
        if (state >> 8 != -3) {
            value = ff_clz((state >> 8) + 3) ^ 0x1F;
        } else {
            value = -1;
        }

        cnt1 = get_unary(b, 0, length);

        if (cnt1 >= length) {
            cnt1 = get_bits(b, nbits);
        } else {
            pfx = 14 + ((((uint64_t)(value - 14)) >> 32) & (value - 14));
            cnt1 *= (1 << pfx) - 1;
            shbits = show_bits(b, pfx);
            if (shbits <= 1) {
                skip_bits(b, pfx - 1);
            } else {
                skip_bits(b, pfx);
                cnt1 += shbits - 1;
            }
        }

        xflag = flag + cnt1;
        yflag = xflag;

        if (flag + cnt1 == 0) {
            value = 0;
        } else {
            xflag &= 1u;
            tmp = c * ((yflag + 1) >> 1) + (c >> 1);
            value = xflag + (tmp ^ -xflag);
        }

        i++;
        dst[j++] = value;
        if (j == width) {
            j = 0;
            dst += stride;
        }
        state += d * yflag - (d * state >> 8);

        flag = 0;

        if (state * 4 > 0xFF || i >= size)
            continue;

        pfx = ((state + 8) >> 5) + (state ? ff_clz(state): 32) - 24;
        escape = av_mod_uintp2(16383, pfx);
        cnt1 = get_unary(b, 0, 8);
        if (cnt1 < 8) {
            value = show_bits(b, pfx);
            if (value > 1) {
                skip_bits(b, pfx);
                rlen = value + escape * cnt1 - 1;
            } else {
                skip_bits(b, pfx - 1);
                rlen = escape * cnt1;
            }
        } else {
            if (get_bits1(b))
                value = get_bits(b, 16);
            else
                value = get_bits(b, 8);

            rlen = value + 8 * escape;
        }

        if (rlen > 0xFFFF || i + rlen > size)
            return AVERROR_INVALIDDATA;
        i += rlen;

        for (k = 0; k < rlen; k++) {
            dst[j++] = 0;
            if (j == width) {
                j = 0;
                dst += stride;
            }
        }

        state = 0;
        flag = rlen < 0xFFFF ? 1 : 0;
    }

    align_get_bits(b);
    return get_bits_count(b) >> 3;
}

static int read_highpass(AVCodecContext *avctx, uint8_t *ptr, int plane, AVFrame *frame)
{
    PixletContext *ctx = avctx->priv_data;
    ptrdiff_t stride = frame->linesize[plane] / 2;
    int i, ret;

    for (i = 0; i < ctx->levels * 3; i++) {
        int32_t a = bytestream2_get_be32(&ctx->gb);
        int32_t b = bytestream2_get_be32(&ctx->gb);
        int32_t c = bytestream2_get_be32(&ctx->gb);
        int32_t d = bytestream2_get_be32(&ctx->gb);
        int16_t *dest = (int16_t *)frame->data[plane] + ctx->band[plane][i + 1].x +
                                               stride * ctx->band[plane][i + 1].y;
        unsigned size = ctx->band[plane][i + 1].size;
        uint32_t magic;

        magic = bytestream2_get_be32(&ctx->gb);
        if (magic != 0xDEADBEEF) {
            av_log(avctx, AV_LOG_ERROR, "wrong magic number: 0x%08X for plane %d, band %d\n", magic, plane, i);
            return AVERROR_INVALIDDATA;
        }

        ret = read_high_coeffs(avctx, ptr + bytestream2_tell(&ctx->gb), dest, size,
                               c, (b >= FFABS(a)) ? b : a, d,
                               ctx->band[plane][i + 1].width, stride);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "error in highpass coefficients for plane %d, band %d\n", plane, i);
            return ret;
        }
        bytestream2_skip(&ctx->gb, ret);
    }

    return 0;
}

static void lowpass_prediction(int16_t *dst, int16_t *pred, int width, int height, ptrdiff_t stride)
{
    int16_t *next, val;
    int i, j;

    memset(pred, 0, width * sizeof(*pred));

    for (i = 0; i < height; i++) {
        val     = pred[0] + dst[0];
        dst[0]  = val;
        pred[0] = val;
        next    = dst + 2;
        for (j = 1; j < width; j++, next++) {
            val       = pred[j] + next[-1];
            next[-1]  = val;
            pred[j]   = val;
            next[-1] += next[-2];
        }
        dst += stride;
    }
}

static void filter(int16_t *dest, int16_t *tmp, unsigned size, float SCALE)
{
    int16_t *low, *high, *ll, *lh, *hl, *hh;
    int hsize, i, j;
    float value;

    hsize = size >> 1;
    low = tmp + 4;
    high = &low[hsize + 8];

    memcpy(low, dest, size);
    memcpy(high, dest + hsize, size);

    ll = &low[hsize];
    lh = &low[hsize];
    hl = &high[hsize];
    hh = hl;
    for (i = 4, j = 2; i; i--, j++, ll--, hh++, lh++, hl--) {
        low[i - 5]  = low[j - 1];
        lh[0]       = ll[-1];
        high[i - 5] = high[j - 2];
        hh[0]       = hl[-2];
    }

    for (i = 0; i < hsize; i++) {
        value = low [i+1] * -0.07576144003329376f +
                low [i  ] *  0.8586296626673486f  +
                low [i-1] * -0.07576144003329376f +
                high[i  ] *  0.3535533905932737f  +
                high[i-1] *  0.3535533905932737f;
        dest[i * 2] = av_clipf(value * SCALE, INT16_MIN, INT16_MAX);
    }

    for (i = 0; i < hsize; i++) {
        value = low [i+2] * -0.01515228715813062f +
                low [i+1] *  0.3687056777514043f  +
                low [i  ] *  0.3687056777514043f  +
                low [i-1] * -0.01515228715813062f +
                high[i+1] *  0.07071067811865475f +
                high[i  ] * -0.8485281374238569f  +
                high[i-1] *  0.07071067811865475f;
        dest[i * 2 + 1] = av_clipf(value * SCALE, INT16_MIN, INT16_MAX);
    }
}

static void reconstruction(AVCodecContext *avctx,
                           int16_t *dest, unsigned width, unsigned height, ptrdiff_t stride, int nb_levels,
                           float *scaling_H, float *scaling_V)
{
    PixletContext *ctx = avctx->priv_data;
    unsigned scaled_width, scaled_height;
    float scale_H, scale_V;
    int16_t *ptr, *tmp;
    int i, j, k;

    scaled_height = height >> nb_levels;
    scaled_width  = width  >> nb_levels;
    tmp = ctx->filter[0];

    for (i = 0; i < nb_levels; i++) {
        scaled_width  <<= 1;
        scaled_height <<= 1;
        scale_H = scaling_H[i];
        scale_V = scaling_V[i];

        ptr = dest;
        for (j = 0; j < scaled_height; j++) {
            filter(ptr, ctx->filter[1], scaled_width, scale_V);
            ptr += stride;
        }

        for (j = 0; j < scaled_width; j++) {
            ptr = dest + j;
            for (k = 0; k < scaled_height; k++) {
                tmp[k] = *ptr;
                ptr += stride;
            }

            filter(tmp, ctx->filter[1], scaled_height, scale_H);

            ptr = dest + j;
            for (k = 0; k < scaled_height; k++) {
                *ptr = tmp[k];
                ptr += stride;
            }
        }
    }
}

#define SQR(a) ((a) * (a))

static void postprocess_luma(AVFrame *frame, int w, int h, int depth)
{
    uint16_t *dsty = (uint16_t *)frame->data[0];
    int16_t *srcy  = (int16_t *)frame->data[0];
    ptrdiff_t stridey = frame->linesize[0] / 2;
    const float factor = 1. / ((1 << depth) - 1);
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            dsty[i] = SQR(FFMAX(srcy[i], 0) * factor) * 65535;
        }
        dsty += stridey;
        srcy += stridey;
    }
}

static void postprocess_chroma(AVFrame *frame, int w, int h, int depth)
{
    uint16_t *dstu = (uint16_t *)frame->data[1];
    uint16_t *dstv = (uint16_t *)frame->data[2];
    int16_t *srcu  = (int16_t *)frame->data[1];
    int16_t *srcv  = (int16_t *)frame->data[2];
    ptrdiff_t strideu = frame->linesize[1] / 2;
    ptrdiff_t stridev = frame->linesize[2] / 2;
    const int add = 1 << (depth - 1);
    const int shift = 16 - depth;
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            dstu[i] = (add + srcu[i]) << shift;
            dstv[i] = (add + srcv[i]) << shift;
        }
        dstu += strideu;
        dstv += stridev;
        srcu += strideu;
        srcv += stridev;
    }
}

static int decode_plane(AVCodecContext *avctx, int plane, AVPacket *avpkt, AVFrame *frame)
{
    PixletContext *ctx = avctx->priv_data;
    ptrdiff_t stride = frame->linesize[plane] / 2;
    unsigned shift = plane > 0;
    int16_t *dst;
    int i, ret;

    for (i = ctx->levels - 1; i >= 0; i--) {
        ctx->scaling[plane][H][i] = 1000000. / sign_extend(bytestream2_get_be32(&ctx->gb), 32);
        ctx->scaling[plane][V][i] = 1000000. / sign_extend(bytestream2_get_be32(&ctx->gb), 32);
    }

    bytestream2_skip(&ctx->gb, 4);

    dst = (int16_t *)frame->data[plane];
    dst[0] = sign_extend(bytestream2_get_be16(&ctx->gb), 16);

    if ((ret = init_get_bits8(&ctx->gbit, avpkt->data + bytestream2_tell(&ctx->gb),
                              bytestream2_get_bytes_left(&ctx->gb))) < 0)
        return ret;

    ret = read_low_coeffs(avctx, dst + 1, ctx->band[plane][0].width - 1, ctx->band[plane][0].width - 1, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "error in lowpass coefficients for plane %d, top row\n", plane);
        return ret;
    }

    ret = read_low_coeffs(avctx, dst + stride, ctx->band[plane][0].height - 1, 1, stride);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "error in lowpass coefficients for plane %d, left column\n", plane);
        return ret;
    }

    ret = read_low_coeffs(avctx, dst + stride + 1,
                          (ctx->band[plane][0].width - 1) * (ctx->band[plane][0].height - 1),
                          ctx->band[plane][0].width - 1, stride);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "error in lowpass coefficients for plane %d, rest\n", plane);
        return ret;
    }

    bytestream2_skip(&ctx->gb, ret);
    if (bytestream2_get_bytes_left(&ctx->gb) <= 0) {
        av_log(avctx, AV_LOG_ERROR, "no bytes left\n");
        return AVERROR_INVALIDDATA;
    }

    ret = read_highpass(avctx, avpkt->data, plane, frame);
    if (ret < 0)
        return ret;

    lowpass_prediction(dst, ctx->prediction,
                       ctx->band[plane][0].width, ctx->band[plane][0].height, stride);

    reconstruction(avctx, (int16_t *)frame->data[plane], ctx->w >> shift, ctx->h >> shift,
                   stride, NB_LEVELS, ctx->scaling[plane][H], ctx->scaling[plane][V]);

    return 0;
}

static int pixlet_decode_frame(AVCodecContext *avctx, void *data,
                               int *got_frame, AVPacket *avpkt)
{
    PixletContext *ctx = avctx->priv_data;
    int i, w, h, width, height, ret, version;
    AVFrame *p = data;
    ThreadFrame frame = { .f = data };
    uint32_t pktsize;

    bytestream2_init(&ctx->gb, avpkt->data, avpkt->size);

    pktsize = bytestream2_get_be32(&ctx->gb);
    if (pktsize <= 44 || pktsize - 4 > bytestream2_get_bytes_left(&ctx->gb)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid packet size %u.\n", pktsize);
        return AVERROR_INVALIDDATA;
    }

    version = bytestream2_get_le32(&ctx->gb);
    if (version != 1)
        avpriv_request_sample(avctx, "Version %d", version);

    bytestream2_skip(&ctx->gb, 4);
    if (bytestream2_get_be32(&ctx->gb) != 1)
        return AVERROR_INVALIDDATA;
    bytestream2_skip(&ctx->gb, 4);

    width  = bytestream2_get_be32(&ctx->gb);
    height = bytestream2_get_be32(&ctx->gb);

    w = FFALIGN(width,  1 << (NB_LEVELS + 1));
    h = FFALIGN(height, 1 << (NB_LEVELS + 1));

    ctx->levels = bytestream2_get_be32(&ctx->gb);
    if (ctx->levels != NB_LEVELS)
        return AVERROR_INVALIDDATA;
    ctx->depth = bytestream2_get_be32(&ctx->gb);
    if (ctx->depth < 8 || ctx->depth > 15) {
        avpriv_request_sample(avctx, "Depth %d", ctx->depth);
        return AVERROR_INVALIDDATA;
    }

    ret = ff_set_dimensions(avctx, w, h);
    if (ret < 0)
        return ret;
    avctx->width  = width;
    avctx->height = height;

    if (ctx->w != w || ctx->h != h) {
        free_buffers(avctx);
        ctx->w = w;
        ctx->h = h;

        ret = init_decoder(avctx);
        if (ret < 0) {
            free_buffers(avctx);
            ctx->w = 0;
            ctx->h = 0;
            return ret;
        }
    }

    bytestream2_skip(&ctx->gb, 8);

    p->pict_type = AV_PICTURE_TYPE_I;
    p->key_frame = 1;
    p->color_range = AVCOL_RANGE_JPEG;

    ret = ff_thread_get_buffer(avctx, &frame, 0);
    if (ret < 0)
        return ret;

    for (i = 0; i < 3; i++) {
        ret = decode_plane(avctx, i, avpkt, frame.f);
        if (ret < 0)
            return ret;
        if (avctx->flags & AV_CODEC_FLAG_GRAY)
            break;
    }

    postprocess_luma(frame.f, ctx->w, ctx->h, ctx->depth);
    postprocess_chroma(frame.f, ctx->w >> 1, ctx->h >> 1, ctx->depth);

    *got_frame = 1;

    return pktsize;
}

#if HAVE_THREADS
static int pixlet_init_thread_copy(AVCodecContext *avctx)
{
    PixletContext *ctx = avctx->priv_data;

    ctx->filter[0] = NULL;
    ctx->filter[1] = NULL;
    ctx->prediction = NULL;
    ctx->w = ctx->h = 0;

    return 0;
}
#endif

AVCodec ff_pixlet_decoder = {
    .name             = "pixlet",
    .long_name        = NULL_IF_CONFIG_SMALL("Apple Pixlet"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_PIXLET,
    .init             = pixlet_init,
    .init_thread_copy = ONLY_IF_THREADS_ENABLED(pixlet_init_thread_copy),
    .close            = pixlet_close,
    .decode           = pixlet_decode_frame,
    .priv_data_size   = sizeof(PixletContext),
    .capabilities     = AV_CODEC_CAP_DR1 |
                        AV_CODEC_CAP_FRAME_THREADS,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE |
                        FF_CODEC_CAP_INIT_CLEANUP,
};
