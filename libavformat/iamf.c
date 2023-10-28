/*
 * Immersive Audio Model and Formats helper functions and defines
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

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

#include "iamf.h"
#include "iamf_internal.h"

const AVChannelLayout ff_iamf_scalable_ch_layouts[10] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    // "Loudspeaker configuration for Sound System B"
    AV_CHANNEL_LAYOUT_5POINT1_BACK,
    // "Loudspeaker configuration for Sound System C"
    AV_CHANNEL_LAYOUT_5POINT1POINT2_BACK,
    // "Loudspeaker configuration for Sound System D"
    AV_CHANNEL_LAYOUT_5POINT1POINT4_BACK,
    // "Loudspeaker configuration for Sound System I"
    AV_CHANNEL_LAYOUT_7POINT1,
    // "Loudspeaker configuration for Sound System I" + Ltf + Rtf
    AV_CHANNEL_LAYOUT_7POINT1POINT2,
    // "Loudspeaker configuration for Sound System J"
    AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK,
    // Front subset of "Loudspeaker configuration for Sound System J"
    AV_CHANNEL_LAYOUT_3POINT1POINT2,
    // Binaural
    AV_CHANNEL_LAYOUT_STEREO,
};

const struct IAMFSoundSystemMap ff_iamf_sound_system_map[13] = {
    { SOUND_SYSTEM_A_0_2_0, AV_CHANNEL_LAYOUT_STEREO },
    { SOUND_SYSTEM_B_0_5_0, AV_CHANNEL_LAYOUT_5POINT1_BACK },
    { SOUND_SYSTEM_C_2_5_0, AV_CHANNEL_LAYOUT_5POINT1POINT2_BACK },
    { SOUND_SYSTEM_D_4_5_0, AV_CHANNEL_LAYOUT_5POINT1POINT4_BACK },
    { SOUND_SYSTEM_E_4_5_1,
        {
            .nb_channels = 11,
            .order       = AV_CHANNEL_ORDER_NATIVE,
            .u.mask      = AV_CH_LAYOUT_5POINT1POINT4_BACK | AV_CH_BOTTOM_FRONT_CENTER,
        },
    },
    { SOUND_SYSTEM_F_3_7_0, AV_CHANNEL_LAYOUT_7POINT2POINT3 },
    { SOUND_SYSTEM_G_4_9_0, AV_CHANNEL_LAYOUT_9POINT1POINT4_BACK },
    { SOUND_SYSTEM_H_9_10_3, AV_CHANNEL_LAYOUT_22POINT2 },
    { SOUND_SYSTEM_I_0_7_0, AV_CHANNEL_LAYOUT_7POINT1 },
    { SOUND_SYSTEM_J_4_7_0, AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK },
    { SOUND_SYSTEM_10_2_7_0, AV_CHANNEL_LAYOUT_7POINT1POINT2 },
    { SOUND_SYSTEM_11_2_3_0, AV_CHANNEL_LAYOUT_3POINT1POINT2 },
    { SOUND_SYSTEM_12_0_1_0, AV_CHANNEL_LAYOUT_MONO },
};

#define IAMF_ADD_FUNC_TEMPLATE(parent_type, parent_name, child_type, child_name, suffix)                   \
int avformat_iamf_ ## parent_name ## _add_ ## child_name(parent_type *parent_name, AVDictionary **options) \
{                                                                                                          \
    child_type **child_name ## suffix, *child_name;                                                        \
                                                                                                           \
    if (parent_name->num_## child_name ## suffix == UINT_MAX)                                              \
        return AVERROR(EINVAL);                                                                            \
                                                                                                           \
    child_name ## suffix = av_realloc_array(parent_name->child_name ## suffix,                             \
                                            parent_name->num_## child_name ## suffix + 1,                  \
                                            sizeof(*parent_name->child_name ## suffix));                   \
    if (!child_name ## suffix)                                                                             \
        return AVERROR(ENOMEM);                                                                            \
                                                                                                           \
    parent_name->child_name ## suffix = child_name ## suffix;                                              \
                                                                                                           \
    child_name = parent_name->child_name ## suffix[parent_name->num_## child_name ## suffix]               \
               = av_mallocz(sizeof(*child_name));                                                          \
    if (!child_name)                                                                                       \
        return AVERROR(ENOMEM);                                                                            \
                                                                                                           \
    child_name->av_class = &child_name ## _class;                                                          \
    av_opt_set_defaults(child_name);                                                                       \
    if (options) {                                                                                         \
        int ret = av_opt_set_dict2(child_name, options, AV_OPT_SEARCH_CHILDREN);                           \
        if (ret < 0) {                                                                                     \
            av_freep(&parent_name->child_name ## suffix[parent_name->num_## child_name ## suffix]);        \
            return ret;                                                                                    \
        }                                                                                                  \
    }                                                                                                      \
    parent_name->num_## child_name ## suffix++;                                                            \
                                                                                                           \
    return 0;                                                                                              \
}

#define FLAGS AV_OPT_FLAG_ENCODING_PARAM

//
// Param Definition
//
#define OFFSET(x) offsetof(AVIAMFMixGainParameterData, x)
static const AVOption mix_gain_options[] = {
    { "subblock_duration", "set subblock_duration", OFFSET(subblock_duration), AV_OPT_TYPE_INT64, {.i64 = 1 }, 1, UINT_MAX, FLAGS },
    { "animation_type", "set animation_type", OFFSET(animation_type), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 2, FLAGS },
    { "start_point_value", "set start_point_value", OFFSET(animation_type), AV_OPT_TYPE_RATIONAL, {.dbl = 0 }, -128.0, 128.0, FLAGS },
    { "end_point_value", "set end_point_value", OFFSET(animation_type), AV_OPT_TYPE_RATIONAL, {.dbl = 0 }, -128.0, 128.0, FLAGS },
    { "control_point_value", "set control_point_value", OFFSET(animation_type), AV_OPT_TYPE_RATIONAL, {.dbl = 0 }, -128.0, 128.0, FLAGS },
    { "control_point_relative_time", "set control_point_relative_time", OFFSET(animation_type), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, UINT8_MAX, FLAGS },
    { NULL },
};

static const AVClass mix_gain_class = {
    .class_name     = "AVIAMFSubmixElement",
    .item_name      = av_default_item_name,
    .version        = LIBAVUTIL_VERSION_INT,
    .option         = mix_gain_options,
};

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFDemixingInfoParameterData, x)
static const AVOption demixing_info_options[] = {
    { "subblock_duration", "set subblock_duration", OFFSET(subblock_duration), AV_OPT_TYPE_INT64, {.i64 = 1 }, 1, UINT_MAX, FLAGS },
    { "dmixp_mode", "set dmixp_mode", OFFSET(dmixp_mode), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 6, FLAGS },
    { NULL },
};

static const AVClass demixing_info_class = {
    .class_name     = "AVIAMFDemixingInfoParameterData",
    .item_name      = av_default_item_name,
    .version        = LIBAVUTIL_VERSION_INT,
    .option         = demixing_info_options,
};

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFReconGainParameterData, x)
static const AVOption recon_gain_options[] = {
    { "subblock_duration", "set subblock_duration", OFFSET(subblock_duration), AV_OPT_TYPE_INT64, {.i64 = 1 }, 1, UINT_MAX, FLAGS },
    { NULL },
};

static const AVClass recon_gain_class = {
    .class_name     = "AVIAMFReconGainParameterData",
    .item_name      = av_default_item_name,
    .version        = LIBAVUTIL_VERSION_INT,
    .option         = recon_gain_options,
};

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFParamDefinition, x)
static const AVOption param_definition_options[] = {
    { "parameter_id", "set parameter_id", OFFSET(parameter_id), AV_OPT_TYPE_INT64, {.i64 = 0 }, 0, UINT_MAX, FLAGS },
    { "parameter_rate", "set parameter_rate", OFFSET(parameter_rate), AV_OPT_TYPE_INT64, {.i64 = 0 }, 0, UINT_MAX, FLAGS },
    { "param_definition_mode", "set param_definition_mode", OFFSET(param_definition_mode), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 1, FLAGS },
    { "duration", "set duration", OFFSET(duration), AV_OPT_TYPE_INT64, {.i64 = 0 }, 0, UINT_MAX, FLAGS },
    { "constant_subblock_duration", "set constant_subblock_duration", OFFSET(constant_subblock_duration), AV_OPT_TYPE_INT64, {.i64 = 0 }, 0, UINT_MAX, FLAGS },
    { NULL },
};

static const AVClass *param_definition_child_iterate(void **opaque)
{
    uintptr_t i = (uintptr_t)*opaque;
    const AVClass *ret = NULL;

    switch(i) {
    case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
        ret = &mix_gain_class;
        break;
    case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
        ret = &demixing_info_class;
        break;
    case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
        ret = &recon_gain_class;
        break;
    default:
        break;
    }

    if (ret)
        *opaque = (void*)(i + 1);
    return ret;
}

static const AVClass param_definition_class = {
    .class_name          = "AVIAMFParamDefinition",
    .item_name           = av_default_item_name,
    .version             = LIBAVUTIL_VERSION_INT,
    .option              = param_definition_options,
    .child_class_iterate = param_definition_child_iterate,
};

const AVClass *avformat_iamf_param_definition_get_class(void)
{
    return &param_definition_class;
}

AVIAMFParamDefinition *avformat_iamf_param_definition_alloc(enum AVIAMFParamDefinitionType type, AVDictionary **options,
                                                            unsigned int num_subblocks, AVDictionary **subblock_options,
                                                            size_t *out_size)
{

    struct MixGainStruct {
        AVIAMFParamDefinition p;
        AVIAMFMixGainParameterData m;
    };
    struct DemixStruct {
        AVIAMFParamDefinition p;
        AVIAMFDemixingInfoParameterData d;
    };
    struct ReconGainStruct {
        AVIAMFParamDefinition p;
        AVIAMFReconGainParameterData r;
    };
    size_t subblocks_offset, subblock_size;
    size_t size;
    AVIAMFParamDefinition *par;

    switch (type) {
    case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
        subblocks_offset = offsetof(struct MixGainStruct, m);
        subblock_size = sizeof(AVIAMFMixGainParameterData);
        break;
    case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
        subblocks_offset = offsetof(struct DemixStruct, d);
        subblock_size = sizeof(AVIAMFDemixingInfoParameterData);
        break;
    case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
        subblocks_offset = offsetof(struct ReconGainStruct, r);
        subblock_size = sizeof(AVIAMFReconGainParameterData);
        break;
    default:
        return NULL;
    }

    size = subblocks_offset;
    if (num_subblocks > (SIZE_MAX - size) / subblock_size)
        return NULL;
    size += subblock_size * num_subblocks;

    par = av_mallocz(size);
    if (!par)
        return NULL;

    par->av_class = &param_definition_class;
    av_opt_set_defaults(par);
    if (options) {
        int ret = av_opt_set_dict(par, options);
        if (ret < 0) {
            av_free(par);
            return NULL;
        }
    }
    par->param_definition_type = type;
    par->num_subblocks = num_subblocks;
    par->subblock_size = subblock_size;
    par->subblocks_offset = subblocks_offset;

    for (int i = 0; i < num_subblocks; i++) {
        void *subblock = avformat_iamf_param_definition_get_subblock(par, i);

        switch (type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
            ((AVIAMFMixGainParameterData *)subblock)->av_class = &mix_gain_class;
            break;
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
            ((AVIAMFDemixingInfoParameterData *)subblock)->av_class = &demixing_info_class;
            break;
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
            ((AVIAMFReconGainParameterData *)subblock)->av_class = &recon_gain_class;
            break;
        default:
            av_assert0(0);
        }

        av_opt_set_defaults(subblock);
        if (subblock_options && subblock_options[i]) {
            int ret = av_opt_set_dict(subblock, &subblock_options[i]);
            if (ret < 0) {
                av_free(par);
                return NULL;
            }
        }
    }

    if (out_size)
        *out_size = size;

    return par;
}

//
// Audio Element
//
#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFLayer, x)
static const AVOption layer_options[] = {
    { "ch_layout", "set ch_layout", OFFSET(ch_layout), AV_OPT_TYPE_CHLAYOUT, {.str = NULL }, 0, 0, FLAGS },
    { "substream_count", "set substream_count", OFFSET(substream_count), AV_OPT_TYPE_INT, {.i64 = 1 }, 1, 255, FLAGS },
    { "recon_gain_is_present", "set recon_gain_is_present", OFFSET(recon_gain_is_present), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, FLAGS },
    { "output_gain_flags", "set output_gain_flags", OFFSET(output_gain_flags), AV_OPT_TYPE_FLAGS,
        {.i64 = 0 }, 0, (1 << 6) - 1, FLAGS, "output_gain_flags" },
            {"FL",  "Left channel",            0, AV_OPT_TYPE_CONST,
                {.i64 = 1 << 5 }, INT_MIN, INT_MAX, FLAGS, "output_gain_flags"},
            {"FR",  "Right channel",           0, AV_OPT_TYPE_CONST,
                {.i64 = 1 << 4 }, INT_MIN, INT_MAX, FLAGS, "output_gain_flags"},
            {"BL",  "Left surround channel",   0, AV_OPT_TYPE_CONST,
                {.i64 = 1 << 3 }, INT_MIN, INT_MAX, FLAGS, "output_gain_flags"},
            {"BR",  "Right surround channel",  0, AV_OPT_TYPE_CONST,
                {.i64 = 1 << 2 }, INT_MIN, INT_MAX, FLAGS, "output_gain_flags"},
            {"TFL", "Left top front channel",  0, AV_OPT_TYPE_CONST,
                {.i64 = 1 << 1 }, INT_MIN, INT_MAX, FLAGS, "output_gain_flags"},
            {"TFR", "Right top front channel", 0, AV_OPT_TYPE_CONST,
                {.i64 = 1 << 0 }, INT_MIN, INT_MAX, FLAGS, "output_gain_flags"},
    { "output_gain", "set output_gain", OFFSET(output_gain), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { "ambisonics_mode", "set ambisonics_mode", OFFSET(ambisonics_mode), AV_OPT_TYPE_INT,
            { .i64 = AV_IAMF_AMBISONICS_MODE_MONO },
            AV_IAMF_AMBISONICS_MODE_MONO, AV_IAMF_AMBISONICS_MODE_PROJECTION, FLAGS, "ambisonics_mode" },
        { "mono",       NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_AMBISONICS_MODE_MONO },       .unit = "ambisonics_mode" },
        { "projection", NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_AMBISONICS_MODE_PROJECTION }, .unit = "ambisonics_mode" },
    { NULL },
};

static const AVClass layer_class = {
    .class_name     = "AVIAMFLayer",
    .item_name      = av_default_item_name,
    .version        = LIBAVUTIL_VERSION_INT,
    .option         = layer_options,
};

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFAudioElement, x)
static const AVOption audio_element_options[] = {
    { "audio_element_type", "set audio_element_type", OFFSET(audio_element_type), AV_OPT_TYPE_INT,
            {.i64 = AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL },
            AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL, AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE, FLAGS, "audio_element_type" },
        { "channel", NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL }, .unit = "audio_element_type" },
        { "scene",   NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE },   .unit = "audio_element_type" },
    { "default_w", "set default_w", OFFSET(default_w), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 10, FLAGS },
    { NULL },
};

static const AVClass *audio_element_child_iterate(void **opaque)
{
    uintptr_t i = (uintptr_t)*opaque;
    const AVClass *ret = NULL;

    if (i)
        ret = &layer_class;

    if (ret)
        *opaque = (void*)(i + 1);
    return ret;
}

static const AVClass audio_element_class = {
    .class_name          = "AVIAMFAudioElement",
    .item_name           = av_default_item_name,
    .version             = LIBAVUTIL_VERSION_INT,
    .option              = audio_element_options,
    .child_class_iterate = audio_element_child_iterate,
};

const AVClass *avformat_iamf_audio_element_get_class(void)
{
    return &audio_element_class;
}

AVIAMFAudioElement *avformat_iamf_audio_element_alloc(void)
{
    AVIAMFAudioElement *audio_element = av_mallocz(sizeof(*audio_element));

    if (audio_element) {
        audio_element->av_class = &audio_element_class;
        av_opt_set_defaults(audio_element);
    }

    return audio_element;
}

IAMF_ADD_FUNC_TEMPLATE(AVIAMFAudioElement, audio_element, AVIAMFLayer, layer, s)

void avformat_iamf_audio_element_free(AVIAMFAudioElement **paudio_element)
{
    AVIAMFAudioElement *audio_element = *paudio_element;

    if (!audio_element)
        return;

    for (int i = 0; i < audio_element->num_layers; i++) {
        AVIAMFLayer *layer = audio_element->layers[i];
        av_opt_free(layer);
        av_free(layer->demixing_matrix);
        av_free(layer);
    }
    av_free(audio_element->layers);

    av_free(audio_element->demixing_info);
    av_free(audio_element->recon_gain_info);
    av_freep(paudio_element);
}

//
// Mix Presentation
//
#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFSubmixElement, x)
static const AVOption submix_element_options[] = {
    { "headphones_rendering_mode", "Headphones rendering mode", OFFSET(headphones_rendering_mode), AV_OPT_TYPE_INT,
            { .i64 = AV_IAMF_HEADPHONES_MODE_STEREO },
            AV_IAMF_HEADPHONES_MODE_STEREO, AV_IAMF_HEADPHONES_MODE_BINAURAL, FLAGS, "headphones_rendering_mode" },
        { "stereo",   NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_HEADPHONES_MODE_STEREO },   .unit = "headphones_rendering_mode" },
        { "binaural", NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_HEADPHONES_MODE_BINAURAL }, .unit = "headphones_rendering_mode" },
    { "default_mix_gain", "Default mix gain", OFFSET(default_mix_gain), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { "annotations", "Annotations", OFFSET(annotations), AV_OPT_TYPE_DICT, { .str = NULL }, 0, 0, FLAGS },
    { NULL },
};

static void *submix_element_child_next(void *obj, void *prev)
{
    AVIAMFSubmixElement *submix_element = obj;
    if (!prev)
        return submix_element->element_mix_config;

    return NULL;
}

static const AVClass *submix_element_child_iterate(void **opaque)
{
    uintptr_t i = (uintptr_t)*opaque;
    const AVClass *ret = NULL;

    if (i)
        ret = &param_definition_class;

    if (ret)
        *opaque = (void*)(i + 1);
    return ret;
}

static const AVClass element_class = {
    .class_name          = "AVIAMFSubmixElement",
    .item_name           = av_default_item_name,
    .version             = LIBAVUTIL_VERSION_INT,
    .option              = submix_element_options,
    .child_next          = submix_element_child_next,
    .child_class_iterate = submix_element_child_iterate,
};

IAMF_ADD_FUNC_TEMPLATE(AVIAMFSubmix, submix, AVIAMFSubmixElement, element, s)

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFSubmixLayout, x)
static const AVOption submix_layout_options[] = {
    { "layout_type", "Layout type", OFFSET(layout_type), AV_OPT_TYPE_INT,
            { .i64 = AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS },
            AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS, AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL, FLAGS, "layout_type" },
        { "loudspeakers", NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS }, .unit = "layout_type" },
        { "binaural",     NULL, 0, AV_OPT_TYPE_CONST,
                   { .i64 = AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL },     .unit = "layout_type" },
    { "sound_system", "Sound System", OFFSET(sound_system), AV_OPT_TYPE_CHLAYOUT, { .str = NULL }, 0, 0, FLAGS },
    { "integrated_loudness", "Integrated loudness", OFFSET(integrated_loudness), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { "digital_peak", "Digital peak", OFFSET(digital_peak), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { "true_peak", "True peak", OFFSET(true_peak), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { "dialog_anchored_loudness", "Anchored loudness (Dialog)", OFFSET(anchored_loudness[1]), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { "album_anchored_loudness", "Anchored loudness (Album)", OFFSET(anchored_loudness[2]), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { NULL },
};

static const AVClass layout_class = {
    .class_name     = "AVIAMFSubmixLayout",
    .item_name      = av_default_item_name,
    .version        = LIBAVUTIL_VERSION_INT,
    .option         = submix_layout_options,
};

IAMF_ADD_FUNC_TEMPLATE(AVIAMFSubmix, submix, AVIAMFSubmixLayout, layout, s)

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFSubmix, x)
static const AVOption submix_presentation_options[] = {
    { "default_mix_gain", "Default mix gain", OFFSET(default_mix_gain), AV_OPT_TYPE_RATIONAL, { .dbl = 0 }, -128.0, 128.0, FLAGS },
    { NULL },
};

static void *submix_presentation_child_next(void *obj, void *prev)
{
    AVIAMFSubmix *sub_mix = obj;
    if (!prev)
        return sub_mix->output_mix_config;

    return NULL;
}

static const AVClass *submix_presentation_child_iterate(void **opaque)
{
    uintptr_t i = (uintptr_t)*opaque;
    const AVClass *ret = NULL;

    switch(i) {
    case 0:
        ret = &element_class;
        break;
    case 1:
        ret = &layout_class;
        break;
    case 2:
        ret = &param_definition_class;
        break;
    default:
        break;
    }

    if (ret)
        *opaque = (void*)(i + 1);
    return ret;
}

static const AVClass submix_class = {
    .class_name          = "AVIAMFSubmix",
    .item_name           = av_default_item_name,
    .version             = LIBAVUTIL_VERSION_INT,
    .option              = submix_presentation_options,
    .child_next          = submix_presentation_child_next,
    .child_class_iterate = submix_presentation_child_iterate,
};

#undef OFFSET
#define OFFSET(x) offsetof(AVIAMFMixPresentation, x)
static const AVOption mix_presentation_options[] = {
    { "annotations", "set annotations", OFFSET(annotations), AV_OPT_TYPE_DICT, {.str = NULL }, 0, 0, FLAGS },
    { NULL },
};

#undef OFFSET
#undef FLAGS

static const AVClass *mix_presentation_child_iterate(void **opaque)
{
    uintptr_t i = (uintptr_t)*opaque;
    const AVClass *ret = NULL;

    if (i)
        ret = &submix_class;

    if (ret)
        *opaque = (void*)(i + 1);
    return ret;
}

static const AVClass mix_presentation_class = {
    .class_name          = "AVIAMFMixPresentation",
    .item_name           = av_default_item_name,
    .version             = LIBAVUTIL_VERSION_INT,
    .option              = mix_presentation_options,
    .child_class_iterate = mix_presentation_child_iterate,
};

const AVClass *avformat_iamf_mix_presentation_get_class(void)
{
    return &mix_presentation_class;
}

AVIAMFMixPresentation *avformat_iamf_mix_presentation_alloc(void)
{
    AVIAMFMixPresentation *mix_presentation = av_mallocz(sizeof(*mix_presentation));

    if (mix_presentation) {
        mix_presentation->av_class = &mix_presentation_class;
        av_opt_set_defaults(mix_presentation);
    }

    return mix_presentation;
}

IAMF_ADD_FUNC_TEMPLATE(AVIAMFMixPresentation, mix_presentation, AVIAMFSubmix, submix, es)

void avformat_iamf_mix_presentation_free(AVIAMFMixPresentation **pmix_presentation)
{
    AVIAMFMixPresentation *mix_presentation = *pmix_presentation;

    if (!mix_presentation)
        return;

    for (int i = 0; i < mix_presentation->num_submixes; i++) {
        AVIAMFSubmix *sub_mix = mix_presentation->submixes[i];
        for (int j = 0; j < sub_mix->num_elements; j++) {
            AVIAMFSubmixElement *submix_element = sub_mix->elements[j];
            av_opt_free(submix_element);
            av_free(submix_element->element_mix_config);
            av_free(submix_element);
        }
        av_free(sub_mix->elements);
        for (int j = 0; j < sub_mix->num_layouts; j++) {
            AVIAMFSubmixLayout *submix_layout = sub_mix->layouts[j];
            av_opt_free(submix_layout);
            av_free(submix_layout);
        }
        av_free(sub_mix->layouts);
        av_free(sub_mix->output_mix_config);
        av_free(sub_mix);
    }
    av_opt_free(mix_presentation);
    av_free(mix_presentation->submixes);

    av_freep(pmix_presentation);
}
