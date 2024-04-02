/*
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

#include <LCEVC/lcevc_dec.h>

#include "libavutil/avassert.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "avcodec.h"
#include "postprocess_internal.h"

typedef struct FFLCEVCContext {
    LCEVC_DecoderHandle decoder;
} FFLCEVCContext;

static LCEVC_ColorFormat map_format(int format)
{
    switch (format) {
    case AV_PIX_FMT_YUV420P:
        return LCEVC_I420_8;
    case AV_PIX_FMT_YUV420P10:
        return LCEVC_I420_10_LE;
    case AV_PIX_FMT_NV12:
        return LCEVC_NV12_8;
    case AV_PIX_FMT_NV21:
        return LCEVC_NV21_8;
    case AV_PIX_FMT_GRAY8:
        return LCEVC_GRAY_8;
    }

    return LCEVC_ColorFormat_Unknown;
}

static int alloc_base_frame(void *logctx, LCEVC_DecoderHandle decoder,
                            const AVFrame *frame, LCEVC_PictureHandle *picture)
{
    LCEVC_PictureDesc desc;
    LCEVC_ColorFormat fmt = map_format(frame->format);
    LCEVC_PictureLockHandle lock;
    uint8_t *data[4] = { NULL };
    int linesizes[4] = { 0 };
    uint32_t planes;
    LCEVC_ReturnCode res;

    res = LCEVC_DefaultPictureDesc(&desc, fmt, frame->width, frame->height);
    if (res != LCEVC_Success)
		return AVERROR_EXTERNAL;

    desc.cropTop    = frame->crop_top;
    desc.cropBottom = frame->crop_bottom;
    desc.cropLeft   = frame->crop_left;
    desc.cropRight  = frame->crop_right;
    desc.sampleAspectRatioNum  = frame->sample_aspect_ratio.num;
    desc.sampleAspectRatioDen  = frame->sample_aspect_ratio.den;

    /* Allocate LCEVC Picture */
    res = LCEVC_AllocPicture(decoder, &desc, picture);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    res = LCEVC_LockPicture(decoder, *picture, LCEVC_Access_Write, &lock);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    res = LCEVC_GetPicturePlaneCount(decoder, *picture, &planes);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    for (unsigned i = 0; i < planes; i++) {
        LCEVC_PicturePlaneDesc plane;

        res = LCEVC_GetPictureLockPlaneDesc(decoder, lock, i, &plane);
        if (res != LCEVC_Success)
            return AVERROR_EXTERNAL;

        data[i] = plane.firstSample;
        linesizes[i] = plane.rowByteStride;
    }

    av_image_copy2(data, linesizes, frame->data, frame->linesize,
                   frame->format, frame->width, frame->height);

    res = LCEVC_UnlockPicture(decoder, lock);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    return 0;
}

static int alloc_enhanced_frame(void *logctx, LCEVC_DecoderHandle decoder,
                                const AVFrame *frame, LCEVC_PictureHandle *picture)
{
    LCEVC_PictureDesc desc ;
    LCEVC_ColorFormat fmt = map_format(frame->format);
    LCEVC_PicturePlaneDesc planes[4] = { 0 };
    int width = frame->width * 2 / FFMAX(frame->sample_aspect_ratio.den, 1);
    int height = frame->height * 2 / FFMAX(frame->sample_aspect_ratio.num, 1);
    LCEVC_ReturnCode res;

    res = LCEVC_DefaultPictureDesc(&desc, fmt, width, height);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    /* Set plane description */
    for (int i = 0; i < 4; i++) {
        planes[i].firstSample = frame->data[i];
        planes[i].rowByteStride = frame->linesize[i];
    }

    /* Allocate LCEVC Picture */
    res = LCEVC_AllocPictureExternal(decoder, &desc, NULL, planes, picture);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    return 0;
}

static int lcevc_send_frame(void *logctx, FFLCEVCContext *lcevc, const AVFrame *in)
{
    const AVFrameSideData *sd = av_frame_get_side_data(in, AV_FRAME_DATA_LCEVC);
    LCEVC_PictureHandle picture;
    LCEVC_ReturnCode res;
    int ret = 0;

    if (!sd)
        return 1;

    res = LCEVC_SendDecoderEnhancementData(lcevc->decoder, in->pts, 0, sd->data, sd->size);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    ret = alloc_base_frame(logctx, lcevc->decoder, in, &picture);
    if (ret < 0)
        return ret;

    res = LCEVC_SendDecoderBase(lcevc->decoder, in->pts, 0, picture, -1, NULL);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    memset(&picture, 0, sizeof(picture));
    ret = alloc_enhanced_frame(logctx, lcevc->decoder, in, &picture);
    if (ret < 0)
        return ret;

    res = LCEVC_SendDecoderPicture(lcevc->decoder, picture);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    return 0;
}

static int generate_output(void *logctx, FFLCEVCContext *lcevc, AVFrame *out)
{
    LCEVC_PictureDesc desc;
    LCEVC_DecodeInformation info;
    LCEVC_PictureHandle picture;
    LCEVC_ReturnCode res;

    res = LCEVC_ReceiveDecoderPicture(lcevc->decoder, &picture, &info);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    res = LCEVC_GetPictureDesc(lcevc->decoder, picture, &desc);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    out->crop_top = desc.cropTop;
    out->crop_bottom = desc.cropBottom;
    out->crop_left = desc.cropLeft;
    out->crop_right = desc.cropRight;
    out->sample_aspect_ratio.num = desc.sampleAspectRatioNum;
    out->sample_aspect_ratio.den = desc.sampleAspectRatioDen;
    out->width = desc.width + out->crop_left + out->crop_right;
    out->height = desc.height + out->crop_top + out->crop_bottom;

    res = LCEVC_FreePicture(lcevc->decoder, picture);
    if (res != LCEVC_Success)
        return AVERROR_EXTERNAL;

    return 0;
}

static int lcevc_receive_frame(void *logctx, FFLCEVCContext *lcevc, AVFrame *out)
{
    LCEVC_PictureHandle picture;
    LCEVC_ReturnCode res;
    int ret;

    ret = generate_output(logctx, lcevc, out);
    if (ret < 0)
        return ret;

    while (1) {
        res = LCEVC_ReceiveDecoderBase (lcevc->decoder, &picture);
        if (res == LCEVC_Again)
            break;

        if (res != LCEVC_Success)
            return AVERROR_EXTERNAL;

        res = LCEVC_FreePicture(lcevc->decoder, picture);
        if (res != LCEVC_Success)
            return AVERROR_EXTERNAL;
    }

    return 0;
}

static int lcevc_get_buffer(FFPostProc *pp, void *opaque, void *obj, int flags)
{
    AVCodecContext *avctx = opaque;
    AVFrame *frame = obj;
    FFLCEVCContext *lcevc = pp->priv_data;
    int ret, width = frame->width, height = frame->height;

    frame->width  = frame->width  * 2 / FFMAX(frame->sample_aspect_ratio.den, 1);
    frame->height = frame->height * 2 / FFMAX(frame->sample_aspect_ratio.num, 1);

    ret = avctx->get_buffer2(avctx, frame, flags);
    if (ret < 0)
        return ret;

    frame->width  = width;
    frame->height = height;

    return 0;
}

static int lcevc_process(FFPostProc *pp, void *logctx, void *obj)
{
    AVFrame *frame = obj;
    FFLCEVCContext *lcevc = pp->priv_data;
    int ret;

    ret = lcevc_send_frame(logctx, lcevc, frame);
    if (ret)
        return ret < 0 ? ret : 0;

    lcevc_receive_frame(logctx, lcevc, frame);
    if (ret < 0)
        return ret;

    av_frame_remove_side_data(frame, AV_FRAME_DATA_LCEVC);

    return 0;
}

static void event_callback(LCEVC_DecoderHandle dec, LCEVC_Event event,
    LCEVC_PictureHandle pic, const LCEVC_DecodeInformation *info,
    const uint8_t *data, uint32_t size, void *logctx)
{
    switch (event) {
    case LCEVC_Log:
        av_log(logctx, AV_LOG_INFO, "%s\n", data);
        break;
    default:
        break;
    }
}

static int lcevc_init(FFPostProc *pp, void *logctx)
{
    FFLCEVCContext *lcevc = pp->priv_data;
    LCEVC_AccelContextHandle dummy = { 0 };

    if (LCEVC_CreateDecoder(&lcevc->decoder, dummy) != LCEVC_Success) {
        av_log(logctx, AV_LOG_ERROR, "Failed to create LCEVC decoder\n");
        return AVERROR_EXTERNAL;
    }

    LCEVC_ConfigureDecoderInt(lcevc->decoder, "log_level", 4);
    LCEVC_ConfigureDecoderInt(lcevc->decoder, "events", LCEVC_Log);
    LCEVC_SetDecoderEventCallback(lcevc->decoder, event_callback, logctx);

    if (LCEVC_InitializeDecoder(lcevc->decoder) != LCEVC_Success) {
        av_log(logctx, AV_LOG_ERROR, "Failed to initialize LCEVC decoder\n");
        LCEVC_DestroyDecoder(lcevc->decoder);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static void lcevc_free(FFPostProc *pp)
{
    FFLCEVCContext *lcevc = pp->priv_data;
    LCEVC_DestroyDecoder(lcevc->decoder);
}

const FFPostProcType ff_lcevc_postproc = {
    .type              = FF_POSTPROC_TYPE_LCEVC,

    .priv_data_size    = sizeof(FFLCEVCContext),

    .init              = &lcevc_init,
    .get_buffer        = &lcevc_get_buffer,
    .process           = &lcevc_process,
    .close             = &lcevc_free,
};
