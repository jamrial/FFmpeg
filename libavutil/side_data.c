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

#include "avassert.h"
#include "buffer.h"
#include "common.h"
#include "dict.h"
#include "frame.h"
#include "mem.h"
#include "refstruct.h"
#include "side_data.h"

// headers for struct sizes
#include "libavcodec/defs.h"
#include "ambient_viewing_environment.h"
#include "downmix_info.h"
#include "hdr_dynamic_metadata.h"
#include "hdr_dynamic_vivid_metadata.h"
#include "mastering_display_metadata.h"
#include "motion_vector.h"
#include "replaygain.h"
#include "spherical.h"
#include "stereo3d.h"

typedef struct FFFrameSideData {
    AVFrameSideData p;

#if FF_API_SIDE_DATA_BUF == 0
    AVBufferRef *buf;
#endif
} FFFrameSideData;

typedef struct FFSideDataDescriptor {
    AVSideDataDescriptor p;

    void (*init)(void *obj);

    size_t size;
} FFSideDataDescriptor;

static const FFSideDataDescriptor sd_props[] = {
    [AV_FRAME_DATA_PANSCAN]                     = { .p = { "AVPanScan",                                    AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_SIZE_DEPENDENT },
                                                    .size = sizeof(AVPanScan) },
    [AV_FRAME_DATA_A53_CC]                      = { .p = { "ATSC A53 Part 4 Closed Captions" } },
    [AV_FRAME_DATA_MATRIXENCODING]              = { .p = { "AVMatrixEncoding",                             AV_SIDE_DATA_PROP_CHANNEL_DEPENDENT } },
    [AV_FRAME_DATA_DOWNMIX_INFO]                = { .p = { "Metadata relevant to a downmix procedure",     AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_CHANNEL_DEPENDENT },
                                                    .size = sizeof(AVDownmixInfo) },
    [AV_FRAME_DATA_AFD]                         = { .p = { "Active format description" } },
    [AV_FRAME_DATA_MOTION_VECTORS]              = { .p = { "Motion vectors",                               AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_SIZE_DEPENDENT },
                                                    .size = sizeof(AVMotionVector) },
    [AV_FRAME_DATA_SKIP_SAMPLES]                = { .p = { "Skip samples" } },
    [AV_FRAME_DATA_GOP_TIMECODE]                = { .p = { "GOP timecode" } },
    [AV_FRAME_DATA_S12M_TIMECODE]               = { .p = { "SMPTE 12-1 timecode" } },
    [AV_FRAME_DATA_DYNAMIC_HDR_PLUS]            = { .p = { "HDR Dynamic Metadata SMPTE2094-40 (HDR10+)",   AV_SIDE_DATA_PROP_STRUCT| AV_SIDE_DATA_PROP_COLOR_DEPENDENT },
                                                    .size = sizeof(AVDynamicHDRPlus) },
    [AV_FRAME_DATA_DYNAMIC_HDR_VIVID]           = { .p = { "HDR Dynamic Metadata CUVA 005.1 2021 (Vivid)", AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_COLOR_DEPENDENT },
                                                    .size = sizeof(AVDynamicHDRVivid) },
    [AV_FRAME_DATA_REGIONS_OF_INTEREST]         = { .p = { "Regions Of Interest",                          AV_SIDE_DATA_PROP_SIZE_DEPENDENT } },
    [AV_FRAME_DATA_VIDEO_ENC_PARAMS]            = { .p = { "Video encoding parameters" } },
    [AV_FRAME_DATA_FILM_GRAIN_PARAMS]           = { .p = { "Film grain parameters",                        AV_SIDE_DATA_PROP_STRUCT } },
    [AV_FRAME_DATA_DETECTION_BBOXES]            = { .p = { "Bounding boxes for object detection and classification", AV_SIDE_DATA_PROP_SIZE_DEPENDENT } },
    [AV_FRAME_DATA_DOVI_RPU_BUFFER]             = { .p = { "Dolby Vision RPU Data",                        AV_SIDE_DATA_PROP_COLOR_DEPENDENT } },
    [AV_FRAME_DATA_DOVI_METADATA]               = { .p = { "Dolby Vision Metadata",                        AV_SIDE_DATA_PROP_COLOR_DEPENDENT } },
    [AV_FRAME_DATA_LCEVC]                       = { .p = { "LCEVC NAL data",                               AV_SIDE_DATA_PROP_SIZE_DEPENDENT } },
    [AV_FRAME_DATA_VIEW_ID]                     = { .p = { "View ID" } },
    [AV_FRAME_DATA_STEREO3D]                    = { .p = { "Stereo 3D",                                    AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_STRUCT },
                                                    .size = sizeof(AVStereo3D) },
    [AV_FRAME_DATA_REPLAYGAIN]                  = { .p = { "AVReplayGain",                                 AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_STRUCT },
                                                    .size = sizeof(AVReplayGain) },
    [AV_FRAME_DATA_DISPLAYMATRIX]               = { .p = { "3x3 displaymatrix",                            AV_SIDE_DATA_PROP_GLOBAL } },
    [AV_FRAME_DATA_AUDIO_SERVICE_TYPE]          = { .p = { "Audio service type",                           AV_SIDE_DATA_PROP_GLOBAL } },
    [AV_FRAME_DATA_MASTERING_DISPLAY_METADATA]  = { .p = { "Mastering display metadata",                   AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_COLOR_DEPENDENT },
                                                    .init = ff_mdm_get_defaults,
                                                    .size = sizeof(AVMasteringDisplayMetadata) },
    [AV_FRAME_DATA_CONTENT_LIGHT_LEVEL]         = { .p = { "Content light level metadata",                 AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_COLOR_DEPENDENT },
                                                    .size = sizeof(AVContentLightMetadata) },
    [AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT] = { .p = { "Ambient viewing environment",                  AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_STRUCT },
                                                    .init = ff_ave_get_defaults,
                                                    .size = sizeof(AVAmbientViewingEnvironment) },
    [AV_FRAME_DATA_SPHERICAL]                   = { .p = { "Spherical Mapping",                            AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_STRUCT | AV_SIDE_DATA_PROP_SIZE_DEPENDENT },
                                                    .init = ff_spherical_get_defaults,
                                                    .size = sizeof(AVSphericalMapping) },
    [AV_FRAME_DATA_ICC_PROFILE]                 = { .p = { "ICC profile",                                  AV_SIDE_DATA_PROP_GLOBAL | AV_SIDE_DATA_PROP_COLOR_DEPENDENT } },
    [AV_FRAME_DATA_SEI_UNREGISTERED]            = { .p = { "H.26[45] User Data Unregistered SEI message",  AV_SIDE_DATA_PROP_MULTI } },
    [AV_FRAME_DATA_VIDEO_HINT]                  = { .p = { "Encoding video hint",                          AV_SIDE_DATA_PROP_SIZE_DEPENDENT } },
};

static FFFrameSideData *sdp_from_sd(AVFrameSideData *sd)
{
    return (FFFrameSideData *)sd;
}

static const FFFrameSideData *csdp_from_sd(const AVFrameSideData *sd)
{
    return (const FFFrameSideData *)sd;
}

static const FFSideDataDescriptor *dp_from_desc(const AVSideDataDescriptor *desc)
{
    return (const FFSideDataDescriptor *)desc;
}

const AVSideDataDescriptor *av_frame_side_data_desc(enum AVFrameSideDataType type)
{
    unsigned t = type;
    if (t < FF_ARRAY_ELEMS(sd_props) && sd_props[t].p.name)
        return &sd_props[t].p;
    return NULL;
}

const char *av_frame_side_data_name(enum AVFrameSideDataType type)
{
    const AVSideDataDescriptor *desc = av_frame_side_data_desc(type);
    return desc ? desc->name : NULL;
}

static void free_side_data_entry(AVFrameSideData **ptr_sd)
{
    AVFrameSideData *sd = *ptr_sd;
    FFFrameSideData *sdp = sdp_from_sd(sd);

#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    av_buffer_unref(&sd->buf);
FF_ENABLE_DEPRECATION_WARNINGS
#else
    av_buffer_unref(&sdp->buf);
#endif
    av_dict_free(&sd->metadata);
    av_freep(ptr_sd);
}

static void remove_side_data_by_entry(AVFrameSideData ***sd, int *nb_sd,
                                      const AVFrameSideData *target)
{
    for (int i = *nb_sd - 1; i >= 0; i--) {
        AVFrameSideData *entry = ((*sd)[i]);
        if (entry != target)
            continue;

        free_side_data_entry(&entry);

        ((*sd)[i]) = ((*sd)[*nb_sd - 1]);
        (*nb_sd)--;

        return;
    }
}

void av_frame_side_data_remove(AVFrameSideData ***sd, int *nb_sd,
                               enum AVFrameSideDataType type)
{
    for (int i = *nb_sd - 1; i >= 0; i--) {
        AVFrameSideData *entry = ((*sd)[i]);
        if (entry->type != type)
            continue;

        free_side_data_entry(&entry);

        ((*sd)[i]) = ((*sd)[*nb_sd - 1]);
        (*nb_sd)--;
    }
}

void av_frame_side_data_remove_by_props(AVFrameSideData ***sd, int *nb_sd,
                                        int props)
{
    for (int i = *nb_sd - 1; i >= 0; i--) {
        AVFrameSideData *entry = ((*sd)[i]);
        const AVSideDataDescriptor *desc = av_frame_side_data_desc(entry->type);
        if (!desc || !(desc->props & props))
            continue;

        free_side_data_entry(&entry);

        ((*sd)[i]) = ((*sd)[*nb_sd - 1]);
        (*nb_sd)--;
    }
}

void av_frame_side_data_free(AVFrameSideData ***sd, int *nb_sd)
{
    for (int i = 0; i < *nb_sd; i++)
        free_side_data_entry(&((*sd)[i]));
    *nb_sd = 0;

    av_freep(sd);
}

static AVFrameSideData *add_side_data_from_buf_ext(AVFrameSideData ***sd,
                                                   int *nb_sd,
                                                   enum AVFrameSideDataType type,
                                                   AVBufferRef *buf, uint8_t *data,
                                                   size_t size)
{
    FFFrameSideData *sdp;
    AVFrameSideData *ret, **tmp;

    // *nb_sd + 1 needs to fit into an int and a size_t.
    if ((unsigned)*nb_sd >= FFMIN(INT_MAX, SIZE_MAX))
        return NULL;

    tmp = av_realloc_array(*sd, sizeof(**sd), *nb_sd + 1);
    if (!tmp)
        return NULL;
    *sd = tmp;

    sdp = av_mallocz(sizeof(*sdp));
    if (!sdp)
        return NULL;

    ret = &sdp->p;
#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    ret->buf = buf;
FF_ENABLE_DEPRECATION_WARNINGS
#else
    sdp->buf = buf;
#endif
    ret->data = data;
    ret->size = size;
    ret->type = type;

    (*sd)[(*nb_sd)++] = ret;

    return ret;
}

AVFrameSideData *ff_frame_side_data_add_from_buf(AVFrameSideData ***sd,
                                                 int *nb_sd,
                                                 enum AVFrameSideDataType type,
                                                 AVBufferRef *buf)
{
    if (!buf)
        return NULL;

    return add_side_data_from_buf_ext(sd, nb_sd, type, buf, buf->data, buf->size);
}

static AVFrameSideData *replace_side_data_from_buf(AVFrameSideData *dst,
                                                   AVBufferRef *buf, int flags)
{
    if (!(flags & AV_FRAME_SIDE_DATA_FLAG_REPLACE))
        return NULL;

    av_dict_free(&dst->metadata);
#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    av_buffer_unref(&dst->buf);
    dst->buf  = buf;
FF_ENABLE_DEPRECATION_WARNINGS
#else
    av_buffer_unref(&sdp_from_sd(dst)->buf);
    sdp_from_sd(dst)->buf = buf;
#endif
    dst->data = buf->data;
    dst->size = buf->size;
    return dst;
}

AVFrameSideData *av_frame_side_data_new(AVFrameSideData ***sd, int *nb_sd,
                                        enum AVFrameSideDataType type,
                                        size_t size, unsigned int flags)
{
    const AVSideDataDescriptor *desc = av_frame_side_data_desc(type);
    AVBufferRef     *buf = av_buffer_alloc(size);
    AVFrameSideData *ret = NULL;

    if (flags & AV_FRAME_SIDE_DATA_FLAG_UNIQUE)
        av_frame_side_data_remove(sd, nb_sd, type);
    if ((!desc || !(desc->props & AV_SIDE_DATA_PROP_MULTI)) &&
        (ret = (AVFrameSideData *)av_frame_side_data_get(*sd, *nb_sd, type))) {
        ret = replace_side_data_from_buf(ret, buf, flags);
        if (!ret)
            av_buffer_unref(&buf);
        return ret;
    }

    ret = ff_frame_side_data_add_from_buf(sd, nb_sd, type, buf);
    if (!ret)
        av_buffer_unref(&buf);

    return ret;
}

AVFrameSideData *av_frame_side_data_add(AVFrameSideData ***sd, int *nb_sd,
                                        enum AVFrameSideDataType type,
                                        AVBufferRef **pbuf, unsigned int flags)
{
    const AVSideDataDescriptor *desc = av_frame_side_data_desc(type);
    AVFrameSideData *sd_dst  = NULL;
    AVBufferRef *buf = *pbuf;

    if ((flags & AV_FRAME_SIDE_DATA_FLAG_NEW_REF) && !(buf = av_buffer_ref(*pbuf)))
        return NULL;
    if (flags & AV_FRAME_SIDE_DATA_FLAG_UNIQUE)
        av_frame_side_data_remove(sd, nb_sd, type);
    if ((!desc || !(desc->props & AV_SIDE_DATA_PROP_MULTI)) &&
        (sd_dst = (AVFrameSideData *)av_frame_side_data_get(*sd, *nb_sd, type))) {
        sd_dst = replace_side_data_from_buf(sd_dst, buf, flags);
    } else
        sd_dst = ff_frame_side_data_add_from_buf(sd, nb_sd, type, buf);

    if (sd_dst && !(flags & AV_FRAME_SIDE_DATA_FLAG_NEW_REF))
        *pbuf = NULL;
    else if (!sd_dst && (flags & AV_FRAME_SIDE_DATA_FLAG_NEW_REF))
        av_buffer_unref(&buf);
    return sd_dst;
}

AVFrameSideData *av_frame_side_data_new_struct(AVFrameSideData ***sd, int *nb_sd,
                                               enum AVFrameSideDataType type,
                                               unsigned int flags)
{
    const AVSideDataDescriptor *desc = av_frame_side_data_desc(type);
    const FFSideDataDescriptor *dp = dp_from_desc(desc);
    AVFrameSideData *ret;

    if (!desc || !(desc->props & AV_SIDE_DATA_PROP_STRUCT))
        return NULL;

    av_assert0(dp->size);
    ret = av_frame_side_data_new(sd, nb_sd, type, dp->size, flags);
    if (ret && dp->init)
         dp->init(ret->data);
    return ret;
}

int av_frame_side_data_clone(AVFrameSideData ***sd, int *nb_sd,
                             const AVFrameSideData *src, unsigned int flags)
{
    const AVSideDataDescriptor *desc;
    const FFFrameSideData *srcp = csdp_from_sd(src);
    AVBufferRef     *buf    = NULL;
    AVFrameSideData *sd_dst = NULL;
    int              ret    = AVERROR_BUG;

    if (!sd || !src || !nb_sd || (*nb_sd && !*sd))
        return AVERROR(EINVAL);

    desc = av_frame_side_data_desc(src->type);
    if (flags & AV_FRAME_SIDE_DATA_FLAG_UNIQUE)
        av_frame_side_data_remove(sd, nb_sd, src->type);
    if ((!desc || !(desc->props & AV_SIDE_DATA_PROP_MULTI)) &&
        (sd_dst = (AVFrameSideData *)av_frame_side_data_get(*sd, *nb_sd, src->type))) {
        FFFrameSideData *dstp = sdp_from_sd(sd_dst);
        AVDictionary *dict = NULL;

        if (!(flags & AV_FRAME_SIDE_DATA_FLAG_REPLACE))
            return AVERROR(EEXIST);

        ret = av_dict_copy(&dict, src->metadata, 0);
        if (ret < 0)
            return ret;

#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
        ret = av_buffer_replace(&sd_dst->buf, src->buf);
FF_ENABLE_DEPRECATION_WARNINGS
#else
        ret = av_buffer_replace(&dstp->buf, srcp->buf);
#endif
        if (ret < 0) {
            av_dict_free(&dict);
            return ret;
        }

        av_dict_free(&sd_dst->metadata);
        sd_dst->metadata = dict;
        sd_dst->data     = src->data;
        sd_dst->size     = src->size;
        return 0;
    }

#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    buf = av_buffer_ref(src->buf);
FF_ENABLE_DEPRECATION_WARNINGS
#else
    buf = av_buffer_ref(srcp->buf);
#endif
    if (!buf)
        return AVERROR(ENOMEM);

    sd_dst = add_side_data_from_buf_ext(sd, nb_sd, src->type, buf,
                                        src->data, src->size);
    if (!sd_dst) {
        av_buffer_unref(&buf);
        return AVERROR(ENOMEM);
    }

    ret = av_dict_copy(&sd_dst->metadata, src->metadata, 0);
    if (ret < 0) {
        remove_side_data_by_entry(sd, nb_sd, sd_dst);
        return ret;
    }

    return 0;
}

const AVFrameSideData *av_frame_side_data_get_c(const AVFrameSideData * const *sd,
                                                const int nb_sd,
                                                enum AVFrameSideDataType type)
{
    for (int i = 0; i < nb_sd; i++) {
        if (sd[i]->type == type)
            return sd[i];
    }
    return NULL;
}

int av_frame_side_data_is_writable(const AVFrameSideData *sd)
{
#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    return !!av_buffer_is_writable(sd->buf);
FF_ENABLE_DEPRECATION_WARNINGS
#else
    const FFFrameSideData *sdp = csdp_from_sd(sd);
    return !!av_buffer_is_writable(sdp->buf);
#endif
}

int av_frame_side_data_make_writable(AVFrameSideData *sd)
{
    FFFrameSideData *sdp = sdp_from_sd(sd);
    AVBufferRef *buf = NULL;

#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    if (av_buffer_is_writable(sd->buf))
FF_ENABLE_DEPRECATION_WARNINGS
#else
    if (av_buffer_is_writable(sdp->buf))
#endif
        return 0;

    buf = av_buffer_alloc(sd->size);
    if (!buf)
        return AVERROR(ENOMEM);

    if (sd->size)
        memcpy(buf->data, sd->data, sd->size);
#if FF_API_SIDE_DATA_BUF
FF_DISABLE_DEPRECATION_WARNINGS
    av_buffer_unref(&sd->buf);
    sd->buf  = buf;
FF_ENABLE_DEPRECATION_WARNINGS
#else
    av_buffer_unref(&sdp->buf);
    sdp->buf = buf;
#endif
    sd->data = buf->data;

    return 0;
}
