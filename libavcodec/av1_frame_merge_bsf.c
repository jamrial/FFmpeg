/*
 * Copyright (c) 2019 James Almer <jamrial@gmail.com>
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

#include "avcodec.h"
#include "bsf.h"
#include "cbs.h"
#include "cbs_av1.h"

typedef struct AV1FMergeContext {
    CodedBitstreamContext *cbc;
    CodedBitstreamFragment temporal_unit;
    CodedBitstreamFragment frag;
} AV1FMergeContext;

static int av1_frame_merge_filter(AVBSFContext *bsf, AVPacket *pkt)
{
    AV1FMergeContext *ctx = bsf->priv_data;
    CodedBitstreamFragment *frag = &ctx->frag, *tu = &ctx->temporal_unit;
    int err, i;

    err = ff_bsf_get_packet_ref(bsf, pkt);
    if (err < 0) {
        if (err == AVERROR_EOF && tu->nb_units > 0)
            goto eof;
        return err;
    }

    err = ff_cbs_read_packet(ctx->cbc, frag, pkt);
    if (err < 0) {
        av_log(bsf, AV_LOG_ERROR, "Failed to read packet.\n");
        goto fail;
    }

    if (frag->nb_units == 0) {
        av_log(bsf, AV_LOG_ERROR, "No OBU in packet.\n");
        err = AVERROR_INVALIDDATA;
        goto fail;
    }

    if (tu->nb_units == 0 && frag->units[0].type != AV1_OBU_TEMPORAL_DELIMITER) {
        av_log(bsf, AV_LOG_ERROR, "Missing Temporal Delimiter.\n");
        err = AVERROR_INVALIDDATA;
        goto fail;
    }

    if (tu->nb_units > 0 && frag->units[0].type == AV1_OBU_TEMPORAL_DELIMITER) {
eof:
        err = ff_cbs_write_packet(ctx->cbc, pkt, tu);
        if (err < 0) {
            av_log(bsf, AV_LOG_ERROR, "Failed to write packet.\n");
            goto fail;
        }
        ff_cbs_fragment_reset(ctx->cbc, tu);

        for (i = 0; i < frag->nb_units; i++) {
            if (i && frag->units[i].type == AV1_OBU_TEMPORAL_DELIMITER) {
                av_log(bsf, AV_LOG_ERROR, "Temporal Delimiter in the middle of a packet.\n");
                err = AVERROR_INVALIDDATA;
                goto fail;
            }
            err = ff_cbs_insert_unit_content(ctx->cbc, tu, -1, frag->units[i].type,
                                             frag->units[i].content, frag->units[i].content_ref);
            if (err < 0)
                goto fail;
        }
        ff_cbs_fragment_reset(ctx->cbc, frag);

        return err;
    }

    for (i = 0; i < frag->nb_units; i++) {
        if (i && frag->units[i].type == AV1_OBU_TEMPORAL_DELIMITER) {
            av_log(bsf, AV_LOG_ERROR, "Temporal Delimiter in the middle of a packet.\n");
            err = AVERROR_INVALIDDATA;
            goto fail;
        }
        err = ff_cbs_insert_unit_content(ctx->cbc, tu, -1, frag->units[i].type,
                                         frag->units[i].content, frag->units[i].content_ref);
        if (err < 0)
            goto fail;
    }
    ff_cbs_fragment_reset(ctx->cbc, frag);
    av_packet_unref(pkt);

    return err < 0 ? err : AVERROR(EAGAIN);

fail:
    ff_cbs_fragment_reset(ctx->cbc, tu);
    ff_cbs_fragment_reset(ctx->cbc, frag);
    av_packet_unref(pkt);

    return err;
}

static int av1_frame_merge_init(AVBSFContext *bsf)
{
    AV1FMergeContext *ctx = bsf->priv_data;
    int ret;

    ret = ff_cbs_init(&ctx->cbc, AV_CODEC_ID_AV1, bsf);
    if (ret < 0)
        return ret;

    return 0;
}

static void av1_frame_merge_flush(AVBSFContext *bsf)
{
    AV1FMergeContext *ctx = bsf->priv_data;

    ff_cbs_fragment_reset(ctx->cbc, &ctx->temporal_unit);
    ff_cbs_fragment_reset(ctx->cbc, &ctx->frag);
}

static void av1_frame_merge_close(AVBSFContext *bsf)
{
    AV1FMergeContext *ctx = bsf->priv_data;

    ff_cbs_fragment_free(ctx->cbc, &ctx->temporal_unit);
    ff_cbs_fragment_free(ctx->cbc, &ctx->frag);
    ff_cbs_close(&ctx->cbc);
}

static const enum AVCodecID av1_frame_merge_codec_ids[] = {
    AV_CODEC_ID_AV1, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_av1_frame_merge_bsf = {
    .name           = "av1_frame_merge",
    .priv_data_size = sizeof(AV1FMergeContext),
    .init           = av1_frame_merge_init,
    .flush          = av1_frame_merge_flush,
    .close          = av1_frame_merge_close,
    .filter         = av1_frame_merge_filter,
    .codec_ids      = av1_frame_merge_codec_ids,
};
