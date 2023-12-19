/*
 * Copyright (c) 2023 James Almer <jamrial@gmail.com>
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
#include "libavutil/opt.h"
#include "libavformat/iamf.h"
#include "bsf.h"
#include "bsf_internal.h"
#include "get_bits.h"
#include "leb.h"

typedef struct ParamDefinition {
    AVIAMFParamDefinition *param;
    size_t param_size;
    int mode;
    int recon_gain_present_bitmask;
} ParamDefinition;

typedef struct IAMFSplitContext {
    AVClass *class;
    AVPacket *buffer_pkt;

    ParamDefinition *param_definitions;
    unsigned int nb_param_definitions;

    unsigned int *ids;
    int nb_ids;

    // AVOptions
    int first_index;
    int inband_descriptors;

    // Packet side data
    AVIAMFParamDefinition *mix;
    size_t mix_size;
    AVIAMFParamDefinition *demix;
    size_t demix_size;
    AVIAMFParamDefinition *recon;
    size_t recon_size;
} IAMFSplitContext;

static int param_parse(AVBSFContext *ctx, GetBitContext *gb,
                       unsigned int type,
                       ParamDefinition **out)
{
    IAMFSplitContext *const c = ctx->priv_data;
    ParamDefinition *param_definition = NULL;
    AVIAMFParamDefinition *param;
    unsigned int parameter_id, parameter_rate, mode;
    unsigned int duration = 0, constant_subblock_duration = 0, nb_subblocks = 0;
    size_t param_size;

    parameter_id = get_leb(gb);

    for (int i = 0; i < c->nb_param_definitions; i++)
        if (c->param_definitions[i].param->parameter_id == parameter_id) {
            param_definition = &c->param_definitions[i];
            break;
        }

    parameter_rate = get_leb(gb);
    mode = get_bits(gb, 8) >> 7;

    if (mode == 0) {
        duration = get_leb(gb);
        constant_subblock_duration = get_leb(gb);
        if (constant_subblock_duration == 0) {
            nb_subblocks = get_leb(gb);
        } else
            nb_subblocks = duration / constant_subblock_duration;
    }

    param = av_iamf_param_definition_alloc(type, nb_subblocks, &param_size);
    if (!param)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_subblocks; i++) {
        if (constant_subblock_duration == 0)
            get_leb(gb); // subblock_duration

        switch (type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
            break;
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
            skip_bits(gb, 8); // dmixp_mode
            break;
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
            break;
        default:
            av_free(param);
            return AVERROR_INVALIDDATA;
        }
    }

    param->parameter_id = parameter_id;
    param->parameter_rate = parameter_rate;
    param->duration = duration;
    param->constant_subblock_duration = constant_subblock_duration;
    param->nb_subblocks = nb_subblocks;

    if (param_definition) {
        if (param_definition->param_size != param_size ||
            memcmp(param_definition->param, param, param_size)) {
            av_log(ctx, AV_LOG_ERROR, "Incosistent parameters for parameter_id %u\n",
                   parameter_id);
            av_free(param);
            return AVERROR_INVALIDDATA;
        }
        av_freep(&param);
    } else {
        ParamDefinition *tmp = av_realloc_array(c->param_definitions,
                                                c->nb_param_definitions + 1,
                                                sizeof(*c->param_definitions));
        if (!tmp) {
            av_free(param);
            return AVERROR(ENOMEM);
        }
        c->param_definitions = tmp;

        param_definition = &c->param_definitions[c->nb_param_definitions++];
        param_definition->param = param;
        param_definition->mode = !mode;
        param_definition->param_size = param_size;
    }
    if (out)
        *out = param_definition;

    return 0;
}

static int scalable_channel_layout_config(AVBSFContext *ctx, GetBitContext *gb,
                                          ParamDefinition *recon_gain)
{
    int nb_layers;

    nb_layers = get_bits(gb, 3);
    skip_bits(gb, 5); //reserved

    if (nb_layers > 6)
        return AVERROR_INVALIDDATA;

    for (int i = 0; i < nb_layers; i++) {
        int output_gain_is_present_flag, recon_gain_is_present;

        skip_bits(gb, 4); // loudspeaker_layout
        output_gain_is_present_flag = get_bits1(gb);
        recon_gain_is_present = get_bits1(gb);
        if (recon_gain)
            recon_gain->recon_gain_present_bitmask |= recon_gain_is_present << i;
        skip_bits(gb, 2); // reserved
        skip_bits(gb, 8); // substream_count
        skip_bits(gb, 8); // coupled_substream_count
        if (output_gain_is_present_flag) {
            skip_bits(gb, 8); // output_gain_flags & reserved
            skip_bits(gb, 16); // output_gain
        }
    }

    return 0;
}

static int audio_element_obu(AVBSFContext *ctx, uint8_t *buf, unsigned size)
{
    IAMFSplitContext *const c = ctx->priv_data;
    GetBitContext gb;
    ParamDefinition *recon_gain = NULL;
    unsigned audio_element_type;
    unsigned num_substreams, num_parameters;
    int ret;

    ret = init_get_bits8(&gb, buf, size);
    if (ret < 0)
        return ret;

    get_leb(&gb); // audio_element_id
    audio_element_type = get_bits(&gb, 3);
    skip_bits(&gb, 5); // reserved

    get_leb(&gb); // codec_config_id
    num_substreams = get_leb(&gb);
    for (unsigned i = 0; i < num_substreams; i++) {
        unsigned *audio_substream_id = av_dynarray2_add((void **)&c->ids, &c->nb_ids,
                                                        sizeof(*c->ids), NULL);
        if (!audio_substream_id)
            return AVERROR(ENOMEM);

        *audio_substream_id = get_leb(&gb);
    }

    num_parameters = get_leb(&gb);
    if (num_parameters && audio_element_type != 0) {
        av_log(ctx, AV_LOG_ERROR, "Audio Element parameter count %u is invalid"
                                  " for Scene representations\n", num_parameters);
        return AVERROR_INVALIDDATA;
    }

    for (int i = 0; i < num_parameters; i++) {
        unsigned type = get_leb(&gb);

        if (type == AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN)
            return AVERROR_INVALIDDATA;
        else if (type == AV_IAMF_PARAMETER_DEFINITION_DEMIXING) {
            ret = param_parse(ctx, &gb, type, NULL);
            if (ret < 0)
                return ret;
            skip_bits(&gb, 8); // default_w
        } else if (type == AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN) {
            ret = param_parse(ctx, &gb, type, &recon_gain);
            if (ret < 0)
                return ret;
        } else {
            unsigned param_definition_size = get_leb(&gb);
            skip_bits_long(&gb, param_definition_size * 8);
        }
    }

    if (audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL) {
        ret = scalable_channel_layout_config(ctx, &gb, recon_gain);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int label_string(GetBitContext *gb)
{
    int byte;

    do {
        byte = get_bits(gb, 8);
    } while (byte);

    return 0;
}

static int mix_presentation_obu(AVBSFContext *ctx, uint8_t *buf, unsigned size)
{
    GetBitContext gb;
    unsigned mix_presentation_id, count_label;
    unsigned nb_submixes, nb_elements;
    int ret;

    ret = init_get_bits8(&gb, buf, size);
    if (ret < 0)
        return ret;

    mix_presentation_id = get_leb(&gb);
    count_label = get_leb(&gb);

    for (int i = 0; i < count_label; i++) {
        ret = label_string(&gb);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < count_label; i++) {
        ret = label_string(&gb);
        if (ret < 0)
            return ret;
    }

    nb_submixes = get_leb(&gb);
    for (int i = 0; i < nb_submixes; i++) {
        unsigned nb_layouts;

        nb_elements = get_leb(&gb);

        for (int j = 0; j < nb_elements; j++) {
            unsigned rendering_config_extension_size;

            get_leb(&gb); // audio_element_id
            for (int k = 0; k < count_label; k++) {
                ret = label_string(&gb);
                if (ret < 0)
                    return ret;
            }

            skip_bits(&gb, 8); // headphones_rendering_mode & reserved
            rendering_config_extension_size = get_leb(&gb);
            skip_bits_long(&gb, rendering_config_extension_size * 8);

            ret = param_parse(ctx, &gb, AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, NULL);
            if (ret < 0)
                return ret;
            skip_bits(&gb, 16); // default_mix_gain
        }

        ret = param_parse(ctx, &gb, AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, NULL);
        if (ret < 0)
            return ret;
        get_bits(&gb, 16); // default_mix_gain

        nb_layouts = get_leb(&gb);
        for (int j = 0; j < nb_layouts; j++) {
            int info_type, layout_type;
            int byte = get_bits(&gb, 8);

            layout_type = byte >> 6;
            if (layout_type < AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS &&
                layout_type > AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL) {
                av_log(ctx, AV_LOG_ERROR, "Invalid Layout type %u in a submix from "
                                          "Mix Presentation %u\n",
                       layout_type, mix_presentation_id);
                return AVERROR_INVALIDDATA;
            }

            info_type = get_bits(&gb, 8);
            get_bits(&gb, 16); // integrated_loudness
            get_bits(&gb, 16); // digital_peak

            if (info_type & 1)
                get_bits(&gb, 16); // true_peak

            if (info_type & 2) {
                unsigned int num_anchored_loudness = get_bits(&gb, 8);

                for (int k = 0; k < num_anchored_loudness; k++) {
                    get_bits(&gb, 8); // anchor_element
                    get_bits(&gb, 16); // anchored_loudness
                }
            }

            if (info_type & 0xFC) {
                unsigned int info_type_size = get_leb(&gb);
                skip_bits_long(&gb, info_type_size * 8);
            }
        }
    }

    return 0;
}

static int find_idx_by_id(AVBSFContext *ctx, unsigned id)
{
    IAMFSplitContext *const c = ctx->priv_data;

    for (int i = 0; i < c->nb_ids; i++)
        if (c->ids[i] == id)
            return i;

    av_log(ctx, AV_LOG_ERROR, "Invalid id %d\n", id);
    return AVERROR_INVALIDDATA;
}

static int audio_frame_obu(AVBSFContext *ctx, enum IAMF_OBU_Type type, int *idx,
                           uint8_t *buf, int *start_pos, unsigned *size,
                           int id_in_bitstream)
{
    GetBitContext gb;
    unsigned audio_substream_id;
    int ret;

    ret = init_get_bits8(&gb, buf + *start_pos, *size);
    if (ret < 0)
        return ret;

    if (id_in_bitstream) {
        int pos;
        audio_substream_id = get_leb(&gb);
        pos = get_bits_count(&gb) / 8;
        *start_pos += pos;
        *size -= pos;
    } else
        audio_substream_id = type - IAMF_OBU_IA_AUDIO_FRAME_ID0;

    ret = find_idx_by_id(ctx, audio_substream_id);
    if (ret < 0)
        return ret;

    *idx = ret;

    return 0;
}

static const ParamDefinition *get_param_definition(AVBSFContext *ctx,
                                                   unsigned int parameter_id)
{
    const IAMFSplitContext *const c = ctx->priv_data;
    const ParamDefinition *param_definition = NULL;

    for (int i = 0; i < c->nb_param_definitions; i++)
        if (c->param_definitions[i].param->parameter_id == parameter_id) {
            param_definition = &c->param_definitions[i];
            break;
        }

    return param_definition;
}

static int parameter_block_obu(AVBSFContext *ctx, uint8_t *buf, unsigned size)
{
    IAMFSplitContext *const c = ctx->priv_data;
    GetBitContext gb;
    const ParamDefinition *param_definition;
    const AVIAMFParamDefinition *param;
    AVIAMFParamDefinition *out_param = NULL;
    unsigned int duration, constant_subblock_duration;
    unsigned int nb_subblocks;
    unsigned int parameter_id;
    size_t out_param_size;
    int ret;

    ret = init_get_bits8(&gb, buf, size);
    if (ret < 0)
        return ret;

    parameter_id = get_leb(&gb);

    param_definition = get_param_definition(ctx, parameter_id);
    if (!param_definition) {
        ret = 0;
        goto fail;
    }

    param = param_definition->param;
    if (!param_definition->mode) {
        duration = get_leb(&gb);
        constant_subblock_duration = get_leb(&gb);
        if (constant_subblock_duration == 0)
            nb_subblocks = get_leb(&gb);
        else
            nb_subblocks = duration / constant_subblock_duration;
    } else {
        duration = param->duration;
        constant_subblock_duration = param->constant_subblock_duration;
        nb_subblocks = param->nb_subblocks;
        if (!nb_subblocks)
            nb_subblocks = duration / constant_subblock_duration;
    }

    out_param = av_iamf_param_definition_alloc(param->type, nb_subblocks,
                                               &out_param_size);
    if (!out_param) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    out_param->parameter_id = param->parameter_id;
    out_param->type = param->type;
    out_param->parameter_rate = param->parameter_rate;
    out_param->duration = duration;
    out_param->constant_subblock_duration = constant_subblock_duration;
    out_param->nb_subblocks = nb_subblocks;

    for (int i = 0; i < nb_subblocks; i++) {
        void *subblock = av_iamf_param_definition_get_subblock(out_param, i);
        unsigned int subblock_duration = constant_subblock_duration;

        if (!param_definition->mode && !constant_subblock_duration)
            subblock_duration = get_leb(&gb);

        switch (param->type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
            AVIAMFMixGain *mix = subblock;

            mix->animation_type = get_leb(&gb);
            if (mix->animation_type > AV_IAMF_ANIMATION_TYPE_BEZIER) {
                ret = 0;
                av_free(out_param);
                goto fail;
            }

            mix->start_point_value =
                av_make_q(sign_extend(get_bits(&gb, 16), 16), 1 << 8);
            if (mix->animation_type >= AV_IAMF_ANIMATION_TYPE_LINEAR)
                mix->end_point_value =
                    av_make_q(sign_extend(get_bits(&gb, 16), 16), 1 << 8);
            if (mix->animation_type == AV_IAMF_ANIMATION_TYPE_BEZIER) {
                mix->control_point_value =
                    av_make_q(sign_extend(get_bits(&gb, 16), 16), 1 << 8);
                mix->control_point_relative_time =
                    av_make_q(get_bits(&gb, 8), 1 << 8);
            }
            mix->subblock_duration = subblock_duration;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
            AVIAMFDemixingInfo *demix = subblock;

            demix->dmixp_mode = get_bits(&gb, 3);
            skip_bits(&gb, 5); // reserved
            demix->subblock_duration = subblock_duration;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
            AVIAMFReconGain *recon = subblock;

            for (int i = 0; i < 6; i++) {
                if (param_definition->recon_gain_present_bitmask & (1 << i)) {
                    unsigned int recon_gain_flags = get_leb(&gb);
                    unsigned int bitcount = 7 + 5 * !!(recon_gain_flags & 0x80);
                    recon_gain_flags =
                        (recon_gain_flags & 0x7F) | ((recon_gain_flags & 0xFF00) >> 1);
                    for (int j = 0; j < bitcount; j++) {
                        if (recon_gain_flags & (1 << j))
                            recon->recon_gain[i][j] = get_bits(&gb, 8);
                    }
                }
            }
            recon->subblock_duration = subblock_duration;
            break;
        }
        default:
            av_assert0(0);
        }
    }

    switch (param->type) {
    case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
        av_free(c->mix);
        c->mix = out_param;
        c->mix_size = out_param_size;
        break;
    case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
        av_free(c->demix);
        c->demix = out_param;
        c->demix_size = out_param_size;
        break;
    case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
        av_free(c->recon);
        c->recon = out_param;
        c->recon_size = out_param_size;
        break;
    default:
        av_assert0(0);
    }

    ret = 0;
fail:
    if (ret < 0)
        av_free(out_param);

    return ret;
}

static int iamf_parse_obu_header(const uint8_t *buf, int buf_size,
                                 unsigned *obu_size, int *start_pos,
                                 enum IAMF_OBU_Type *type,
                                 unsigned *skip, unsigned *discard)
{
    GetBitContext gb;
    int ret, extension_flag, trimming, start;
    unsigned size;

    ret = init_get_bits8(&gb, buf, FFMIN(buf_size, MAX_IAMF_OBU_HEADER_SIZE));
    if (ret < 0)
        return ret;

    *type          = get_bits(&gb, 5);
    /*redundant      =*/ get_bits1(&gb);
    trimming       = get_bits1(&gb);
    extension_flag = get_bits1(&gb);

    *obu_size = get_leb(&gb);
    if (*obu_size > INT_MAX)
        return AVERROR_INVALIDDATA;

    start = get_bits_count(&gb) / 8;

    if (trimming) {
        *skip = get_leb(&gb); // num_samples_to_trim_at_end
        *discard = get_leb(&gb); // num_samples_to_trim_at_start
    }

    if (extension_flag) {
        unsigned extension_bytes = get_leb(&gb);
        if (extension_bytes > INT_MAX / 8)
            return AVERROR_INVALIDDATA;
        skip_bits_long(&gb, extension_bytes * 8);
    }

    if (get_bits_left(&gb) < 0)
        return AVERROR_INVALIDDATA;

    size = *obu_size + start;
    if (size > INT_MAX)
        return AVERROR_INVALIDDATA;

    *obu_size -= get_bits_count(&gb) / 8 - start;
    *start_pos = size - *obu_size;

    return size;
}

static int iamf_frame_split_filter(AVBSFContext *ctx, AVPacket *out)
{
    IAMFSplitContext *const c = ctx->priv_data;
    int ret = 0;

    while (1) {
        enum IAMF_OBU_Type type;
        unsigned skip_samples = 0, discard_padding = 0, obu_size;
        int len, start_pos, idx;


        if (!c->buffer_pkt->size) {
            av_packet_unref(c->buffer_pkt);
            ret = ff_bsf_get_packet_ref(ctx, c->buffer_pkt);
            if (ret < 0)
                return ret;
        }

        len = iamf_parse_obu_header(c->buffer_pkt->data,
                                    c->buffer_pkt->size,
                                    &obu_size, &start_pos, &type,
                                    &skip_samples, &discard_padding);
        if (len < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to read obu\n");
            ret = len;
            goto fail;
        }

        if (type >= IAMF_OBU_IA_AUDIO_FRAME &&
            type <= IAMF_OBU_IA_AUDIO_FRAME_ID17) {
            ret = audio_frame_obu(ctx, type, &idx,
                                  c->buffer_pkt->data, &start_pos,
                                  &obu_size,
                                  type == IAMF_OBU_IA_AUDIO_FRAME);
            if (ret < 0)
                goto fail;
        } else {
            switch (type) {
            case IAMF_OBU_IA_PARAMETER_BLOCK:
                ret = parameter_block_obu(ctx, c->buffer_pkt->data + start_pos, obu_size);
                if (ret < 0)
                    goto fail;
                break;
            case IAMF_OBU_IA_SEQUENCE_HEADER:
                for (int i = 0; c->param_definitions && i < c->nb_param_definitions; i++)
                    av_free(c->param_definitions[i].param);
                av_freep(&c->param_definitions);
                av_freep(&c->ids);
                c->nb_param_definitions = 0;
                c->nb_ids = 0;
                // fall-through
            case IAMF_OBU_IA_TEMPORAL_DELIMITER:
                av_freep(&c->mix);
                av_freep(&c->demix);
                av_freep(&c->recon);
                c->mix_size = 0;
                c->demix_size = 0;
                c->recon_size = 0;
                break;
            case IAMF_OBU_IA_AUDIO_ELEMENT:
                if (!c->inband_descriptors)
                    break;
                ret = audio_element_obu(ctx, c->buffer_pkt->data + start_pos, obu_size);
                if (ret < 0) {
                    av_log(ctx, AV_LOG_ERROR, "Failed to read Audio Element OBU\n");
                    return ret;
                }
                break;
            case IAMF_OBU_IA_MIX_PRESENTATION:
                if (!c->inband_descriptors)
                    break;
                ret = mix_presentation_obu(ctx, c->buffer_pkt->data + start_pos, obu_size);
                if (ret < 0) {
                    av_log(ctx, AV_LOG_ERROR, "Failed to read Mix Presentation OBU\n");
                    return ret;
                }
                break;
            }

            c->buffer_pkt->data += len;
            c->buffer_pkt->size -= len;

            if (c->buffer_pkt->size < 0) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            continue;
        }

        ret = av_packet_ref(out, c->buffer_pkt);
        if (ret < 0)
            goto fail;

        if (skip_samples || discard_padding) {
            uint8_t *side_data = av_packet_new_side_data(out,
                                                         AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (!side_data)
                return AVERROR(ENOMEM);
            AV_WL32(side_data, skip_samples);
            AV_WL32(side_data + 4, discard_padding);
        }
        if (c->mix) {
            uint8_t *side_data = av_packet_new_side_data(out,
                                                         AV_PKT_DATA_IAMF_MIX_GAIN_PARAM,
                                                         c->mix_size);
            if (!side_data)
                return AVERROR(ENOMEM);
            memcpy(side_data, c->mix, c->mix_size);
        }
        if (c->demix) {
            uint8_t *side_data = av_packet_new_side_data(out,
                                                         AV_PKT_DATA_IAMF_DEMIXING_INFO_PARAM, 
                                                         c->demix_size);
            if (!side_data)
                return AVERROR(ENOMEM);
            memcpy(side_data, c->demix, c->demix_size);
        }
        if (c->recon) {
            uint8_t *side_data = av_packet_new_side_data(out,
                                                         AV_PKT_DATA_IAMF_RECON_GAIN_INFO_PARAM,
                                                         c->recon_size);
            if (!side_data)
                return AVERROR(ENOMEM);
            memcpy(side_data, c->recon, c->recon_size);
        }

        out->data += start_pos;
        out->size = obu_size;
        out->stream_index = idx + c->first_index;

        c->buffer_pkt->data += len;
        c->buffer_pkt->size -= len;

        if (!c->buffer_pkt->size)
            av_packet_unref(c->buffer_pkt);
        else if (c->buffer_pkt->size < 0) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        return 0;
    }

fail:
    if (ret < 0) {
        av_packet_unref(out);
        av_packet_unref(c->buffer_pkt);
    }

    return ret;
}

static int iamf_frame_split_init(AVBSFContext *ctx)
{
    IAMFSplitContext *const c = ctx->priv_data;
    uint8_t *extradata;
    int extradata_size;

    c->buffer_pkt = av_packet_alloc();
    if (!c->buffer_pkt)
        return AVERROR(ENOMEM);

    if (!c->inband_descriptors && !ctx->par_in->extradata_size) {
        av_log(ctx, AV_LOG_ERROR, "Missing extradata\n");
        return AVERROR_INVALIDDATA;
    }

    extradata = ctx->par_in->extradata;
    extradata_size = ctx->par_in->extradata_size;

    while (extradata_size) {
        enum IAMF_OBU_Type type;
        unsigned skip_samples = 0, discard_padding = 0, obu_size;
        int ret, len, start_pos;

        len = iamf_parse_obu_header(extradata,
                                    extradata_size,
                                    &obu_size, &start_pos, &type,
                                    &skip_samples, &discard_padding);
        if (len < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to read descriptors\n");
            return len;
        }

        switch (type) {
        case IAMF_OBU_IA_SEQUENCE_HEADER:
            for (int i = 0; c->param_definitions && i < c->nb_param_definitions; i++)
                av_free(c->param_definitions[i].param);
            av_freep(&c->param_definitions);
            av_freep(&c->ids);
            c->nb_param_definitions = 0;
            c->nb_ids = 0;
            break;
        case IAMF_OBU_IA_AUDIO_ELEMENT:
            ret = audio_element_obu(ctx, extradata + start_pos, obu_size);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Failed to read Audio Element OBU\n");
                return ret;
            }
            break;
        case IAMF_OBU_IA_MIX_PRESENTATION:
            ret = mix_presentation_obu(ctx, extradata + start_pos, obu_size);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Failed to read Mix Presentation OBU\n");
                return ret;
            }
            break;
        }

        extradata += len;
        extradata_size -= len;

        if (extradata_size < 0)
            return AVERROR_INVALIDDATA;
    }

    av_freep(&ctx->par_out->extradata);
    ctx->par_out->extradata_size = 0;

    return 0;
}

static void iamf_frame_split_flush(AVBSFContext *ctx)
{
    IAMFSplitContext *const c = ctx->priv_data;

    if (c->buffer_pkt)
        av_packet_unref(c->buffer_pkt);

    av_freep(&c->mix);
    av_freep(&c->demix);
    av_freep(&c->recon);
    c->mix_size = 0;
    c->demix_size = 0;
    c->recon_size = 0;
}

static void iamf_frame_split_close(AVBSFContext *ctx)
{
    IAMFSplitContext *const c = ctx->priv_data;

    iamf_frame_split_flush(ctx);
    av_packet_free(&c->buffer_pkt);

    for (int i = 0; c->param_definitions && i < c->nb_param_definitions; i++)
        av_free(c->param_definitions[i].param);
    av_freep(&c->param_definitions);
    c->nb_param_definitions = 0;

    av_freep(&c->ids);
    c->nb_ids = 0;
}

#define OFFSET(x) offsetof(IAMFSplitContext, x)
#define FLAGS (AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_BSF_PARAM)
static const AVOption iamf_frame_split_options[] = {
    { "first_index", "First index to set stream index in output packets",
        OFFSET(first_index), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGS },
    { "inband_descriptors", "Parse inband descriptor OBUs",
        OFFSET(inband_descriptors), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL }
};

static const AVClass iamf_frame_split_class = {
    .class_name = "iamf_frame_split_bsf",
    .item_name  = av_default_item_name,
    .option     = iamf_frame_split_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFBitStreamFilter ff_iamf_frame_split_bsf = {
    .p.name         = "iamf_frame_split",
    .p.priv_class   = &iamf_frame_split_class,
    .priv_data_size = sizeof(IAMFSplitContext),
    .init           = iamf_frame_split_init,
    .flush          = iamf_frame_split_flush,
    .close          = iamf_frame_split_close,
    .filter         = iamf_frame_split_filter,
};
