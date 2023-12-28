/*
 * Copyright (c) 2024 James Almer <jamrial@gmail.com>
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
#include <stddef.h>

#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/opt.h"
#include "libavformat/iamf.h"
#include "bsf.h"
#include "bsf_internal.h"
#include "bytestream.h"
#include "get_bits.h"
#include "leb.h"
#include "put_bits.h"

typedef struct IAMFMergeContext {
    AVClass *class;

    AVFifo *fifo;

    // AVOptions
    AVDictionary *index_mapping;
    int stream_count;
    int out_index;
} IAMFMergeContext;

static int find_id_from_idx(AVBSFContext *ctx, int idx)
{
    IAMFMergeContext *const c = ctx->priv_data;
    const AVDictionaryEntry *e = NULL;

    while (e = av_dict_iterate(c->index_mapping, e)) {
        char *endptr = NULL;
        int id, map_idx = strtol(e->key, &endptr, 0);
        if (!endptr || *endptr)
            return AVERROR_INVALIDDATA;
        endptr = NULL;
        id = strtol(e->value, &endptr, 0);
        if (!endptr || *endptr)
            return AVERROR_INVALIDDATA;
        if (map_idx == idx)
            return id;
    }

    av_log(ctx, AV_LOG_ERROR, "Invalid stream idx %d\n", idx);
    return AVERROR_INVALIDDATA;
}

static int iamf_frame_merge_filter(AVBSFContext *ctx, AVPacket *out)
{
    IAMFMergeContext *const c = ctx->priv_data;
    AVPacket *pkt;
    int ret;

    while (av_fifo_can_write(c->fifo)) {
        ret = ff_bsf_get_packet(ctx, &pkt);
        if (ret < 0)
            return ret;
        av_fifo_write(c->fifo, &pkt, 1);
    }

    pkt = NULL;
    while (av_fifo_can_read(c->fifo)) {
        PutBitContext pb;
        PutByteContext p;
        uint8_t *side_data, header[MAX_IAMF_OBU_HEADER_SIZE], obu[8];
        unsigned int obu_header;
        unsigned int skip_samples = 0, discard_padding = 0;
        size_t side_data_size;
        int header_size, obu_size, old_out_size = out->size;
        int id, type;

        av_packet_free(&pkt);
        av_fifo_read(c->fifo, &pkt, 1);
        id = find_id_from_idx(ctx, pkt->stream_index);
        if (id < 0)
            return AVERROR_INVALIDDATA;

        type = id <= 17 ? id + IAMF_OBU_IA_AUDIO_FRAME_ID0 : IAMF_OBU_IA_AUDIO_FRAME;

        side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES,
                                            &side_data_size);

        if (side_data && side_data_size >= 10) {
            skip_samples = AV_RL32(side_data);
            discard_padding = AV_RL32(side_data + 4);
        }

        init_put_bits(&pb, (uint8_t *)&obu_header, sizeof(obu_header));
        put_bits(&pb, 5, type);
        put_bits(&pb, 1, 0); // obu_redundant_copy
        put_bits(&pb, 1, skip_samples || discard_padding);
        put_bits(&pb, 1, 0); // obu_extension_flag
        flush_put_bits(&pb);

        init_put_bits(&pb, header, sizeof(header));
        if (skip_samples || discard_padding) {
            put_leb(&pb, discard_padding);
            put_leb(&pb, skip_samples);
        }
        if (id > 17)
            put_leb(&pb, id);
        flush_put_bits(&pb);

        header_size = put_bytes_count(&pb, 1);

        init_put_bits(&pb, obu, sizeof(obu));
        put_leb(&pb, header_size + pkt->size);
        flush_put_bits(&pb);

        obu_size = put_bytes_count(&pb, 1);

        ret = av_grow_packet(out, 1 + obu_size + header_size + pkt->size);
        if (ret < 0)
            goto fail;

        bytestream2_init_writer(&p, out->data + old_out_size, 1 + obu_size + header_size + pkt->size);
        bytestream2_put_byteu(&p, obu_header);
        bytestream2_put_bufferu(&p, obu, obu_size);
        bytestream2_put_bufferu(&p, header, header_size);
        bytestream2_put_bufferu(&p, pkt->data, pkt->size);
    }

    ret = av_packet_copy_props(out, pkt);
    if (ret < 0)
        goto fail;
    out->stream_index = c->out_index;

    ret = 0;
fail:
    av_packet_free(&pkt);
    if (ret < 0)
        av_packet_free(&out);
    return ret;
}

static int iamf_frame_merge_init(AVBSFContext *ctx)
{
    IAMFMergeContext *const c = ctx->priv_data;

    if (!c->index_mapping) {
        av_log(ctx, AV_LOG_ERROR, "Empty index map\n");
        return AVERROR(EINVAL);
    }

    c->fifo = av_fifo_alloc2(av_dict_count(c->index_mapping), sizeof(AVPacket*), 0);
    if (!c->fifo)
        return AVERROR(ENOMEM);

    return 0;
}

static void iamf_frame_merge_flush(AVBSFContext *ctx)
{
    IAMFMergeContext *const c = ctx->priv_data;

    while (av_fifo_can_read(c->fifo)) {
        AVPacket *pkt;
        av_fifo_read(c->fifo, &pkt, 1);
        av_packet_free(&pkt);
    }
    av_fifo_reset2(c->fifo);
}

static void iamf_frame_merge_close(AVBSFContext *ctx)
{
    IAMFMergeContext *const c = ctx->priv_data;

    if (c->fifo)
        iamf_frame_merge_flush(ctx);
    av_fifo_freep2(&c->fifo);
}

#define OFFSET(x) offsetof(IAMFMergeContext, x)
#define FLAGS (AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_BSF_PARAM)
static const AVOption iamf_frame_merge_options[] = {
    { "index_mapping", "a :-separated list of stream_index=audio_substream_id entries "
                       "to set stream id in output Audio Frame OBUs",
        OFFSET(index_mapping), AV_OPT_TYPE_DICT, { .str = NULL }, 0, 0, FLAGS },
    { "out_index", "Stream index to set in output packets",
        OFFSET(out_index), AV_OPT_TYPE_INT, { 0 }, 0, INT_MAX, FLAGS },
    { NULL }
};

static const AVClass iamf_frame_merge_class = {
    .class_name = "iamf_frame_merge_bsf",
    .item_name  = av_default_item_name,
    .option     = iamf_frame_merge_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const enum AVCodecID iamf_frame_merge_codec_ids[] = {
    AV_CODEC_ID_PCM_S16LE, AV_CODEC_ID_PCM_S16BE,
    AV_CODEC_ID_PCM_S24LE, AV_CODEC_ID_PCM_S24BE,
    AV_CODEC_ID_PCM_S32LE, AV_CODEC_ID_PCM_S32BE,
    AV_CODEC_ID_OPUS,      AV_CODEC_ID_AAC,
    AV_CODEC_ID_FLAC,      AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_iamf_frame_merge_bsf = {
    .p.name         = "iamf_frame_merge",
    .p.codec_ids    = iamf_frame_merge_codec_ids,
    .p.priv_class   = &iamf_frame_merge_class,
    .priv_data_size = sizeof(IAMFMergeContext),
    .init           = iamf_frame_merge_init,
    .flush          = iamf_frame_merge_flush,
    .close          = iamf_frame_merge_close,
    .filter         = iamf_frame_merge_filter,
};
