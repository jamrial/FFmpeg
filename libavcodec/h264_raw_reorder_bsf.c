/*
 * Copyright (c) 2022 James Almer
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
 * Derive PTS by reordering DTS from H.264 streams
 */

#include "libavutil/avassert.h"
#include "libavutil/eval.h"
#include "libavutil/fifo.h"
#include "libavutil/opt.h"
#include "libavutil/tree.h"

#include "bsf.h"
#include "bsf_internal.h"
#include "cbs.h"
#include "cbs_h264.h"
#include "h264_parse.h"
#include "h264_ps.h"

typedef struct H264RawReorderNode {
    int64_t      dts;
    int          poc;
} H264RawReorderNode;

typedef struct H264RawReorderFrame {
    AVPacket    *pkt;
    int          poc;
} H264RawReorderFrame;

typedef struct H264RawReorderContext {
    CodedBitstreamContext *cbc;
    struct AVTreeNode *root;
    AVFifo *fifo;
    CodedBitstreamFragment au;
    H264POCContext poc;
    SPS sps;
    int eof;
    int nb_frame;
    int last_poc;
    int highest_poc;
    int poc_diff;
} H264RawReorderContext;

static const CodedBitstreamUnitType decompose_unit_types[] = {
    H264_NAL_SPS,
    H264_NAL_PPS,
    H264_NAL_IDR_SLICE,
    H264_NAL_SLICE,
};

static int h264_raw_reorder_init(AVBSFContext *ctx)
{
    H264RawReorderContext *s = ctx->priv_data;
    CodedBitstreamFragment *au = &s->au;
    int ret;

    // Don't include delayed frames on the POC tree
    s->nb_frame = -(ctx->par_in->video_delay << (ctx->par_in->field_order != AV_FIELD_PROGRESSIVE));
    s->fifo = av_fifo_alloc2(H264_MAX_DPB_FRAMES, sizeof(H264RawReorderFrame), 0);
    if (!s->fifo)
        return AVERROR(ENOMEM);

    ret = ff_cbs_init(&s->cbc, AV_CODEC_ID_H264, ctx);
    if (ret < 0)
        return ret;

    s->cbc->decompose_unit_types    = decompose_unit_types;
    s->cbc->nb_decompose_unit_types = FF_ARRAY_ELEMS(decompose_unit_types);

    if (!ctx->par_in->extradata_size)
        return 0;

    ret = ff_cbs_read_extradata(s->cbc, au, ctx->par_in);
    if (ret < 0)
        av_log(ctx, AV_LOG_WARNING, "Failed to parse extradata.\n");

    ff_cbs_fragment_reset(au);

    return 0;
}

static int get_mmco_reset(const H264RawSliceHeader *header)
{
    if (header->nal_unit_header.nal_ref_idc == 0 || !header->adaptive_ref_pic_marking_mode_flag)
        return 0;

    for (int i = 0; i < H264_MAX_MMCO_COUNT; i++) {
        if (header->mmco[i].memory_management_control_operation == 0)
            return 0;
        else if (header->mmco[i].memory_management_control_operation == 5)
            return 1;
    }

    return 0;
}

static int cmp_insert(const void *key, const void *node)
{
    return ((const H264RawReorderNode *) key)->poc - ((const H264RawReorderNode *) node)->poc;
}

static int h264_raw_reorder_queue_packet(AVBSFContext *ctx)
{
    H264RawReorderContext *s = ctx->priv_data;
    const CodedBitstreamH264Context *h264 = s->cbc->priv_data;
    CodedBitstreamFragment *au = &s->au;
    H264RawReorderFrame frame;
    AVPacket *in;
    int picture_structure, output_picture_number = -1;
    int field_poc[2];
    int ret;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    ret = ff_cbs_read_packet(s->cbc, au, in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Failed to parse access unit.\n");
        return ret;
    }

    for (int i = 0; i < au->nb_units; i++) {
        CodedBitstreamUnit *unit = &au->units[i];

        switch (unit->type) {
        case H264_NAL_IDR_SLICE:
            s->poc.prev_frame_num        = 0;
            s->poc.prev_frame_num_offset = 0;
            s->poc.prev_poc_msb          =
            s->poc.prev_poc_lsb          = 0;
        // fall-through
        case H264_NAL_SLICE: {
            const H264RawSlice *slice = unit->content;
            const H264RawSliceHeader *header = &slice->header;
            const H264RawSPS *sps = h264->active_sps;
            int got_reset;

            // Initialize the SPS struct with the fields ff_h264_init_poc() cares about
            s->sps.log2_max_frame_num             = sps->log2_max_frame_num_minus4 + 4;
            s->sps.poc_type                       = sps->pic_order_cnt_type;
            s->sps.log2_max_poc_lsb               = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
            s->sps.offset_for_non_ref_pic         = sps->offset_for_non_ref_pic;
            s->sps.offset_for_top_to_bottom_field = sps->offset_for_top_to_bottom_field;
            s->sps.poc_cycle_length               = sps->num_ref_frames_in_pic_order_cnt_cycle;
            for (int i = 0; i < s->sps.poc_cycle_length; i++)
                s->sps.offset_for_ref_frame[i] = sps->offset_for_ref_frame[i];

            picture_structure = sps->frame_mbs_only_flag ? 3 :
                                header->field_pic_flag + header->bottom_field_flag;

            s->poc.frame_num = header->frame_num;
            s->poc.poc_lsb = header->pic_order_cnt_lsb;
            s->poc.delta_poc_bottom = header->delta_pic_order_cnt_bottom;
            s->poc.delta_poc[0] = header->delta_pic_order_cnt[0];
            s->poc.delta_poc[1] = header->delta_pic_order_cnt[1];

            field_poc[0] = field_poc[1] = INT_MAX;
            ret = ff_h264_init_poc(field_poc, &output_picture_number, &s->sps,
                                   &s->poc, picture_structure,
                                   header->nal_unit_header.nal_ref_idc);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "ff_h264_init_poc() failure\n");
                goto fail;
            }

            got_reset = get_mmco_reset(header);
            s->poc.prev_frame_num        = got_reset ? 0 : s->poc.frame_num;
            s->poc.prev_frame_num_offset = got_reset ? 0 : s->poc.frame_num_offset;
            if (header->nal_unit_header.nal_ref_idc != 0) {
                s->poc.prev_poc_msb      = got_reset ? 0 : s->poc.poc_msb;
                if (got_reset)
                    s->poc.prev_poc_lsb = picture_structure == 2 ? 0 : field_poc[0];
                else
                    s->poc.prev_poc_lsb = s->poc.poc_lsb;
            }

            // Calculate the difference between POC values, and store the highest POC value found
            if (output_picture_number != s->last_poc) {
				int pdiff = FFABS(s->last_poc - output_picture_number);

				if (!s->poc_diff || (s->poc_diff > pdiff))
					s->poc_diff = pdiff;
				s->last_poc = output_picture_number;
				s->highest_poc = FFMAX(s->highest_poc, output_picture_number);
            }
            break;
        }
        default:
            break;
        }
    }

    if (output_picture_number < 0) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    // Add the packet's dts to POC tree if needed
    if (s->nb_frame >= 0) {
        H264RawReorderNode *poc_node;
        struct AVTreeNode *node = av_tree_node_alloc();
        if (!node) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        poc_node = av_malloc(sizeof(*poc_node));
        if (!poc_node) {
            av_free(node);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        // Check if there was a POC reset (Like an IDR slice)
        if (s->nb_frame > s->highest_poc / FFMAX(s->poc_diff, 1)) {
            s->nb_frame = 0;
            s->highest_poc = s->last_poc;
        }
        *poc_node = (H264RawReorderNode) { in->dts, s->nb_frame };
        av_tree_insert(&s->root, poc_node, cmp_insert, &node);
    }
    av_log(ctx, AV_LOG_DEBUG, "Queueing frame with POC %d, dts %"PRId64"\n",
           output_picture_number, in->dts);
    s->nb_frame++;

    // Add packet to output FIFO
    frame = (H264RawReorderFrame) { in, output_picture_number };
    ret = av_fifo_write(s->fifo, &frame, 1);
    av_assert2(ret >= 0);
    in = NULL;

    ret = AVERROR(EAGAIN);
fail:
    ff_cbs_fragment_reset(au);
    if (ret < 0)
        av_packet_free(&in);

    return ret;
}

static int cmp_find(const void *key, const void *node)
{
    return *(const int *)key - ((const H264RawReorderNode *) node)->poc;
}

static int h264_raw_reorder_filter(AVBSFContext *ctx, AVPacket *out)
{
    H264RawReorderContext *s = ctx->priv_data;
    struct AVTreeNode *node = NULL;
    H264RawReorderNode *poc_node = NULL, *next[2] = { NULL, NULL };
    H264RawReorderFrame frame;
    int poc, ret;

    // Fill up the FIFO and POC tree with MAX_DPB_FRAMES packets
    if (!s->eof && av_fifo_can_write(s->fifo)) {
        ret = h264_raw_reorder_queue_packet(ctx);
        if (ret != AVERROR_EOF)
            return ret;
        s->eof = 1;
    }

    if (!av_fifo_can_read(s->fifo))
        return AVERROR_EOF;

    // Fetch a packet from the FIFO
    ret = av_fifo_read(s->fifo, &frame, 1);
    av_assert2(ret >= 0);
    av_packet_move_ref(out, frame.pkt);
    av_packet_free(&frame.pkt);

    // Search the timestamp for the requested POC and set PTS
    poc = frame.poc / FFMAX(s->poc_diff, 1);
    poc_node = av_tree_find(s->root, &poc, cmp_find, (void **)next);
    if (poc_node) {
        // Remove the found entry from the tree
        av_tree_insert(&s->root, poc_node, cmp_insert, &node);
        out->pts = poc_node->dts;
        av_free(poc_node);
        av_free(node);
    } else
        av_log(ctx, AV_LOG_WARNING, "No timestamp for POC %d in tree\n", frame.poc);
    av_log(ctx, AV_LOG_DEBUG, "Returning frame with POC %d, dts %"PRId64", pts %"PRId64"\n",
           frame.poc, out->dts, out->pts);

    return 0;
}

static int free_node(void *opaque, void *elem)
{
    H264RawReorderNode *node = elem;
    av_free(node);
    return 0;
}

static void h264_raw_reorder_flush(AVBSFContext *ctx)
{
    H264RawReorderContext *s = ctx->priv_data;
    H264RawReorderFrame frame;

    memset(&s->sps, 0, sizeof(s->sps));
    memset(&s->poc, 0, sizeof(s->poc));
    s->nb_frame = -(ctx->par_out->video_delay << (ctx->par_out->field_order != AV_FIELD_PROGRESSIVE));
    s->poc_diff = s->last_poc = s->highest_poc = 0;
    s->eof = 0;

    while (av_fifo_can_read(s->fifo)) {
        av_fifo_read(s->fifo, &frame, 1);
        av_packet_free(&frame.pkt);
    }

    av_tree_enumerate(s->root, NULL, NULL, free_node);
    av_tree_destroy(s->root);

    ff_cbs_fragment_reset(&s->au);
    ff_cbs_flush(s->cbc);
}

static void h264_raw_reorder_close(AVBSFContext *ctx)
{
    H264RawReorderContext *s = ctx->priv_data;

    h264_raw_reorder_flush(ctx);

    av_fifo_freep2(&s->fifo);
    ff_cbs_fragment_free(&s->au);
    ff_cbs_close(&s->cbc);
}

static const enum AVCodecID h264_raw_reorder_codec_ids[] = {
    AV_CODEC_ID_H264, AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_h264_raw_reorder_bsf = {
    .p.name         = "h264_raw_reorder",
    .p.codec_ids    = h264_raw_reorder_codec_ids,
    .priv_data_size = sizeof(H264RawReorderContext),
    .init           = h264_raw_reorder_init,
    .flush          = h264_raw_reorder_flush,
    .close          = h264_raw_reorder_close,
    .filter         = h264_raw_reorder_filter,
};
