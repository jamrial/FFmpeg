/*
 * librav1e encoder
 *
 * Copyright (c) 2019 Derek Buitenhuis
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

#include <rav1e.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avcodec.h"
#include "internal.h"

typedef struct librav1eContext {
    const AVClass *class;

    RaContext *ctx;
    AVBSFContext *bsf;
    RaFrame *rframe;
    char *rav1e_opts;
    int max_quantizer;
    int quantizer;
} librav1eContext;

static inline RaPixelRange range_map(enum AVColorRange range)

{
    switch (range) {
    case AVCOL_RANGE_MPEG:
        return RA_PIXEL_RANGE_LIMITED;
    case AVCOL_RANGE_JPEG:
        return RA_PIXEL_RANGE_FULL;
    default:
        return RA_PIXEL_RANGE_UNSPECIFIED;
    }
}

static inline RaChromaSampling pix_fmt_map(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV420P12:
        return RA_CHROMA_SAMPLING_CS420;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUV422P12:
        return RA_CHROMA_SAMPLING_CS422;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUV444P12:
        return RA_CHROMA_SAMPLING_CS444;
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_GRAY10:
    case AV_PIX_FMT_GRAY12:
        return RA_CHROMA_SAMPLING_CS400;
    default:
        // This should be impossible
        return (RaChromaSampling) -1;
    }
}

static inline RaChromaSamplePosition chroma_loc_map(enum AVChromaLocation chroma_loc)
{
    switch (chroma_loc) {
    case AVCHROMA_LOC_LEFT:
        return RA_CHROMA_SAMPLE_POSITION_VERTICAL;
    case AVCHROMA_LOC_TOPLEFT:
        return RA_CHROMA_SAMPLE_POSITION_COLOCATED;
    default:
        return RA_CHROMA_SAMPLE_POSITION_UNKNOWN;
    }
}

static av_cold int librav1e_encode_close(AVCodecContext *avctx)
{
    librav1eContext *ctx = avctx->priv_data;

    if (ctx->ctx) {
        rav1e_context_unref(ctx->ctx);
        ctx->ctx = NULL;
    }

    av_bsf_free(&ctx->bsf);

    return 0;
}

static av_cold int librav1e_encode_init(AVCodecContext *avctx)
{
    librav1eContext *ctx = avctx->priv_data;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    RaConfig *cfg = NULL;
    int rret;
    int ret = 0;

    cfg = rav1e_config_default();
    if (!cfg) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate rav1e config.\n");
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
         const AVBitStreamFilter *filter = av_bsf_get_by_name("extract_extradata");
         int bret;

         if (!filter) {
            av_log(avctx, AV_LOG_ERROR, "extract_extradata bitstream filter "
                   "not found. This is a bug, please report it.\n");
            ret = AVERROR_BUG;
            goto end;
         }

         bret = av_bsf_alloc(filter, &ctx->bsf);
         if (bret < 0) {
             ret = bret;
             goto end;
         }

         bret = avcodec_parameters_from_context(ctx->bsf->par_in, avctx);
         if (bret < 0) {
             ret = bret;
             goto end;
         }

         bret = av_bsf_init(ctx->bsf);
         if (bret < 0) {
             ret = bret;
             goto end;
         }
    }

    if (ctx->rav1e_opts) {
        AVDictionary *dict    = NULL;
        AVDictionaryEntry *en = NULL;

        if (!av_dict_parse_string(&dict, ctx->rav1e_opts, "=", ":", 0)) {
            while (en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)) {
                int parse_ret = rav1e_config_parse(cfg, en->key, en->value);
                if (parse_ret < 0)
                    av_log(avctx, AV_LOG_WARNING, "Invalid value for %s: %s.\n", en->key, en->value);
            }
            av_dict_free(&dict);
        }
    }

    rret = rav1e_config_parse_int(cfg, "width", avctx->width);
    if (rret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid width passed to rav1e.\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rret = rav1e_config_parse_int(cfg, "height", avctx->height);
    if (rret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid width passed to rav1e.\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rret = rav1e_config_parse_int(cfg, "threads", avctx->thread_count);
    if (rret < 0)
        av_log(avctx, AV_LOG_WARNING, "Invalid number of threads, defaulting to auto.\n");

    if (avctx->bit_rate && ctx->quantizer < 0) {
        rret = rav1e_config_parse_int(cfg, "quantizer", ctx->max_quantizer);
        if (rret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not set max quantizer.\n");
            ret = AVERROR_EXTERNAL;
            goto end;
        }
        rret = rav1e_config_parse_int(cfg, "bitrate", avctx->bit_rate);
        if (rret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not set bitrate.\n");
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    } else if (ctx->quantizer >= 0) {
        rret = rav1e_config_parse_int(cfg, "quantizer", ctx->quantizer);
        if (rret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not set quantizer.\n");
            ret = AVERROR_EXTERNAL;
            goto end;
        }
    }

    rret = rav1e_config_set_pixel_format(cfg, desc->comp[0].depth,
                                         pix_fmt_map(avctx->pix_fmt),
                                         chroma_loc_map(avctx->chroma_sample_location),
                                         range_map(avctx->color_range));
    if (rret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set pixel format properties.\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /* rav1e's colorspace enums match standard values. */
    rret = rav1e_config_set_color_description(cfg, (RaMatrixCoefficients) avctx->colorspace,
                                              (RaColorPrimaries) avctx->color_primaries,
                                              (RaTransferCharacteristics) avctx->color_trc);
    if (rret < 0) {
        av_log(avctx, AV_LOG_WARNING, "Failed to set color properties.\n");
        if (avctx->err_recognition & AV_EF_EXPLODE) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }

    ctx->ctx = rav1e_context_new(cfg);
    if (!ctx->ctx) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create rav1e encode context.\n");
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    ret = 0;

end:
    if (cfg)
        rav1e_config_unref(cfg);

    if (ret)
        librav1e_encode_close(avctx);

    return ret;
}

static int librav1e_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    librav1eContext *ctx = avctx->priv_data;
    int ret;

    if (!ctx->rframe && frame) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);

        ctx->rframe = rav1e_frame_new(ctx->ctx);
        if (!ctx->rframe) {
            av_log(avctx, AV_LOG_ERROR, "Could not allocate new rav1e frame.\n");
            return AVERROR(ENOMEM);
        }

        for (int i = 0; i < 3; i++) {
            int shift = i ? desc->log2_chroma_h : 0;
            int bytes = desc->comp[0].depth == 8 ? 1 : 2;
            rav1e_frame_fill_plane(ctx->rframe, i, frame->data[i],
                                   (frame->height >> shift) * frame->linesize[i],
                                   frame->linesize[i], bytes);
        }
    }

    ret = rav1e_send_frame(ctx->ctx, ctx->rframe);
    if (ctx->rframe && ret != RA_ENCODER_STATUS_ENOUGH_DATA) {
         rav1e_frame_unref(ctx->rframe); /* No need to unref if flushing. */
         ctx->rframe = NULL;
    }
    switch(ret) {
    case RA_ENCODER_STATUS_SUCCESS:
        break;
    case RA_ENCODER_STATUS_ENOUGH_DATA:
        return AVERROR(EAGAIN);
    case RA_ENCODER_STATUS_FAILURE:
        av_log(avctx, AV_LOG_ERROR, "Could not send frame.\n");
        return AVERROR_EXTERNAL;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown return code from rav1e_send_frame.\n");
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static void librav1e_packet_unref(void *opaque, uint8_t *data)
{
    RaPacket *rpkt = opaque;

    rav1e_packet_unref(rpkt);
}

static int librav1e_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    librav1eContext *ctx = avctx->priv_data;
    RaPacket *rpkt = NULL;
    int ret;

retry:
    ret = rav1e_receive_packet(ctx->ctx, &rpkt);
    switch(ret) {
    case RA_ENCODER_STATUS_SUCCESS:
        break;
    case RA_ENCODER_STATUS_LIMIT_REACHED:
        return AVERROR_EOF;
    case RA_ENCODER_STATUS_ENCODED:
        if (avctx->internal->draining)
            goto retry;
        return AVERROR(EAGAIN);
    case RA_ENCODER_STATUS_NEED_MORE_DATA:
        if (avctx->internal->draining) {
            av_log(avctx, AV_LOG_ERROR, "Unexpected error when receiving packet after EOF.\n");
            return AVERROR_EXTERNAL;
        }
        return AVERROR(EAGAIN);
    case RA_ENCODER_STATUS_FAILURE:
        av_log(avctx, AV_LOG_ERROR, "Could not encode frame.\n");
        return AVERROR_EXTERNAL;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown return code %d from rav1e_receive_packet.\n", ret);
        return AVERROR_UNKNOWN;
    }

    pkt->buf = av_buffer_create((uint8_t *)rpkt->data, rpkt->len, librav1e_packet_unref,
                                rpkt, AV_BUFFER_FLAG_READONLY);
    if (!pkt->buf) {
        rav1e_packet_unref(rpkt);
        return AVERROR(ENOMEM);
    }

    pkt->data = (uint8_t *)rpkt->data;
    pkt->size = rpkt->len;

    if (rpkt->frame_type == RA_FRAME_TYPE_KEY)
        pkt->flags |= AV_PKT_FLAG_KEY;

    pkt->pts = pkt->dts = rpkt->number * avctx->ticks_per_frame;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        int ret = av_bsf_send_packet(ctx->bsf, pkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "extradata extraction send failed.\n");
            av_packet_unref(pkt);
            return ret;
        }

        ret = av_bsf_receive_packet(ctx->bsf, pkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "extradata extraction receive failed.\n");
            av_packet_unref(pkt);
            return ret;
        }
    }

    return 0;
}

#define OFFSET(x) offsetof(librav1eContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "quantizer", "use constant quantizer mode", OFFSET(quantizer), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 255, VE },
    { "max-quantizer", "max quantizer when using bitrate mode", OFFSET(max_quantizer), AV_OPT_TYPE_INT, { .i64 = 255 }, 1, 255, VE },
    { "rav1e-params", "set the rav1e configuration using a :-separated list of key=value parameters", OFFSET(rav1e_opts), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { NULL }
};

static const AVClass class = {
    .class_name = "librav1e",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_librav1e_encoder = {
    .name           = "librav1e",
    .long_name      = NULL_IF_CONFIG_SMALL("librav1e AV1"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_AV1,
    .init           = librav1e_encode_init,
    .send_frame     = librav1e_send_frame,
    .receive_packet = librav1e_receive_packet,
    .close          = librav1e_encode_close,
    .priv_data_size = sizeof(librav1eContext),
    .priv_class     = &class,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV420P10,
        AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV422P10,
        AV_PIX_FMT_YUV422P12,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV444P12,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_GRAY10,
        AV_PIX_FMT_GRAY12,
        AV_PIX_FMT_NONE
    },
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
    .wrapper_name   = "librav1e",
};
