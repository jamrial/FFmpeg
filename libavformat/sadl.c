/*
 * SADL demuxer
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

#include "libavcodec/bytestream.h"
#include "libavutil/intreadwrite.h"
#include "avio.h"
#include "avformat.h"
#include "internal.h"

static int sadl_probe(AVProbeData *p)
{
    if (AV_RB32(p->buf) == MKBETAG('s','a','d','l') &&
        (p->buf[51] & 0xf0) && (p->buf[51] & 6))
        return AVPROBE_SCORE_MAX / 3 * 2;
    return 0;
}

static int sadl_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    int codec_id, start = 0x100;
    AVCodecContext *codec;
    AVStream *st = avformat_new_stream(s, NULL);

    if (!st)
        return AVERROR(ENOMEM);

    avio_skip(pb, 50);

    codec = st->codec;
    codec->codec_type = AVMEDIA_TYPE_AUDIO;
    codec->channels = avio_r8(pb);
    if (!codec->channels)
        return AVERROR_INVALIDDATA;

    codec_id = avio_r8(pb);
    switch (codec_id & 0xf0) {
    case 0x70:
        codec->codec_id = AV_CODEC_ID_ADPCM_IMA_SADL;
        break;
    default:
        avpriv_request_sample(s, "Codec id: %d", codec_id & 0xf0);
        return AVERROR_PATCHWELCOME;
    }

    switch (codec_id & 6) {
    case 2:
        codec->sample_rate = 16364;
        break;
    case 4:
        codec->sample_rate = 32728;
        break;
    default:
        return AVERROR_INVALIDDATA;
    }

    avio_skip(pb, 12); // Unknown

    st->duration = (avio_rl32(pb) - start) / codec->channels * 2;
    st->start_time         = 0;
    avio_skip(pb, start - avio_tell(pb));

    avpriv_set_pts_info(st, 64, 1, codec->sample_rate);

    return 0;
}

#define SADL_BUFSIZE 1024
static int sadl_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int i, ret, size = SADL_BUFSIZE;
    uint8_t buf[SADL_BUFSIZE], *dst;

    if (url_feof(s->pb))
        return AVERROR_EOF;

    if (av_new_packet(pkt, size) < 0)
        return AVERROR(ENOMEM);
    dst = pkt->data;

    ret = avio_read(s->pb, buf, size);
    if (ret < 0) {
       av_free_packet(pkt);
       return ret;
    }

    for (i = 0; i < size / 32; i++) {
        int j;

        for (j = 0; j < 16; j++) {
            bytestream_put_byte(&dst, buf[i*32+j]);
            bytestream_put_byte(&dst, buf[i*32+j+16]);
        }
    }

    if (ret != size)
        av_shrink_packet(pkt, ret);

    pkt->stream_index = 0;

    return ret;
}

AVInputFormat ff_sadl_demuxer = {
    .name           =   "sadl",
    .long_name      =   NULL_IF_CONFIG_SMALL("SADL"),
    .read_probe     =   sadl_probe,
    .read_header    =   sadl_read_header,
    .read_packet    =   sadl_read_packet,
    .extensions     =   "sad",
};
