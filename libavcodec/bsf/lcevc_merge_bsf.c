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
 * Derive PTS by reordering DTS from supported streams
 */

#include "libavutil/avassert.h"
#include "libavutil/attributes.h"
#include "libavutil/fifo.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/tree.h"

#include "bsf.h"
#include "bsf_internal.h"

typedef struct LCEVCMergeContext {
    const AVClass *class;

    struct AVTreeNode *base;
    struct AVTreeNode *enhancement;
    AVFifo *nodes;

    int base_idx, enhancement_idx;
} LCEVCMergeContext;

// AVTreeNode callbacks
static int cmp_insert(const void *_key, const void *_node)
{
    const AVPacket *key = _key, *node = _node;

    return FFDIFFSIGN(key->pts, node->pts);
}

static int cmp_find(const void *_key, const void *_node)
{
    int64_t key = *(const int64_t *)_key;
    const AVPacket *node = _node;

    return FFDIFFSIGN(key, node->pts);
}

#define WARN_BUFFERED(type)                                              \
static int warn_##type##_buffered(void *logctx, void *elem)              \
{                                                                        \
    const AVPacket *pkt = (const AVPacket *)elem;                        \
    av_log(logctx, AV_LOG_WARNING, #type" packet with PTS %"PRId64       \
                                   " left buffered at EOF\n", pkt->pts); \
    return 0;                                                            \
}

WARN_BUFFERED(base)
WARN_BUFFERED(enhanced)

static int lcevc_merge_init(AVBSFContext *ctx)
{
    LCEVCMergeContext *s = ctx->priv_data;

    if (s->base_idx < 0 || s->enhancement_idx < 0) {
        av_log(ctx, AV_LOG_ERROR, "Both base and enhancement stream index must be set\n");
        return AVERROR(EINVAL);
    }

    s->nodes = av_fifo_alloc2(1, sizeof(struct AVTreeNode*), AV_FIFO_FLAG_AUTO_GROW);
    if (!s->nodes)
        return AVERROR(ENOMEM);

    return 0;
}

static int merge_packet(AVBSFContext *ctx, struct AVTreeNode **root,
                        AVPacket *out, AVPacket *in)
{
    LCEVCMergeContext *s = ctx->priv_data;
    struct AVTreeNode *node = NULL;
    uint8_t *side_data;
    int ret;

    // It doesn't matter if the packet is from the base or enhancement stream
    // as both share the pts cmp_insert() will look for to remove the element.
    av_tree_insert(root, in, cmp_insert, &node);
    memset(node, 0, av_tree_node_size);
    ret = av_fifo_write(s->nodes, &node, 1);
    if (ret < 0) {
        av_free(node);
        return ret;
    }

    side_data = av_packet_new_side_data(out, AV_PKT_DATA_LCEVC, in->size);
    if (!side_data)
        return AVERROR(ENOMEM);

    memcpy(side_data, in->data, in->size);

    return 0;
}

static int buffer_packet(AVBSFContext *ctx, struct AVTreeNode **root,
                         AVPacket **p_in)
{
    LCEVCMergeContext *s = ctx->priv_data;
    AVPacket *in = *p_in, *pkt;
    struct AVTreeNode *node = NULL;

    if (av_fifo_can_read(s->nodes)) {
        int av_unused ret = av_fifo_read(s->nodes, &node, 1);
        av_assert2(ret >= 0);
    } else
        node = av_tree_node_alloc();
    if (!node)
        return AVERROR(ENOMEM);

    pkt = av_tree_insert(root, in, cmp_insert, &node);
    if (pkt && pkt != in) {
        av_log(ctx, AV_LOG_ERROR, "Duplicate packet with PTS %"PRId64
                                  " for stream_index %d \n", in->pts, in->stream_index);
        av_free(node);
        return AVERROR_INVALIDDATA;
    }
    *p_in = NULL;

    return 0;
}

#define HANDLE_PACKET(type1, type2, pkt1, pkt2)                                       \
static int handle_##type1##_packet(AVBSFContext *ctx, AVPacket *out, AVPacket **p_in) \
{                                                                                     \
    LCEVCMergeContext *s = ctx->priv_data;                                            \
    AVPacket *in = *p_in, *pkt;                                                       \
    int ret;                                                                          \
                                                                                      \
    pkt = av_tree_find(s->type2, &in->pts, cmp_find, NULL);                           \
    if (pkt) {                                                                        \
        ret = merge_packet(ctx, &s->type2, pkt1, pkt2);                               \
        if (!ret)                                                                     \
            av_packet_move_ref(out, pkt1);                                            \
        av_packet_free(&pkt);                                                         \
        av_packet_free(p_in);                                                         \
        return ret;                                                                   \
    }                                                                                 \
                                                                                      \
    return buffer_packet(ctx, &s->type1, p_in);                                       \
}

HANDLE_PACKET(base, enhancement, in, pkt)
HANDLE_PACKET(enhancement, base, pkt, in)

static int lcevc_merge_filter(AVBSFContext *ctx, AVPacket *out)
{
    LCEVCMergeContext *s = ctx->priv_data;
    AVPacket *in;
    int ret;

    do {
        ret = ff_bsf_get_packet(ctx, &in);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                av_tree_enumerate(s->base, ctx, NULL, warn_base_buffered);
                av_tree_enumerate(s->enhancement, ctx, NULL, warn_enhanced_buffered);
            }
            return ret;
        }

        if (!in->size || in->pts < 0) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        if (in->stream_index == s->base_idx)
            ret = handle_base_packet(ctx, out, &in);
        else if (in->stream_index == s->enhancement_idx)
            ret = handle_enhancement_packet(ctx, out, &in);
        else {
            av_log(ctx, AV_LOG_ERROR, "Input packet neither base or enhacement\n");
            ret = AVERROR(EINVAL);
        }
        if (ret < 0)
            goto fail;
    } while (!out->data);

    ret = 0;
fail:
    if (ret < 0)
        av_packet_free(&in);

    return ret;
}

static int free_node(void *opaque, void *elem)
{
    AVPacket *pkt = elem;
    av_packet_free(&pkt);
    return 0;
}

static void lcevc_merge_flush(AVBSFContext *ctx)
{
    LCEVCMergeContext *s = ctx->priv_data;

    av_tree_enumerate(s->base, NULL, NULL, free_node);
    av_tree_enumerate(s->enhancement, NULL, NULL, free_node);
    av_tree_destroy(s->base);
    av_tree_destroy(s->enhancement);
    s->base = NULL;
    s->enhancement = NULL;
}

static void lcevc_merge_close(AVBSFContext *ctx)
{
    LCEVCMergeContext *s = ctx->priv_data;

    lcevc_merge_flush(ctx);

    if (s->nodes) {
        struct AVTreeNode *node;
        while (av_fifo_read(s->nodes, &node, 1) >= 0)
            av_free(node);
    }

    av_fifo_freep2(&s->nodes);
}

#define OFFSET(x) offsetof(LCEVCMergeContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_BSF_PARAM)
static const AVOption lcevc_merge_options[] = {
    { "base_idx", NULL, OFFSET(base_idx), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, FLAGS },
    { "enhancement_idx", NULL, OFFSET(enhancement_idx), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, FLAGS },
    { NULL }
};

static const AVClass lcevc_merge_class = {
    .class_name = "lcevc_merge_bsf",
    .item_name  = av_default_item_name,
    .option     = lcevc_merge_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFBitStreamFilter ff_lcevc_merge_bsf = {
    .p.name         = "lcevc_merge",
    .p.priv_class   = &lcevc_merge_class,
    .priv_data_size = sizeof(LCEVCMergeContext),
    .init           = lcevc_merge_init,
    .flush          = lcevc_merge_flush,
    .close          = lcevc_merge_close,
    .filter         = lcevc_merge_filter,
};
