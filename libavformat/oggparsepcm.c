/*
 * PCM parser for Ogg
 * Copyright (c) 2013 James Almer
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

#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "internal.h"
#include "oggdec.h"

struct oggpcm_private {
    int vorbis_comment;
    uint32_t extra_headers;
};

static int ogg_pcm_get_codec_id(int format_id)
{
    switch (format_id) {
    case 0x00: return AV_CODEC_ID_PCM_S8;
    case 0x01: return AV_CODEC_ID_PCM_U8;
    case 0x02: return AV_CODEC_ID_PCM_S16LE;
    case 0x03: return AV_CODEC_ID_PCM_S16BE;
    case 0x04: return AV_CODEC_ID_PCM_S24LE;
    case 0x05: return AV_CODEC_ID_PCM_S24BE;
    case 0x06: return AV_CODEC_ID_PCM_S32LE;
    case 0x07: return AV_CODEC_ID_PCM_S32BE;
    case 0x20: return AV_CODEC_ID_PCM_F32LE;
    case 0x21: return AV_CODEC_ID_PCM_F32BE;
    case 0x22: return AV_CODEC_ID_PCM_F64LE;
    case 0x23: return AV_CODEC_ID_PCM_F64BE;
    }

    return -1;
}

static int pcm_header(AVFormatContext * s, int idx)
{
    struct ogg *ogg = s->priv_data;
    struct ogg_stream *os = ogg->streams + idx;
    struct oggpcm_private *priv = os->private;
    AVStream *st = s->streams[idx];
    uint8_t *p = os->buf + os->pstart;
    uint16_t major, minor;
    int format_id, codec_id;

    if (os->flags & OGG_FLAG_BOS) {
        if (os->psize < 28) {
            av_log(s, AV_LOG_ERROR, "Invalid OggPCM header packet");
            return -1;
        }

        major = AV_RB16(p + 8);
        minor = AV_RB16(p + 10);
        if (major) {
            av_log(s, AV_LOG_ERROR, "Unsupported OggPCM version %u.%u\n", major, minor);
            return -1;
        }

        format_id = AV_RB32(p + 12);
        if ((codec_id = ogg_pcm_get_codec_id(format_id)) < 0) {
            av_log(s, AV_LOG_ERROR, "Unsupported PCM format ID 0x%X\n", format_id);
            return -1;
        }

        priv = os->private = av_mallocz(sizeof(*priv));
        if (!priv)
            return AVERROR(ENOMEM);
        st->codec->codec_type  = AVMEDIA_TYPE_AUDIO;
        st->codec->codec_id    = codec_id;
        st->codec->sample_rate = AV_RB32(p + 16);
        st->codec->channels    = AV_RB8 (p + 21);
        priv->extra_headers    = AV_RB32(p + 24);
        priv->vorbis_comment   = 1;
        avpriv_set_pts_info(st, 64, 1, st->codec->sample_rate);
    } else if (priv && priv->vorbis_comment) {
        ff_vorbis_comment(s, &st->metadata, p, os->psize);
        priv->vorbis_comment   = 0;
    } else if (priv && priv->extra_headers) {
        // TODO: Support for channel mapping and conversion headers.
        priv->extra_headers--;
    } else
        return 0;

    return 1;
}

const struct ogg_codec ff_pcm_codec = {
    .magic     = "PCM     ",
    .magicsize = 8,
    .header    = pcm_header,
    .nb_header = 2,
};
