/*
 * IAMF muxer
 * Copyright (c) 2023 James Almer
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

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/flac.h"
#include "libavcodec/mpeg4audio.h"
#include "libavcodec/put_bits.h"
#include "avformat.h"
#include "avio_internal.h"
#include "iamf.h"
#include "iamf_internal.h"
#include "internal.h"
#include "mux.h"

typedef struct IAMFCodecConfig {
    unsigned codec_config_id;
    enum AVCodecID codec_id;
    uint32_t codec_tag;
    unsigned nb_samples;
    int seek_preroll;
    uint8_t *extradata;
    int extradata_size;
    int sample_rate;
} IAMFCodecConfig;

typedef struct IAMFAudioElement {
    const IAMFCodecConfig *codec_config;
    const AVStreamGroup *stream_group;
    const AVStream **audio_substreams;
    unsigned int num_substreams;
} IAMFAudioElement;

typedef struct IAMFMixPresentation {
    const AVStreamGroup *stream_group;
} IAMFMixPresentation;

typedef struct IAMFParamDefinition {
    const AVIAMFAudioElement *audio_element;
    const AVIAMFParamDefinition *param;
} IAMFParamDefinition;

typedef struct IAMFMuxContext {
    IAMFCodecConfig *codec_configs;
    int nb_codec_configs;
    IAMFAudioElement *audio_elements;
    int nb_audio_elements;
    IAMFMixPresentation *mix_presentations;
    int nb_mix_presentations;
    IAMFParamDefinition *param_definitions;
    int nb_param_definitions;

    int first_stream_id;

    unsigned int nb_stream_groups;
    AVStreamGroup **stream_groups;
} IAMFMuxContext;

// Temporary code to generate stream groups.
// They should be set by the caller, as this is not guaranteed to be right
static int iamf_generate_stream_group(AVFormatContext *s)
{
    IAMFMuxContext *const c = s->priv_data;
    FFStreamGroup *stgi;
    AVStreamGroup *stg;
    AVIAMFParamDefinition *param;
    int ret, channel_count = 0, num_layers = 0;
    int highest_channel_count = 0;

    c->stream_groups = av_mallocz(sizeof(*c->stream_groups));
    if (!c->stream_groups)
        return AVERROR(ENOMEM);

    stgi = av_mallocz(sizeof(*stgi));
    if (!stgi)
        return AVERROR(ENOMEM);
    stg = &stgi->pub;
    stgi->fmtctx = s;

    c->stream_groups[c->nb_stream_groups] = stg;

    stg->id = c->nb_stream_groups;
    stg->index = c->nb_stream_groups++;
    stg->type = AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT;

    stg->params.iamf_audio_element = av_mallocz(sizeof(AVIAMFAudioElement));
    if (!stg->params.iamf_audio_element)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->nb_streams; i++) {
        ret = avformat_stream_group_add_stream(stg, s->streams[i]);
        if (ret < 0)
            return ret;
        channel_count += s->streams[i]->codecpar->ch_layout.nb_channels;
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); i++) {
        if (channel_count == ff_iamf_scalable_ch_layouts[i].nb_channels)
            break;
        if (highest_channel_count >= ff_iamf_scalable_ch_layouts[i].nb_channels)
            continue;
        highest_channel_count = ff_iamf_scalable_ch_layouts[i].nb_channels;
        num_layers++;
    }
    if (!num_layers)
        num_layers = 1;

    channel_count = 0;
    for (int k = 0, i = 0; i < num_layers; i++) {
        const AVChannelLayout *ch_layout = NULL;
        int substream_count = 0;

        ret = avformat_iamf_audio_element_add_layer(stg->params.iamf_audio_element, NULL);
        if (ret < 0)
            return ret;

        for (int j = 0; j < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); j++) {
            if (channel_count >= ff_iamf_scalable_ch_layouts[j].nb_channels)
                continue;
            if (ff_iamf_scalable_ch_layouts[j].nb_channels < s->streams[0]->codecpar->ch_layout.nb_channels)
                continue;
            ch_layout = &ff_iamf_scalable_ch_layouts[j];
            break;
        }
        av_assert0(ch_layout);
        av_channel_layout_copy(&stg->params.iamf_audio_element->layers[i]->ch_layout, ch_layout);
        for (int j = k; j < s->nb_streams; j++) {
            substream_count++;
            k++;
            channel_count += s->streams[j]->codecpar->ch_layout.nb_channels;
            if (channel_count == stg->params.iamf_audio_element->layers[i]->ch_layout.nb_channels)
                break;
        }
        stg->params.iamf_audio_element->layers[i]->substream_count = substream_count;
    }

    param = avformat_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_DEMIXING, NULL, 1, NULL, NULL);
    if (!param)
        return AVERROR(ENOMEM);
    stg->params.iamf_audio_element->demixing_info = param;

    param->parameter_id = 998;
    param->param_definition_mode = 0;
    param->parameter_rate = s->streams[0]->codecpar->sample_rate;
    param->duration = s->streams[0]->codecpar->frame_size;
    param->constant_subblock_duration = s->streams[0]->codecpar->frame_size;

    param = avformat_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN, NULL, 1, NULL, NULL);
    if (!param)
        return AVERROR(ENOMEM);
    stg->params.iamf_audio_element->recon_gain_info = param;

    param->parameter_id = 999;
    param->param_definition_mode = 0;
    param->parameter_rate = s->streams[0]->codecpar->sample_rate;
    param->duration = s->streams[0]->codecpar->frame_size;
    param->constant_subblock_duration = s->streams[0]->codecpar->frame_size;

    return 0;
}

// Temporary code to generate a mix presentation.
// They should be set by the caller, as this is not guaranteed to be right
static int iamf_generate_mix_presentations(AVFormatContext *s)
{
    IAMFMuxContext *const c = s->priv_data;
    AVIAMFMixPresentation *mix;
    FFStreamGroup *stgi;
    AVStreamGroup **stream_groups, *stg;
    int ret;

    stream_groups = av_realloc_array(c->stream_groups, c->nb_stream_groups + 1, sizeof(*c->stream_groups));
    if (!stream_groups)
        return AVERROR(ENOMEM);

    c->stream_groups = stream_groups;
    stgi = av_mallocz(sizeof(*stgi));
    if (!stgi)
        return AVERROR(ENOMEM);
    stg = &stgi->pub;
    stgi->fmtctx = s;

    c->stream_groups[c->nb_stream_groups] = stg;

    stg->id = c->nb_stream_groups;
    stg->index = c->nb_stream_groups++;
    stg->type = AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION;

    for (int i = 0; i < s->nb_streams; i++) {
        ret = avformat_stream_group_add_stream(stg, s->streams[i]);
        if (ret < 0)
            return ret;
    }

    stg->params.iamf_mix_presentation = av_mallocz(sizeof(AVIAMFMixPresentation));
    if (!stg->params.iamf_mix_presentation)
        return AVERROR(ENOMEM);

    mix = stg->params.iamf_mix_presentation;

    ret = avformat_iamf_mix_presentation_add_submix(mix, NULL);
    if (ret < 0)
        return ret;

    for (int i = 0; i < mix->num_submixes; i++) {
        AVIAMFSubmix *sub_mix = mix->submixes[i];
        AVStream *st = s->streams[c->stream_groups[0]->streams[0]->index];
        AVIAMFSubmixElement *submix_element;

        ret = avformat_iamf_submix_add_element(sub_mix, NULL);
        if (ret < 0)
            return ret;

        submix_element = sub_mix->elements[0];
        submix_element->audio_element = c->stream_groups[0];

        submix_element->element_mix_config = avformat_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, NULL, 0, NULL, NULL);
        if (!submix_element->element_mix_config)
            return AVERROR(ENOMEM);
        submix_element->element_mix_config->parameter_id = 1;
        submix_element->element_mix_config->param_definition_mode = 1;
        submix_element->element_mix_config->parameter_rate = st->codecpar->sample_rate;
        submix_element->element_mix_config->duration = st->codecpar->frame_size;
        submix_element->element_mix_config->constant_subblock_duration = st->codecpar->frame_size;

        for (int k = 0; k < c->stream_groups[0]->params.iamf_audio_element->num_layers; k++) {
            AVIAMFSubmixLayout *submix_layout;

            ret = avformat_iamf_submix_add_layout(sub_mix, NULL);
            if (ret < 0)
                return ret;

            submix_layout = sub_mix->layouts[k];
            submix_layout->layout_type = 2; // LOUDSPEAKERS_SS_CONVENTION
            av_channel_layout_copy(&submix_layout->sound_system, &c->stream_groups[0]->params.iamf_audio_element->layers[k]->ch_layout);
        }

        sub_mix->output_mix_config = avformat_iamf_param_definition_alloc(AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, NULL, 0, NULL, NULL);
        if (!sub_mix->output_mix_config)
            return AVERROR(ENOMEM);

        memcpy(sub_mix->output_mix_config, submix_element->element_mix_config, sizeof(*sub_mix->output_mix_config));
    }

    return 0;
}

static int update_extradata(AVFormatContext *s, IAMFCodecConfig *codec_config)
{
    GetBitContext gb;
    PutBitContext pb;
    int ret;

    switch(codec_config->codec_id) {
    case AV_CODEC_ID_OPUS:
        if (codec_config->extradata_size < 19)
            return AVERROR_INVALIDDATA;
        codec_config->extradata_size -= 8;
        memmove(codec_config->extradata, codec_config->extradata + 8, codec_config->extradata_size);
        AV_WB8(codec_config->extradata + 1, 2); // set channels to stereo
        break;
    case AV_CODEC_ID_FLAC: {
        uint8_t buf[13];

        init_put_bits(&pb, buf, sizeof(buf));
        ret = init_get_bits8(&gb, codec_config->extradata, codec_config->extradata_size);
        if (ret < 0)
            return ret;

        put_bits32(&pb, get_bits_long(&gb, 32)); // min/max blocksize
        put_bits64(&pb, 48, get_bits64(&gb, 48)); // min/max framesize
        put_bits(&pb, 20, get_bits(&gb, 20)); // samplerate
        skip_bits(&gb, 3);
        put_bits(&pb, 3, 1); // set channels to stereo
        ret = put_bits_left(&pb);
        put_bits(&pb, ret, get_bits(&gb, ret));
        flush_put_bits(&pb);

        memcpy(codec_config->extradata, buf, sizeof(buf));
        break;
    }
    default:
        break;
    }

    return 0;
}

static int fill_codec_config(AVFormatContext *s, const AVStreamGroup *stg,
                             IAMFCodecConfig *codec_config)
{
    const AVIAMFAudioElement *iamf = stg->params.iamf_audio_element;
    const AVStream *st = stg->streams[0];
    int ret;

    av_freep(&codec_config->extradata);
    codec_config->extradata_size = 0;

    codec_config->codec_config_id = iamf->codec_config_id;
    codec_config->codec_id = st->codecpar->codec_id;
    codec_config->sample_rate = st->codecpar->sample_rate;
    codec_config->codec_tag = st->codecpar->codec_tag;
    codec_config->nb_samples = st->codecpar->frame_size;
    codec_config->seek_preroll = st->codecpar->seek_preroll;
    if (st->codecpar->extradata_size) {
        codec_config->extradata = av_memdup(st->codecpar->extradata, st->codecpar->extradata_size);
        if (!codec_config->extradata)
            return AVERROR(ENOMEM);
        codec_config->extradata_size = st->codecpar->extradata_size;
        ret = update_extradata(s, codec_config);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static IAMFParamDefinition *get_param_definition(AVFormatContext *s, unsigned int parameter_id)
{
    const IAMFMuxContext *const c = s->priv_data;
    IAMFParamDefinition *param_definition = NULL;

    for (int i = 0; i < c->nb_param_definitions; i++)
        if (c->param_definitions[i].param->parameter_id == parameter_id) {
            param_definition = &c->param_definitions[i];
            break;
        }

    return param_definition;
}

static IAMFParamDefinition *add_param_definition(AVFormatContext *s, const AVIAMFParamDefinition *param)
{
    IAMFMuxContext *const c = s->priv_data;
    IAMFParamDefinition *param_definition = av_dynarray2_add_nofree((void **)&c->param_definitions,
                                                                    &c->nb_param_definitions,
                                                                    sizeof(*c->param_definitions), NULL);
    if (!param_definition)
        return NULL;
    param_definition->param = param;
    param_definition->audio_element = NULL;

    return param_definition;
}

static int iamf_init(AVFormatContext *s)
{
    IAMFMuxContext *const c = s->priv_data;
    int nb_audio_elements = 0, nb_mix_presentations = 0;
    int stream_id = 0, ret;

    if (!s->nb_streams) {
        av_log(s, AV_LOG_ERROR, "There must be at least one stream\n");
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
            (s->streams[i]->codecpar->codec_tag != MKTAG('m','p','4','a') &&
             s->streams[i]->codecpar->codec_tag != MKTAG('O','p','u','s') &&
             s->streams[i]->codecpar->codec_tag != MKTAG('f','L','a','C') &&
             s->streams[i]->codecpar->codec_tag != MKTAG('i','p','c','m'))) {
            av_log(s, AV_LOG_ERROR, "Unsupported codec id %s\n",
                   avcodec_get_name(s->streams[i]->codecpar->codec_id));
            return AVERROR(EINVAL);
        }

        if (s->streams[i]->codecpar->ch_layout.nb_channels > 2) {
            av_log(s, AV_LOG_ERROR, "Unsupported channel layout on stream #%d\n", i);
            return AVERROR(EINVAL);
        }

        if (!s->streams[i]->id)
            s->streams[i]->id = ++stream_id;
    }

    if (s->nb_stream_groups) {
        c->stream_groups = s->stream_groups;
        c->nb_stream_groups = s->nb_stream_groups;
    } else {
        av_log(s, AV_LOG_WARNING, "No stream groups. Making some up. The output may not "
                                  "accurately represent the input\n");
        ret = iamf_generate_stream_group(s);
        if (ret < 0)
            return ret;

        ret = iamf_generate_mix_presentations(s);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < c->nb_stream_groups; i++) {
        AVStreamGroup *stg = c->stream_groups[i];

        if (stg->type == AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT)
            nb_audio_elements++;
        if (stg->type == AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION)
            nb_mix_presentations++;
    }
    if ((nb_audio_elements < 1 && nb_audio_elements > 2) || nb_mix_presentations < 1) {
        av_log(s, AV_LOG_ERROR, "There must be >= 1 and <= 2 IAMF_AUDIO_ELEMENT and at least one IAMF_MIX_PRESENTATION stream groups\n");
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < c->nb_stream_groups; i++) {
        const AVStreamGroup *stg = c->stream_groups[i];
        const AVIAMFLayer *layer;
        IAMFAudioElement *audio_element;
        IAMFCodecConfig *codec_config = NULL;

        if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT)
            continue;

        if (stg->params.iamf_audio_element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE) {
            if (stg->params.iamf_audio_element->num_layers != 1) {
                av_log(s, AV_LOG_ERROR, "Invalid amount of layers for SCENE_BASED audio element. Must be 1\n");
                return AVERROR(EINVAL);
            }
            layer = stg->params.iamf_audio_element->layers[0];
            if (layer->ch_layout.order != AV_CHANNEL_ORDER_CUSTOM &&
                layer->ch_layout.order != AV_CHANNEL_ORDER_AMBISONIC) {
                av_log(s, AV_LOG_ERROR, "Invalid channel layout for SCENE_BASED audio element\n");
                return AVERROR(EINVAL);
            }
        } else
            for (int k, j = 0; j < stg->params.iamf_audio_element->num_layers; j++) {
                layer = stg->params.iamf_audio_element->layers[j];
                for (k = 0; k < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); k++)
                    if (!av_channel_layout_compare(&layer->ch_layout, &ff_iamf_scalable_ch_layouts[k]))
                        break;

                if (k >= FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts)) {
                    av_log(s, AV_LOG_ERROR, "Unsupported channel layout in stream group #%d\n", i);
                    return AVERROR(EINVAL);
                }
            }

        for (int j = 0; j < c->nb_codec_configs; j++) {
            if (c->codec_configs[i].codec_config_id == stg->params.iamf_audio_element->codec_config_id) {
                codec_config = &c->codec_configs[i];
                break;
            }
        }

        if (!codec_config) {
            codec_config = av_dynarray2_add_nofree((void **)&c->codec_configs, &c->nb_codec_configs,
                                                   sizeof(*c->codec_configs), NULL);
            if (!codec_config)
                return AVERROR(ENOMEM);
            memset(codec_config, 0, sizeof(*codec_config));

        }

        ret = fill_codec_config(s, stg, codec_config);
        if (ret < 0)
            return ret;

        audio_element = av_dynarray2_add_nofree((void **)&c->audio_elements, &c->nb_audio_elements,
                                               sizeof(*c->audio_elements), NULL);
        if (!audio_element)
            return AVERROR(ENOMEM);
        memset(audio_element, 0, sizeof(*audio_element));

        audio_element->stream_group = stg;
        audio_element->codec_config = codec_config;
        audio_element->audio_substreams = av_calloc(stg->nb_streams, sizeof(*audio_element->audio_substreams));
        if (!audio_element->audio_substreams)
            return AVERROR(ENOMEM);

        if (stg->params.iamf_audio_element->demixing_info) {
            const AVIAMFParamDefinition *param = stg->params.iamf_audio_element->demixing_info;
            IAMFParamDefinition *param_definition = get_param_definition(s, param->parameter_id);

            if (param->num_subblocks != 1) {
                av_log(s, AV_LOG_ERROR, "num_subblocks in demixing_info for stream group %u is not 1\n", stg->index);
                return AVERROR(EINVAL);
            }
            if (!param_definition) {
                param_definition = add_param_definition(s, param);
                if (!param_definition)
                    return AVERROR(ENOMEM);
            }
            param_definition->audio_element = stg->params.iamf_audio_element;
        }
        if (stg->params.iamf_audio_element->recon_gain_info) {
            const AVIAMFParamDefinition *param = stg->params.iamf_audio_element->recon_gain_info;
            IAMFParamDefinition *param_definition = get_param_definition(s, param->parameter_id);

            if (param->num_subblocks != 1) {
                av_log(s, AV_LOG_ERROR, "num_subblocks in recon_gain_info for stream group %u is not 1\n", stg->index);
                return AVERROR(EINVAL);
            }

            if (!param_definition) {
                param_definition = add_param_definition(s, param);
                if (!param_definition)
                    return AVERROR(ENOMEM);
            }
            param_definition->audio_element = stg->params.iamf_audio_element;
        }

        for (int j = 0; j < stg->nb_streams; j++) {
            if (stg->params.iamf_audio_element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE &&
                stg->streams[j]->codecpar->ch_layout.nb_channels > 1) {
                av_log(s, AV_LOG_ERROR, "PROJECTION mode ambisonics not supported\n");
                return AVERROR_PATCHWELCOME;
            }

            audio_element->audio_substreams[j] = stg->streams[j];
        }
        audio_element->num_substreams = stg->nb_streams;
    }

    for (int i = 0; i < c->nb_stream_groups; i++) {
        const AVStreamGroup *stg = c->stream_groups[i];
        IAMFMixPresentation *mix_presentation;

        if (stg->type != AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION)
            continue;

        mix_presentation = av_dynarray2_add_nofree((void **)&c->mix_presentations, &c->nb_mix_presentations,
                                                   sizeof(*c->mix_presentations), NULL);
        if (!mix_presentation)
            return AVERROR(ENOMEM);
        memset(mix_presentation, 0, sizeof(*mix_presentation));

        mix_presentation->stream_group = stg;

        for (int i = 0; i < stg->params.iamf_mix_presentation->num_submixes; i++) {
            AVIAMFSubmix *submix = stg->params.iamf_mix_presentation->submixes[i];
            const AVIAMFParamDefinition *param = submix->output_mix_config;
            IAMFParamDefinition *param_definition;

            if (!param) {
                av_log(s, AV_LOG_ERROR, "output_mix_config is not present in submix %u from Mix Presentation ID %"PRId64"\n", i, stg->id);
                return AVERROR(EINVAL);
            }

            param_definition = get_param_definition(s, param->parameter_id);
            if (!param_definition) {
                param_definition = add_param_definition(s, param);
                if (!param_definition)
                    return AVERROR(ENOMEM);
            }

            for (int j = 0; j < submix->num_elements; j++) {
                AVIAMFSubmixElement *element = submix->elements[j];
                param = element->element_mix_config;

                if (!param) {
                    av_log(s, AV_LOG_ERROR, "element_mix_config is not present for element %u in submix %u from Mix Presentation ID %"PRId64"\n", j, i, stg->id);
                    return AVERROR(EINVAL);
                }
                param_definition = get_param_definition(s, param->parameter_id);
                if (!param_definition) {
                    param_definition = add_param_definition(s, param);
                    if (!param_definition)
                        return AVERROR(ENOMEM);
                }
                param_definition->audio_element = element->audio_element->params.iamf_audio_element;
            }
        }
    }

    c->first_stream_id = s->streams[0]->id;

    return 0;
}

static void leb(AVIOContext *pb, unsigned value)
{
    int len, i;
    uint8_t byte;

    len = (av_log2(value) + 7) / 7;

    for (i = 0; i < len; i++) {
        byte = value >> (7 * i) & 0x7f;
        if (i < len - 1)
            byte |= 0x80;

        avio_w8(pb, byte);
    }
}

static int iamf_write_codec_config(AVFormatContext *s, const IAMFCodecConfig *codec_config)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    PutBitContext pb;
    int dyn_size;

    int ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    leb(dyn_bc, codec_config->codec_config_id);
    avio_wl32(dyn_bc, codec_config->codec_tag);

    leb(dyn_bc, codec_config->nb_samples);
    avio_wb16(dyn_bc, codec_config->seek_preroll);

    switch(codec_config->codec_id) {
    case AV_CODEC_ID_OPUS:
        avio_write(dyn_bc, codec_config->extradata, codec_config->extradata_size);
        break;
    case AV_CODEC_ID_AAC:
        return AVERROR_PATCHWELCOME;
    case AV_CODEC_ID_FLAC:
        avio_w8(dyn_bc, 0x80);
        avio_wb24(dyn_bc, codec_config->extradata_size);
        avio_write(dyn_bc, codec_config->extradata, codec_config->extradata_size);
        break;
    case AV_CODEC_ID_PCM_S16LE:
        avio_w8(dyn_bc, 0);
        avio_w8(dyn_bc, 16);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S24LE:
        avio_w8(dyn_bc, 0);
        avio_w8(dyn_bc, 24);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S32LE:
        avio_w8(dyn_bc, 0);
        avio_w8(dyn_bc, 32);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S16BE:
        avio_w8(dyn_bc, 1);
        avio_w8(dyn_bc, 16);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S24BE:
        avio_w8(dyn_bc, 1);
        avio_w8(dyn_bc, 24);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    case AV_CODEC_ID_PCM_S32BE:
        avio_w8(dyn_bc, 1);
        avio_w8(dyn_bc, 32);
        avio_wb32(dyn_bc, codec_config->sample_rate);
        break;
    default:
        break;
    }

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 5, IAMF_OBU_IA_CODEC_CONFIG);
    put_bits(&pb, 3, 0);
    flush_put_bits(&pb);

    dyn_size = avio_close_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(s->pb, header, put_bytes_count(&pb, 1));
    leb(s->pb, dyn_size);
    avio_write(s->pb, dyn_buf, dyn_size);
    av_free(dyn_buf);

    return 0;
}

static inline int rescale_rational(AVRational q, int b)
{
    return av_clip_int16(av_rescale(q.num, b, q.den));
}

static int scalable_channel_layout_config(AVFormatContext *s, AVIOContext *dyn_bc,
                                          const IAMFAudioElement *audio_element)
{
    const AVStreamGroup *stg = audio_element->stream_group;
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    PutBitContext pb;

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 3, stg->params.iamf_audio_element->num_layers);
    put_bits(&pb, 5, 0);
    flush_put_bits(&pb);
    avio_write(dyn_bc, header, put_bytes_count(&pb, 1));
    for (int i = 0, k = 0; i < stg->params.iamf_audio_element->num_layers; i++) {
        AVIAMFLayer *layer = stg->params.iamf_audio_element->layers[i];
        int coupled_substream_count = 0, layout;
        for (layout = 0; layout < FF_ARRAY_ELEMS(ff_iamf_scalable_ch_layouts); layout++) {
            if (!av_channel_layout_compare(&layer->ch_layout, &ff_iamf_scalable_ch_layouts[layout]))
                break;
        }
        init_put_bits(&pb, header, sizeof(header));
        put_bits(&pb, 4, layout);
        put_bits(&pb, 1, !!layer->output_gain_flags);
        put_bits(&pb, 1, layer->recon_gain_is_present);
        put_bits(&pb, 2, 0); // reserved
        put_bits(&pb, 8, layer->substream_count);
        for (int j = 0; j < layer->substream_count; j++) {
            if (audio_element->audio_substreams[k++]->codecpar->ch_layout.nb_channels == 2)
                coupled_substream_count++;
        }
        put_bits(&pb, 8, coupled_substream_count);
       // av_log(s, AV_LOG_WARNING, "k %d, substream_count %d, coupled_substream_count %d\n", k, layer->substream_count, coupled_substream_count);
        if (layer->output_gain_flags) {
            put_bits(&pb, 6, layer->output_gain_flags);
            put_bits(&pb, 2, 0);
            put_bits(&pb, 16, rescale_rational(layer->output_gain, 1 << 8));
        }
        flush_put_bits(&pb);
        avio_write(dyn_bc, header, put_bytes_count(&pb, 1));
    }

    return 0;
}

static int ambisonics_config(AVFormatContext *s, AVIOContext *dyn_bc,
                             const IAMFAudioElement *audio_element)
{
    const AVStreamGroup *stg = audio_element->stream_group;
    AVIAMFLayer *layer = stg->params.iamf_audio_element->layers[0];

    leb(dyn_bc, 0); // ambisonics_mode
    leb(dyn_bc, layer->ch_layout.nb_channels); // output_channel_count
    leb(dyn_bc, stg->nb_streams); // substream_count

    if (layer->ch_layout.order == AV_CHANNEL_ORDER_AMBISONIC)
        for (int i = 0; i < layer->ch_layout.nb_channels; i++)
            avio_w8(dyn_bc, i);
    else
        for (int i = 0; i < layer->ch_layout.nb_channels; i++)
            avio_w8(dyn_bc, layer->ch_layout.u.map[i].id);

    return 0;
}

static int param_definition(AVFormatContext *s, AVIOContext *dyn_bc,
                            AVIAMFParamDefinition *param)
{
    leb(dyn_bc, param->parameter_id);
    leb(dyn_bc, param->parameter_rate);
    avio_w8(dyn_bc, !!param->param_definition_mode << 7); // param_definition_mode
    if (!param->param_definition_mode) {
        leb(dyn_bc, param->duration); // duration
        leb(dyn_bc, param->constant_subblock_duration); // constant_subblock_duration
        if (param->constant_subblock_duration == 0) {
            leb(dyn_bc, param->num_subblocks);
            for (int i = 0; i < param->num_subblocks; i++) {
                const void *subblock = avformat_iamf_param_definition_get_subblock(param, i);

                switch (param->param_definition_type) {
                case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
                    const AVIAMFMixGainParameterData *mix = subblock;
                    leb(dyn_bc, mix->subblock_duration);
                    break;
                }
                case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
                    const AVIAMFDemixingInfoParameterData *demix = subblock;
                    leb(dyn_bc, demix->subblock_duration);
                    break;
                }
                case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
                    const AVIAMFReconGainParameterData *recon = subblock;
                    leb(dyn_bc, recon->subblock_duration);
                    break;
                }
                }
            }
        }
    }

    return 0;
}

static int iamf_write_audio_element(AVFormatContext *s, const IAMFAudioElement *audio_element)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    const AVStreamGroup *stg = audio_element->stream_group;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    PutBitContext pb;
    int param_definition_types = AV_IAMF_PARAMETER_DEFINITION_DEMIXING, dyn_size;

    int ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    leb(dyn_bc, stg->id);

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 3, stg->params.iamf_audio_element->audio_element_type);
    put_bits(&pb, 5, 0);
    flush_put_bits(&pb);
    avio_write(dyn_bc, header, put_bytes_count(&pb, 1));

    leb(dyn_bc, audio_element->codec_config->codec_config_id);
    leb(dyn_bc, audio_element->num_substreams);

    for (int i = 0; i < audio_element->num_substreams; i++)
        leb(dyn_bc, audio_element->audio_substreams[i]->id);

    if (stg->params.iamf_audio_element->num_layers == 1)
        param_definition_types &= ~AV_IAMF_PARAMETER_DEFINITION_DEMIXING;
    if (stg->params.iamf_audio_element->num_layers > 1)
        param_definition_types |= AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN;
    if (audio_element->codec_config->codec_tag == MKTAG('f','L','a','C') ||
        audio_element->codec_config->codec_tag == MKTAG('i','p','c','m'))
        param_definition_types &= ~AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN;

    leb(dyn_bc, av_popcount(param_definition_types)); // num_parameters

    if (param_definition_types & 1) {
        AVIAMFParamDefinition *param = stg->params.iamf_audio_element->demixing_info;
        AVIAMFDemixingInfoParameterData *demix;

        if (!param) {
            av_log(s, AV_LOG_ERROR, "demixing_info needed but not set in Stream Group #%"PRId64"\n", stg->id);
            return AVERROR(EINVAL);
        }

        demix = avformat_iamf_param_definition_get_subblock(param, 0);
        leb(dyn_bc, AV_IAMF_PARAMETER_DEFINITION_DEMIXING); // param_definition_type
        param_definition(s, dyn_bc, param);

        avio_w8(dyn_bc, demix->dmixp_mode << 5); // dmixp_mode
        avio_w8(dyn_bc, stg->params.iamf_audio_element->default_w << 4); // default_w
    }
    if (param_definition_types & 2) {
        AVIAMFParamDefinition *param = stg->params.iamf_audio_element->recon_gain_info;

        if (!param) {
            av_log(s, AV_LOG_ERROR, "recon_gain_info needed but not set in Stream Group #%"PRId64"\n", stg->id);
            return AVERROR(EINVAL);
        }
        leb(dyn_bc, AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN); // param_definition_type
        param_definition(s, dyn_bc, param);
    }

    if (stg->params.iamf_audio_element->audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL) {
        ret = scalable_channel_layout_config(s, dyn_bc, audio_element);
        if (ret < 0)
            return ret;
    } else {
        ret = ambisonics_config(s, dyn_bc, audio_element);
        if (ret < 0)
            return ret;
    }

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 5, IAMF_OBU_IA_AUDIO_ELEMENT);
    put_bits(&pb, 3, 0);
    flush_put_bits(&pb);

    dyn_size = avio_close_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(s->pb, header, put_bytes_count(&pb, 1));
    leb(s->pb, dyn_size);
    avio_write(s->pb, dyn_buf, dyn_size);
    av_free(dyn_buf);

    return 0;
}

static int iamf_write_mixing_presentation(AVFormatContext *s, const IAMFMixPresentation *mix_presentation)
{
    IAMFMuxContext *const c = s->priv_data;
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    const AVStreamGroup *stg = mix_presentation->stream_group;
    AVIAMFMixPresentation *mix = mix_presentation->stream_group->params.iamf_mix_presentation;
    const AVDictionaryEntry *tag = NULL;
    PutBitContext pb;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    int dyn_size;

    int ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    leb(dyn_bc, stg->id); // mix_presentation_id
    leb(dyn_bc, av_dict_count(mix->annotations)); // count_label

    while ((tag = av_dict_iterate(mix->annotations, tag)))
        avio_put_str(dyn_bc, tag->key);
    while ((tag = av_dict_iterate(mix->annotations, tag)))
        avio_put_str(dyn_bc, tag->value);

    leb(dyn_bc, mix->num_submixes);
    for (int i = 0; i < mix->num_submixes; i++) {
        const AVIAMFSubmix *sub_mix = mix->submixes[i];

        leb(dyn_bc, sub_mix->num_elements);
        for (int j = 0; j < sub_mix->num_elements; j++) {
            const IAMFAudioElement *audio_element = NULL;
            const AVIAMFSubmixElement *submix_element = sub_mix->elements[j];

            for (int k = 0; k < c->nb_audio_elements; k++)
                if (c->audio_elements[k].stream_group->index == submix_element->audio_element->index)
                    audio_element = &c->audio_elements[k];

            av_assert0(audio_element);
            leb(dyn_bc, audio_element->stream_group->id);

            if (av_dict_count(submix_element->annotations) != av_dict_count(mix->annotations)) {
                av_log(s, AV_LOG_ERROR, "Inconsistent amount of labels in submix %d from Mix Presentation id #%"PRId64"\n", j, stg->id);
                return AVERROR(EINVAL);
            }
            while ((tag = av_dict_iterate(submix_element->annotations, tag)))
                avio_put_str(dyn_bc, tag->value);

            init_put_bits(&pb, header, sizeof(header));
            put_bits(&pb, 2, submix_element->headphones_rendering_mode);
            put_bits(&pb, 6, 0); // reserved
            flush_put_bits(&pb);
            avio_write(dyn_bc, header, put_bytes_count(&pb, 1));
            leb(dyn_bc, 0); // rendering_config_extension_size
            param_definition(s, dyn_bc, submix_element->element_mix_config);
            avio_wb16(dyn_bc, rescale_rational(submix_element->default_mix_gain, 1 << 8));
        }
        param_definition(s, dyn_bc, sub_mix->output_mix_config);
        avio_wb16(dyn_bc, rescale_rational(sub_mix->default_mix_gain, 1 << 8));

        leb(dyn_bc, sub_mix->num_layouts); // num_layouts
        for (int i = 0; i < sub_mix->num_layouts; i++) {
            AVIAMFSubmixLayout *submix_layout = sub_mix->layouts[i];
            int layout, info_type;

            if (layout == FF_ARRAY_ELEMS(ff_iamf_sound_system_map)) {
                av_log(s, AV_LOG_ERROR, "Invalid Sound System value in a submix\n");
                return AVERROR(EINVAL);
            }

            if (submix_layout->layout_type == AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS) {
                for (layout = 0; layout < FF_ARRAY_ELEMS(ff_iamf_sound_system_map); layout++) {
                    if (!av_channel_layout_compare(&submix_layout->sound_system, &ff_iamf_sound_system_map[layout].layout))
                        break;
                }
                if (layout == FF_ARRAY_ELEMS(ff_iamf_sound_system_map)) {
                    av_log(s, AV_LOG_ERROR, "Invalid Sound System value in a submix\n");
                    return AVERROR(EINVAL);
                }
            }
            init_put_bits(&pb, header, sizeof(header));
            put_bits(&pb, 2, submix_layout->layout_type); // layout_type
            if (submix_layout->layout_type == AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS) {
                put_bits(&pb, 4, ff_iamf_sound_system_map[layout].id); // sound_system
                put_bits(&pb, 2, 0); // reserved
            } else
                put_bits(&pb, 6, 0); // reserved
            flush_put_bits(&pb);
            avio_write(dyn_bc, header, put_bytes_count(&pb, 1));

            info_type = (submix_layout->true_peak.num && submix_layout->true_peak.den);
            avio_w8(dyn_bc, info_type);
            avio_wb16(dyn_bc, rescale_rational(submix_layout->integrated_loudness, 1 << 8));
            avio_wb16(dyn_bc, rescale_rational(submix_layout->digital_peak, 1 << 8));
            if (info_type)
                avio_wb16(dyn_bc, rescale_rational(submix_layout->true_peak, 1 << 8));
        }
    }

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 5, IAMF_OBU_IA_MIX_PRESENTATION);
    put_bits(&pb, 3, 0);
    flush_put_bits(&pb);

    dyn_size = avio_close_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(s->pb, header, put_bytes_count(&pb, 1));
    leb(s->pb, dyn_size);
    avio_write(s->pb, dyn_buf, dyn_size);
    av_free(dyn_buf);

    return 0;
}

static int iamf_write_header(AVFormatContext *s)
{
    IAMFMuxContext *const c = s->priv_data;
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    PutBitContext pb;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    int dyn_size;

    int ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    // Sequence Header
    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 5, IAMF_OBU_IA_SEQUENCE_HEADER);
    put_bits(&pb, 3, 0);
    flush_put_bits(&pb);

    avio_write(dyn_bc, header, put_bytes_count(&pb, 1));
    leb(dyn_bc, 6);
    avio_wb32(dyn_bc, MKBETAG('i','a','m','f'));
    avio_w8(dyn_bc, c->nb_audio_elements > 1); // primary_profile
    avio_w8(dyn_bc, c->nb_audio_elements > 1); // additional_profile

    dyn_size = avio_close_dyn_buf(dyn_bc, &dyn_buf);
    avio_write(s->pb, dyn_buf, dyn_size);
    av_free(dyn_buf);

    for (int i; i < c->nb_codec_configs; i++) {
        ret = iamf_write_codec_config(s, &c->codec_configs[i]);
        if (ret < 0)
            return ret;
    }

    for (int i; i < c->nb_audio_elements; i++) {
        ret = iamf_write_audio_element(s, &c->audio_elements[i]);
        if (ret < 0)
            return ret;
    }

    for (int i; i < c->nb_mix_presentations; i++) {
        ret = iamf_write_mixing_presentation(s, &c->mix_presentations[i]);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int write_parameter_block(AVFormatContext *s, AVIAMFParamDefinition *param)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    IAMFParamDefinition *param_definition = get_param_definition(s, param->parameter_id);
    PutBitContext pb;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    int dyn_size, ret;

    if (param->param_definition_type > AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN) {
        av_log(s, AV_LOG_DEBUG, "Ignoring side data with unknown param_definition_type %u\n",
               param->param_definition_type);
        return 0;
    }

    if (!param_definition) {
        av_log(s, AV_LOG_ERROR, "Non-existent Parameter Definition with ID %u referenced by a packet\n",
               param->parameter_id);
        return AVERROR(EINVAL);
    }

    if (param->param_definition_type != param_definition->param->param_definition_type ||
        param->param_definition_mode != param_definition->param->param_definition_mode) {
        av_log(s, AV_LOG_ERROR, "Inconsistent param_definition_mode or param_definition_type values "
                                "for Parameter Definition with ID %u in a packet\n",
               param->parameter_id);
        return AVERROR(EINVAL);
    }

    ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    // Sequence Header
    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 5, IAMF_OBU_IA_PARAMETER_BLOCK);
    put_bits(&pb, 3, 0);
    flush_put_bits(&pb);
    avio_write(s->pb, header, put_bytes_count(&pb, 1));

    leb(dyn_bc, param->parameter_id);
    if (param->param_definition_mode) {
        leb(dyn_bc, param->duration);
        leb(dyn_bc, param->constant_subblock_duration);
        if (param->constant_subblock_duration == 0)
            leb(dyn_bc, param->num_subblocks);
    }

    for (int i = 0; i < param->num_subblocks; i++) {
        const void *subblock = avformat_iamf_param_definition_get_subblock(param, i);

        switch (param->param_definition_type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
            const AVIAMFMixGainParameterData *mix = subblock;
            if (param->param_definition_mode && param->constant_subblock_duration == 0)
                leb(dyn_bc, mix->subblock_duration);

            leb(dyn_bc, mix->animation_type);

            avio_wb16(dyn_bc, rescale_rational(mix->start_point_value, 1 << 8));
            if (mix->animation_type >= AV_IAMF_ANIMATION_TYPE_LINEAR)
                avio_wb16(dyn_bc, rescale_rational(mix->end_point_value, 1 << 8));
            if (mix->animation_type == AV_IAMF_ANIMATION_TYPE_BEZIER) {
                avio_wb16(dyn_bc, rescale_rational(mix->control_point_value, 1 << 8));
                avio_w8(dyn_bc, mix->control_point_relative_time);
            }
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
            const AVIAMFDemixingInfoParameterData *demix = subblock;
            if (param->param_definition_mode && param->constant_subblock_duration == 0)
                leb(dyn_bc, demix->subblock_duration);

            avio_w8(dyn_bc, demix->dmixp_mode << 5);
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
            const AVIAMFReconGainParameterData *recon = subblock;
            const AVIAMFAudioElement *audio_element = param_definition->audio_element;

            if (param->param_definition_mode && param->constant_subblock_duration == 0)
                leb(dyn_bc, recon->subblock_duration);

            if (!audio_element) {
                av_log(s, AV_LOG_ERROR, "Invalid Parameter Definition with ID %u referenced by a packet\n", param->parameter_id);
                return AVERROR(EINVAL);
            }

            for (int j = 0; j < audio_element->num_layers; j++) {
                const AVIAMFLayer *layer = audio_element->layers[j];

                if (layer->recon_gain_is_present) {
                    unsigned int recon_gain_flags = 0;
                    int k = 0;

                    for (; k < 7; k++)
                        recon_gain_flags |= (1 << k) * !!recon->recon_gain[j][k];
                    if (recon_gain_flags >> 8)
                        recon_gain_flags |= (1 << k);
                    for (; k < 12; k++)
                        recon_gain_flags |= (2 << k) * !!recon->recon_gain[j][k];

                    leb(dyn_bc, recon_gain_flags);
                    for (k = 0; k < 12; k++) {
                        if (recon->recon_gain[j][k])
                            avio_w8(dyn_bc, recon->recon_gain[j][k]);
                    }
                }
            }
            break;
        }
        default:
            av_assert0(0);
        }
    }

    dyn_size = avio_close_dyn_buf(dyn_bc, &dyn_buf);
    leb(s->pb, dyn_size);
    avio_write(s->pb, dyn_buf, dyn_size);
    av_free(dyn_buf);

    return 0;
}

static int iamf_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    const IAMFMuxContext *const c = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE];
    PutBitContext pb;
    AVIOContext *dyn_bc;
    uint8_t *dyn_buf = NULL;
    int dyn_size;
    int ret, type = st->id <= 17 ? st->id + IAMF_OBU_IA_AUDIO_FRAME_ID0 : IAMF_OBU_IA_AUDIO_FRAME;

    if (s->nb_stream_groups && st->id == c->first_stream_id) {
        AVIAMFParamDefinition *mix =
            (AVIAMFParamDefinition *)av_packet_get_side_data(pkt, AV_PKT_DATA_IAMF_MIX_GAIN_PARAM, NULL);
        AVIAMFParamDefinition *demix =
            (AVIAMFParamDefinition *)av_packet_get_side_data(pkt, AV_PKT_DATA_IAMF_DEMIXING_INFO_PARAM, NULL);
        AVIAMFParamDefinition *recon =
            (AVIAMFParamDefinition *)av_packet_get_side_data(pkt, AV_PKT_DATA_IAMF_RECON_GAIN_INFO_PARAM, NULL);

        if (mix) {
            ret = write_parameter_block(s, mix);
            if (ret < 0)
               return ret;
        }
        if (demix) {
            ret = write_parameter_block(s, demix);
            if (ret < 0)
               return ret;
        }
        if (recon) {
            ret = write_parameter_block(s, recon);
            if (ret < 0)
               return ret;
        }
    }

    ret = avio_open_dyn_buf(&dyn_bc);
    if (ret < 0)
        return ret;

    init_put_bits(&pb, header, sizeof(header));
    put_bits(&pb, 5, type);
    put_bits(&pb, 3, 0);
    flush_put_bits(&pb);
    avio_write(s->pb, header, put_bytes_count(&pb, 1));

    if (st->id > 17)
        leb(dyn_bc, st->id);

    dyn_size = avio_close_dyn_buf(dyn_bc, &dyn_buf);
    leb(s->pb, dyn_size + pkt->size);
    avio_write(s->pb, dyn_buf, dyn_size);
    av_free(dyn_buf);
    avio_write(s->pb, pkt->data, pkt->size);

    return 0;
}

static void iamf_deinit(AVFormatContext *s)
{
    IAMFMuxContext *const c = s->priv_data;

    if (!s->nb_stream_groups) {
        if (c->nb_stream_groups)
            ff_free_stream_group(&c->stream_groups[0]);
        av_free(c->stream_groups);
    }
    c->stream_groups = NULL;
    s->nb_stream_groups = 0;

    for (int i = 0; i < c->nb_codec_configs; i++)
        av_free(c->codec_configs[i].extradata);
    av_freep(&c->codec_configs);
    c->nb_codec_configs = 0;

    for (int i = 0; i < c->nb_audio_elements; i++)
        av_free(c->audio_elements[i].audio_substreams);
    av_freep(&c->audio_elements);
    c->nb_audio_elements = 0;

    av_freep(&c->mix_presentations);
    c->nb_mix_presentations = 0;

    av_freep(&c->param_definitions);
    c->nb_param_definitions = 0;

    return;
}

static const AVCodecTag iamf_codec_tags[] = {
    { AV_CODEC_ID_AAC,       MKTAG('m','p','4','a') },
    { AV_CODEC_ID_FLAC,      MKTAG('f','L','a','C') },
    { AV_CODEC_ID_OPUS,      MKTAG('O','p','u','s') },
    { AV_CODEC_ID_PCM_S16LE, MKTAG('i','p','c','m') },
    { AV_CODEC_ID_PCM_S16BE, MKTAG('i','p','c','m') },
    { AV_CODEC_ID_PCM_S24LE, MKTAG('i','p','c','m') },
    { AV_CODEC_ID_PCM_S24BE, MKTAG('i','p','c','m') },
    { AV_CODEC_ID_PCM_S32LE, MKTAG('i','p','c','m') },
    { AV_CODEC_ID_PCM_S32BE, MKTAG('i','p','c','m') },
    { AV_CODEC_ID_NONE,      MKTAG('i','p','c','m') }
};

const FFOutputFormat ff_iamf_muxer = {
    .p.name            = "iamf",
    .p.long_name       = NULL_IF_CONFIG_SMALL("Raw Immersive Audio Model and Formats"),
    .p.extensions      = "iamf",
    .priv_data_size    = sizeof(IAMFMuxContext),
    .p.audio_codec     = AV_CODEC_ID_OPUS,
    .init              = iamf_init,
    .deinit            = iamf_deinit,
    .write_header      = iamf_write_header,
    .write_packet      = iamf_write_packet,
    .p.codec_tag       = (const AVCodecTag* const []){ iamf_codec_tags, NULL },
    .p.flags           = AVFMT_GLOBALHEADER | AVFMT_NOTIMESTAMPS,
   // .p.priv_class      = &iamf_class,
};
