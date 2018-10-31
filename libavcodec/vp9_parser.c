/*
 * VP9 parser
 *
 * Copyright (C) 2018 James Almer <jamrial@gmail.com>
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

#include "cbs.h"
#include "cbs_vp9.h"
#include "parser.h"

typedef struct VP9ParseContext {
    CodedBitstreamContext *cbc;
    CodedBitstreamFragment temporal_unit;
} VP9ParseContext;

static const enum AVPixelFormat pix_fmts_8bit[2][2] = {
    { AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV440P },
    { AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P },
};
static const enum AVPixelFormat pix_fmts_10bit[2][2] = {
    { AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV440P },
    { AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV420P10 },
};
static const enum AVPixelFormat pix_fmts_12bit[2][2] = {
    { AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV440P },
    { AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12 },
};

static int vp9_parser_parse(AVCodecParserContext *ctx,
                            AVCodecContext *avctx,
                            const uint8_t **out_data, int *out_size,
                            const uint8_t *data, int size)
{
    VP9ParseContext *s = ctx->priv_data;
    CodedBitstreamFragment *td = &s->temporal_unit;
    CodedBitstreamVP9Context *vp9 = s->cbc->priv_data;
    int ret;

    *out_data = data;
    *out_size = size;

    ctx->key_frame         = -1;
    ctx->pict_type         = AV_PICTURE_TYPE_NONE;
    ctx->picture_structure = AV_PICTURE_STRUCTURE_UNKNOWN;

    s->cbc->log_ctx = avctx;

    ret = ff_cbs_read(s->cbc, td, data, size);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to parse temporal unit.\n");
        goto end;
    }
    for (int i = 0; i < td->nb_units; i++) {
        CodedBitstreamUnit *unit = &td->units[i];
        VP9RawFrame *frame = unit->content;
        VP9RawFrameHeader *hdr = &frame->header;
        int subsampling_x, subsampling_y, bit_depth, intra_only;

        if (hdr->show_existing_frame) {
            VP9ReferenceFrameState *ref = &vp9->ref[hdr->frame_to_show_map_idx];

            ctx->width  = ref->frame_width;
            ctx->height = ref->frame_height;

            subsampling_x = ref->subsampling_x;
            subsampling_y = ref->subsampling_y;
            bit_depth     = ref->bit_depth;
            intra_only    = ref->intra_only;

            ctx->key_frame = 0;
        } else if (!hdr->show_frame) {
            continue;
        } else {
            ctx->width  = vp9->frame_width;
            ctx->height = vp9->frame_height;

            subsampling_x = vp9->subsampling_x;
            subsampling_y = vp9->subsampling_y;
            bit_depth     = vp9->bit_depth;
            intra_only    = 0;

            ctx->key_frame = !hdr->frame_type;
        }

        avctx->profile = vp9->profile;

        ctx->pict_type = (ctx->key_frame || intra_only) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
        ctx->picture_structure = AV_PICTURE_STRUCTURE_FRAME;

        switch (bit_depth) {
        case 8:
            ctx->format = pix_fmts_8bit [subsampling_x][subsampling_y];
            break;
        case 10:
            ctx->format = pix_fmts_10bit[subsampling_x][subsampling_y];
            break;
        case 12:
            ctx->format = pix_fmts_12bit[subsampling_x][subsampling_y];
            break;
        }
    }

end:
    ff_cbs_fragment_uninit(s->cbc, td);

    s->cbc->log_ctx = NULL;

    return size;
}

static av_cold int vp9_parser_init(AVCodecParserContext *ctx)
{
    VP9ParseContext *s = ctx->priv_data;
    int ret;

    ret = ff_cbs_init(&s->cbc, AV_CODEC_ID_VP9, NULL);
    if (ret < 0)
        return ret;

    return 0;
}

static void vp9_parser_close(AVCodecParserContext *ctx)
{
    VP9ParseContext *s = ctx->priv_data;

    ff_cbs_close(&s->cbc);
}

AVCodecParser ff_vp9_parser = {
    .codec_ids      = { AV_CODEC_ID_VP9 },
    .priv_data_size = sizeof(VP9ParseContext),
    .parser_init    = vp9_parser_init,
    .parser_close   = vp9_parser_close,
    .parser_parse   = vp9_parser_parse,
};
