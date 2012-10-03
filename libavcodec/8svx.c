/*
 * Copyright (C) 2008 Jaikrishnan Menon
 * Copyright (C) 2011 Stefano Sabatini
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

/**
 * @file
 * 8svx audio decoder
 * @author Jaikrishnan Menon
 *
 * supports: fibonacci delta encoding
 *         : exponential encoding
 *
 * For more information about the 8SVX format:
 * http://netghost.narod.ru/gff/vendspec/iff/iff.txt
 * http://sox.sourceforge.net/AudioFormats-11.html
 * http://aminet.net/package/mus/misc/wavepak
 * http://amigan.1emu.net/reg/8SVX.txt
 *
 * Samples can be found here:
 * http://aminet.net/mods/smpl/
 */

#include "libavutil/avassert.h"
#include "avcodec.h"
#include "libavutil/common.h"

/** decoder context */
typedef struct EightSvxContext {
    AVFrame frame;
    const int8_t *table;

    /* buffer used to store the whole audio decoded/interleaved chunk,
     * which is sent with the first packet */
    uint8_t *samples;
    int64_t samples_size;
    int samples_idx;
} EightSvxContext;

static const int8_t fibonacci[16]   = { -34,  -21, -13,  -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8,  13, 21 };
static const int8_t exponential[16] = { -128, -64, -32, -16, -8, -4, -2, -1, 0, 1, 2, 4, 8, 16, 32, 64 };

#define MAX_FRAME_SIZE 2048

/**
 * Delta decode the compressed values in src, and put the resulting
 * decoded n samples in dst.
 *
 * @param val starting value assumed by the delta sequence
 * @param table delta sequence table
 * @return size in bytes of the decoded data, must be src_size*2
 */
static int delta_decode(uint8_t *dst, const uint8_t *src, int src_size,
                         unsigned val, const int8_t *table)
{
    uint8_t *dst0 = dst;

    while (src_size--) {
        uint8_t d = *src++;
        val = av_clip_uint8(val + table[d & 0xF]);
        *dst++ = val;
        val = av_clip_uint8(val + table[d >> 4]);
        *dst++ = val;
    }

    return dst-dst0;
}

static void raw_decode(uint8_t *dst, const int8_t *src, int src_size)
{
    while (src_size--)
        *dst++ = *src++ + 128;
}

/** decode a frame */
static int eightsvx_decode_frame(AVCodecContext *avctx, void *data,
                                 int *got_frame_ptr, AVPacket *avpkt)
{
    EightSvxContext *esc = avctx->priv_data;
    int n, out_data_size;
    int ch, ret;
    uint8_t *src;

    /* decode and interleave the first packet */
    if (!esc->samples && avpkt) {
        int packet_size = avpkt->size;

        if (packet_size % avctx->channels) {
            av_log(avctx, AV_LOG_WARNING, "Packet with odd size, ignoring last byte\n");
            if (packet_size < avctx->channels)
                return packet_size;
            packet_size -= packet_size % avctx->channels;
        }
        esc->samples_size = !esc->table ?
            packet_size : avctx->channels + (packet_size-avctx->channels) * 2;
        if (!(esc->samples = av_malloc(esc->samples_size)))
            return AVERROR(ENOMEM);

        /* decompress */
        if (esc->table) {
            const uint8_t *buf = avpkt->data;
            uint8_t *dst;
            int buf_size = avpkt->size;
            int i, n = esc->samples_size;

            if (buf_size < 2) {
                av_log(avctx, AV_LOG_ERROR, "packet size is too small\n");
                return AVERROR(EINVAL);
            }

            /* the uncompressed starting value is contained in the first byte */
            dst = esc->samples;
            for (i = 0; i < avctx->channels; i++) {
                *(dst++) = buf[0]+128;
                delta_decode(dst, buf + 1, buf_size / avctx->channels - 1, (buf[0]+128)&0xFF, esc->table);
                buf += buf_size / avctx->channels;
                dst += n / avctx->channels - 1;
            }
        } else {
            raw_decode(esc->samples, avpkt->data, esc->samples_size);
        }
    }

    /* get output buffer */
    av_assert1(!(esc->samples_size % avctx->channels || esc->samples_idx % avctx->channels));
    esc->frame.nb_samples = FFMIN(MAX_FRAME_SIZE, esc->samples_size - esc->samples_idx)  / avctx->channels;
    if ((ret = avctx->get_buffer(avctx, &esc->frame)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        return ret;
    }

    *got_frame_ptr   = 1;
    *(AVFrame *)data = esc->frame;

    out_data_size = esc->frame.nb_samples;
    for (ch = 0; ch<avctx->channels; ch++) {
        src = esc->samples + esc->samples_idx / avctx->channels + ch * esc->samples_size / avctx->channels;
        memcpy(esc->frame.data[ch], src, out_data_size);
    }
    out_data_size *= avctx->channels;
    esc->samples_idx += out_data_size;

    return esc->table ?
        (avctx->frame_number == 0)*2 + out_data_size / 2 :
        out_data_size;
}

static av_cold int eightsvx_decode_init(AVCodecContext *avctx)
{
    EightSvxContext *esc = avctx->priv_data;

    if (avctx->channels < 1 || avctx->channels > 2) {
        av_log(avctx, AV_LOG_ERROR, "8SVX does not support more than 2 channels\n");
        return AVERROR_INVALIDDATA;
    }

    switch (avctx->codec->id) {
    case AV_CODEC_ID_8SVX_FIB: esc->table = fibonacci;    break;
    case AV_CODEC_ID_8SVX_EXP: esc->table = exponential;  break;
    case AV_CODEC_ID_PCM_S8_PLANAR:
    case AV_CODEC_ID_8SVX_RAW: esc->table = NULL;         break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Invalid codec id %d.\n", avctx->codec->id);
        return AVERROR_INVALIDDATA;
    }
    avctx->sample_fmt = AV_SAMPLE_FMT_U8P;

    avcodec_get_frame_defaults(&esc->frame);
    avctx->coded_frame = &esc->frame;

    return 0;
}

static av_cold int eightsvx_decode_close(AVCodecContext *avctx)
{
    EightSvxContext *esc = avctx->priv_data;

    av_freep(&esc->samples);
    esc->samples_size = 0;
    esc->samples_idx = 0;

    return 0;
}

#if CONFIG_EIGHTSVX_FIB_DECODER
AVCodec ff_eightsvx_fib_decoder = {
  .name           = "8svx_fib",
  .type           = AVMEDIA_TYPE_AUDIO,
  .id             = AV_CODEC_ID_8SVX_FIB,
  .priv_data_size = sizeof (EightSvxContext),
  .init           = eightsvx_decode_init,
  .decode         = eightsvx_decode_frame,
  .close          = eightsvx_decode_close,
  .capabilities   = CODEC_CAP_DR1,
  .long_name      = NULL_IF_CONFIG_SMALL("8SVX fibonacci"),
  .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_U8P,
                                                    AV_SAMPLE_FMT_NONE },
};
#endif
#if CONFIG_EIGHTSVX_EXP_DECODER
AVCodec ff_eightsvx_exp_decoder = {
  .name           = "8svx_exp",
  .type           = AVMEDIA_TYPE_AUDIO,
  .id             = AV_CODEC_ID_8SVX_EXP,
  .priv_data_size = sizeof (EightSvxContext),
  .init           = eightsvx_decode_init,
  .decode         = eightsvx_decode_frame,
  .close          = eightsvx_decode_close,
  .capabilities   = CODEC_CAP_DR1,
  .long_name      = NULL_IF_CONFIG_SMALL("8SVX exponential"),
  .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_U8P,
                                                    AV_SAMPLE_FMT_NONE },
};
#endif
#if CONFIG_PCM_S8_PLANAR_DECODER
AVCodec ff_pcm_s8_planar_decoder = {
    .name           = "pcm_s8_planar",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_PCM_S8_PLANAR,
    .priv_data_size = sizeof(EightSvxContext),
    .init           = eightsvx_decode_init,
    .close          = eightsvx_decode_close,
    .decode         = eightsvx_decode_frame,
    .capabilities   = CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("PCM signed 8-bit planar"),
    .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_U8P,
                                                      AV_SAMPLE_FMT_NONE },
};
#endif
