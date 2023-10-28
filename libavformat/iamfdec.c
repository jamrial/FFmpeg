/*
 * Immersive Audio Model and Formats demuxer
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

#include "config_components.h"

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/flac.h"
#include "libavcodec/mpeg4audio.h"
#include "libavcodec/put_bits.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "iamf.h"
#include "iamf_internal.h"
#include "internal.h"
#include "isom.h"

typedef struct IAMFCodecConfig {
    unsigned codec_config_id;
    enum AVCodecID codec_id;
    unsigned nb_samples;
    int seek_preroll;
    uint8_t *extradata;
    int extradata_size;
    int sample_rate;
} IAMFCodecConfig;

typedef struct IAMFAudioElement {
    AVStreamGroup *stream_group;

    AVStream **audio_substreams;
    int num_substreams;
} IAMFAudioElement;

typedef struct IAMFMixPresentation {
    AVStreamGroup *stream_group;
    unsigned int count_label;
    char **language_label;
} IAMFMixPresentation;

typedef struct IAMFParamDefinition {
    const AVIAMFAudioElement *audio_element;
    AVIAMFParamDefinition *param;
    size_t param_size;
} IAMFParamDefinition;

typedef struct IAMFDemuxContext {
    IAMFCodecConfig *codec_configs;
    int nb_codec_configs;
    IAMFAudioElement *audio_elements;
    int nb_audio_elements;
    IAMFMixPresentation *mix_presentations;
    int nb_mix_presentations;
    IAMFParamDefinition *param_definitions;
    int nb_param_definitions;

    // Packet side data
    AVIAMFParamDefinition *mix;
    size_t mix_size;
    AVIAMFParamDefinition *demix;
    size_t demix_size;
    AVIAMFParamDefinition *recon;
    size_t recon_size;
} IAMFDemuxContext;

static inline unsigned get_leb128(GetBitContext *gb) {
    int more, i = 0;
    unsigned len = 0;

    do {
        unsigned bits;
        int byte = get_bits(gb, 8);
        more = byte & 0x80;
        bits = byte & 0x7f;
        if (i <= 3 || (i == 4 && bits < (1 << 4)))
            len |= bits << (i * 7);
        else if (bits)
            return AVERROR_INVALIDDATA;
        if (++i == 8 && more)
            return AVERROR_INVALIDDATA;
    } while (more);

    return len;
}

static int parse_obu_header(const uint8_t *buf, int buf_size,
                            unsigned *obu_size, int *start_pos, enum IAMF_OBU_Type *type,
                            unsigned *skip_samples, unsigned *discard_padding)
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

    *obu_size = get_leb128(&gb);
    if (*obu_size > INT_MAX)
        return AVERROR_INVALIDDATA;

    start = get_bits_count(&gb) / 8;

    if (skip_samples)
        *skip_samples = trimming ? get_leb128(&gb) : 0; // num_samples_to_trim_at_end
    if (discard_padding)
        *discard_padding = trimming ? get_leb128(&gb) : 0; // num_samples_to_trim_at_start

    if (extension_flag) {
        unsigned extension_bytes = get_leb128(&gb);
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

//return < 0 if we need more data
static int get_score(const uint8_t *buf, int buf_size, enum IAMF_OBU_Type type, int *seq)
{
    if (type == IAMF_OBU_IA_SEQUENCE_HEADER) {
        if (buf_size < 4 || AV_RB32(buf) != MKBETAG('i','a','m','f'))
            return 0;
        *seq = 1;
        return -1;
    }
    if (type >= IAMF_OBU_IA_CODEC_CONFIG && type <= IAMF_OBU_IA_TEMPORAL_DELIMITER)
        return *seq ? -1 : 0;
    if (type >= IAMF_OBU_IA_AUDIO_FRAME && type <= IAMF_OBU_IA_AUDIO_FRAME_ID17)
        return *seq ? AVPROBE_SCORE_EXTENSION + 1 : 0;
    return 0;
}

static int iamf_probe(const AVProbeData *p)
{
    unsigned obu_size;
    enum IAMF_OBU_Type type;
    int seq = 0, cnt = 0, start_pos;
    int ret;

    while (1) {
        int size = parse_obu_header(p->buf + cnt, p->buf_size - cnt,
                                    &obu_size, &start_pos, &type,
                                    NULL, NULL);
        if (size < 0)
            return 0;

        ret = get_score(p->buf + cnt + start_pos,
                        p->buf_size - cnt - start_pos,
                        type, &seq);
        if (ret >= 0)
            return ret;

        cnt += FFMIN(size, p->buf_size - cnt);
    }
    return 0;
}

static inline int leb(AVIOContext *pb, unsigned *len) {
    int more, i = 0;
    *len = 0;

    do {
        unsigned bits;
        int byte = avio_r8(pb);
        if (pb->error)
            return pb->error;
        if (pb->eof_reached)
            return  AVERROR_INVALIDDATA;
        more = byte & 0x80;
        bits = byte & 0x7f;
        if (i <= 3 || (i == 4 && bits < (1 << 4)))
            *len |= bits << (i * 7);
        else if (bits)
            return AVERROR_INVALIDDATA;
        if (++i == 8 && more)
            return AVERROR_INVALIDDATA;
    } while (more);

    return i;
}

static int opus_decoder_config(AVFormatContext *s, AVIOContext *pb, int len,
                               IAMFCodecConfig *codec_config)
{
    int left = len - avio_tell(pb);

    if (left < 11)
        return AVERROR_INVALIDDATA;

    codec_config->extradata = av_malloc(left + 8);
    if (!codec_config->extradata)
        return AVERROR(ENOMEM);

    AV_WB32(codec_config->extradata, MKBETAG('O','p','u','s'));
    AV_WB32(codec_config->extradata + 4, MKBETAG('H','e','a','d'));
    codec_config->extradata_size = avio_read(pb, codec_config->extradata + 8, left);
    if (codec_config->extradata_size < left)
        return AVERROR_INVALIDDATA;

    codec_config->extradata_size += 8;
    codec_config->sample_rate = 48000;

    return 0;
}

static int aac_decoder_config(AVFormatContext *s, AVIOContext *pb, int len,
                              IAMFCodecConfig *codec_config)
{
    MPEG4AudioConfig cfg = { 0 };
    int object_type_id, codec_id, stream_type;
    int ret, tag, left;

    tag = avio_r8(pb);
    if (tag != MP4DecConfigDescrTag)
        return AVERROR_INVALIDDATA;

    object_type_id = avio_r8(pb);
    if (object_type_id != 0x40)
        return AVERROR_INVALIDDATA;

    stream_type = avio_r8(pb);
    if (((stream_type >> 2) != 5) || ((stream_type >> 1) & 1))
        return AVERROR_INVALIDDATA;

    avio_skip(pb, 3); // buffer size db
    avio_skip(pb, 4); // rc_max_rate
    avio_skip(pb, 4); // avg bitrate

    codec_id = ff_codec_get_id(ff_mp4_obj_type, object_type_id);
    if (codec_id && codec_id != codec_config->codec_id)
        return AVERROR_INVALIDDATA;

    tag = avio_r8(pb);
    if (tag != MP4DecSpecificDescrTag)
        return AVERROR_INVALIDDATA;

    left = len - avio_tell(pb);
    if (left <= 0)
        return AVERROR_INVALIDDATA;

    codec_config->extradata = av_malloc(left);
    if (!codec_config->extradata)
        return AVERROR(ENOMEM);

    codec_config->extradata_size = avio_read(pb, codec_config->extradata, left);
    if (codec_config->extradata_size < left)
        return AVERROR_INVALIDDATA;

    ret = avpriv_mpeg4audio_get_config2(&cfg, codec_config->extradata,
                                        codec_config->extradata_size, 1, s);
    if (ret < 0)
        return ret;

    codec_config->sample_rate = cfg.sample_rate;

    return 0;
}

static int flac_decoder_config(AVFormatContext *s, AVIOContext *pb, int len,
                               IAMFCodecConfig *codec_config)
{
    int left;

    avio_skip(pb, 4); // METADATA_BLOCK_HEADER

    left = len - avio_tell(pb);
    if (left < FLAC_STREAMINFO_SIZE)
        return AVERROR_INVALIDDATA;

    codec_config->extradata = av_malloc(left);
    if (!codec_config->extradata)
        return AVERROR(ENOMEM);

    codec_config->extradata_size = avio_read(pb, codec_config->extradata, left);
    if (codec_config->extradata_size < left)
        return AVERROR_INVALIDDATA;

    codec_config->sample_rate = AV_RB24(codec_config->extradata + 10) >> 4;

    return 0;
}

static int ipcm_decoder_config(AVFormatContext *s, AVIOContext *pb, int len,
                               IAMFCodecConfig *codec_config)
{
    static const enum AVSampleFormat sample_fmt[2][3] = {
        { AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S24BE, AV_CODEC_ID_PCM_S32BE },
        { AV_CODEC_ID_PCM_S16LE, AV_CODEC_ID_PCM_S24LE, AV_CODEC_ID_PCM_S32LE },
    };
    int sample_format = avio_r8(pb); // 0 = BE, 1 = LE
    int sample_size = (avio_r8(pb) / 8 - 2); // 16, 24, 32
    if (sample_format > 1 || sample_size > 2)
        return AVERROR_INVALIDDATA;

    codec_config->codec_id = sample_fmt[sample_format][sample_size];
    codec_config->sample_rate = avio_rb32(pb);

    if (len - avio_tell(pb))
        return AVERROR_INVALIDDATA;

    return 0;
}

static int codec_config_obu(AVFormatContext *s, int len)
{
    IAMFDemuxContext *const c = s->priv_data;
    IAMFCodecConfig *codec_config = NULL;
    FFIOContext b;
    AVIOContext *pb;
    uint8_t *buf;
    enum AVCodecID avcodec_id;
    unsigned codec_config_id, nb_samples, codec_id;
    int16_t seek_preroll;
    int ret;

    buf = av_malloc(len);
    if (!buf)
        return AVERROR(ENOMEM);

    ret = avio_read(s->pb, buf, len);
    if (ret != len) {
        if (ret >= 0)
            ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    ffio_init_context(&b, buf, len, 0, NULL, NULL, NULL, NULL);
    pb = &b.pub;

    ret = leb(pb, &codec_config_id);
    if (ret < 0)
        goto fail;

    codec_id = avio_rb32(pb);
    ret = leb(pb, &nb_samples);
    if (ret < 0)
        goto fail;

    seek_preroll = avio_rb16(pb);

    switch(codec_id) {
    case MKBETAG('O','p','u','s'):
        avcodec_id = AV_CODEC_ID_OPUS;
        break;
    case MKBETAG('m','p','4','a'):
        avcodec_id = AV_CODEC_ID_AAC;
        break;
    case MKBETAG('f','L','a','C'):
        avcodec_id = AV_CODEC_ID_FLAC;
        break;
    default:
        avcodec_id = AV_CODEC_ID_NONE;
        break;
    }

    for (int i = 0; i < c->nb_codec_configs; i++)
        if (c->codec_configs[i].codec_config_id == codec_config_id) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

    codec_config = av_dynarray2_add_nofree((void **)&c->codec_configs, &c->nb_codec_configs,
                                            sizeof(*c->codec_configs), NULL);
    if (!codec_config) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(codec_config, 0, sizeof(*codec_config));

    codec_config->codec_config_id = codec_config_id;
    codec_config->codec_id = avcodec_id;
    codec_config->nb_samples = nb_samples;
    codec_config->seek_preroll = seek_preroll;

    switch(codec_id) {
    case MKBETAG('O','p','u','s'):
        ret = opus_decoder_config(s, pb, len, codec_config);
        break;
    case MKBETAG('m','p','4','a'):
        ret = aac_decoder_config(s, pb, len, codec_config);
        break;
    case MKBETAG('f','L','a','C'):
        ret = flac_decoder_config(s, pb, len, codec_config);
        break;
    case MKBETAG('i','p','c','m'):
        ret = ipcm_decoder_config(s, pb, len, codec_config);
        break;
    default:
        break;
    }
    if (ret < 0)
        goto fail;

    len -= avio_tell(pb);
    if (len) {
       int level = (s->error_recognition & AV_EF_EXPLODE) ? AV_LOG_ERROR : AV_LOG_WARNING;
       av_log(s, level, "Underread in codec_config_obu. %d bytes left at the end\n", len);
    }

    ret = 0;
fail:
    av_free(buf);
    return ret;
}

static int update_extradata(AVFormatContext *s, AVStream *st)
{
    GetBitContext gb;
    PutBitContext pb;
    int ret;

    switch(st->codecpar->codec_id) {
    case AV_CODEC_ID_OPUS:
        AV_WB8(st->codecpar->extradata + 9, st->codecpar->ch_layout.nb_channels);
        break;
    case AV_CODEC_ID_AAC: {
        uint8_t buf[5];

        init_put_bits(&pb, buf, sizeof(buf));
        ret = init_get_bits8(&gb, st->codecpar->extradata, st->codecpar->extradata_size);
        if (ret < 0)
            return ret;

        ret = get_bits(&gb, 5);
        put_bits(&pb, 5, ret);
        if (ret == AOT_ESCAPE) // violates section 3.11.2, but better check for it
            put_bits(&pb, 6, get_bits(&gb, 6));
        ret = get_bits(&gb, 4);
        put_bits(&pb, 4, ret);
        if (ret == 0x0f)
            put_bits(&pb, 24, get_bits(&gb, 24));

        skip_bits(&gb, 4);
        put_bits(&pb, 4, st->codecpar->ch_layout.nb_channels); // set channel config
        ret = put_bits_left(&pb);
        put_bits(&pb, ret, get_bits(&gb, ret));
        flush_put_bits(&pb);

        memcpy(st->codecpar->extradata, buf, sizeof(buf));
        break;
    }
    case AV_CODEC_ID_FLAC: {
        uint8_t buf[13];

        init_put_bits(&pb, buf, sizeof(buf));
        ret = init_get_bits8(&gb, st->codecpar->extradata, st->codecpar->extradata_size);
        if (ret < 0)
            return ret;

        put_bits32(&pb, get_bits_long(&gb, 32)); // min/max blocksize
        put_bits64(&pb, 48, get_bits64(&gb, 48)); // min/max framesize
        put_bits(&pb, 20, get_bits(&gb, 20)); // samplerate
        skip_bits(&gb, 3);
        put_bits(&pb, 3, st->codecpar->ch_layout.nb_channels - 1);
        ret = put_bits_left(&pb);
        put_bits(&pb, ret, get_bits(&gb, ret));
        flush_put_bits(&pb);

        memcpy(st->codecpar->extradata, buf, sizeof(buf));
        break;
    }
    }

    return 0;
}

static int scalable_channel_layout_config(AVFormatContext *s, AVIOContext *pb,
                                          IAMFAudioElement *audio_element,
                                          const IAMFCodecConfig *codec_config)
{
    AVStreamGroup *stg = audio_element->stream_group;
    int num_layers, k = 0;

    num_layers = avio_r8(pb) >> 5; // get_bits(&gb, 3);
    // skip_bits(&gb, 5); //reserved

    if (num_layers > 6)
        return AVERROR_INVALIDDATA;

    for (int i = 0; i < num_layers; i++) {
        AVIAMFLayer *layer;
        int loudspeaker_layout, output_gain_is_present_flag;
        int coupled_substream_count;
        int ret, byte = avio_r8(pb);

        ret = avformat_iamf_audio_element_add_layer(stg->params.iamf_audio_element, NULL);
        if (ret < 0)
            return ret;

        loudspeaker_layout = byte >> 4; // get_bits(&gb, 4);
        output_gain_is_present_flag = (byte >> 3) & 1; //get_bits1(&gb);
        layer = stg->params.iamf_audio_element->layers[i];
        layer->recon_gain_is_present = (byte >> 2) & 1;
        layer->substream_count = avio_r8(pb);
        coupled_substream_count = avio_r8(pb);

        if (output_gain_is_present_flag) {
            layer->output_gain_flags = avio_r8(pb) >> 2;  // get_bits(&gb, 6);
            layer->output_gain = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
        }

        if (loudspeaker_layout < 10)
            av_channel_layout_copy(&layer->ch_layout, &ff_iamf_scalable_ch_layouts[loudspeaker_layout]);
        else
            layer->ch_layout = (AVChannelLayout){ .order = AV_CHANNEL_ORDER_UNSPEC,
                                                          .nb_channels = layer->substream_count +
                                                                         coupled_substream_count };

        for (int j = 0; j < layer->substream_count; j++) {
            AVStream *st = audio_element->audio_substreams[k++];

            ret = avformat_stream_group_add_stream(stg, st);
            if (ret < 0)
                return ret;

            st->codecpar->ch_layout = coupled_substream_count-- > 0 ? (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO :
                                                                      (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;

            ret = update_extradata(s, st);
            if (ret < 0)
                return ret;

            avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
        }

    }

    return 0;
}

static int ambisonics_config(AVFormatContext *s, AVIOContext *pb,
                             IAMFAudioElement *audio_element,
                             const IAMFCodecConfig *codec_config)
{
    AVStreamGroup *stg = audio_element->stream_group;
    AVIAMFLayer *layer;
    unsigned ambisonics_mode;
    int output_channel_count, substream_count, order;
    int ret;

    ret = leb(pb, &ambisonics_mode);
    if (ret < 0)
        return ret;

    if (ambisonics_mode > 1)
        return 0;

    output_channel_count = avio_r8(pb);  // C
    substream_count = avio_r8(pb);  // N
    if (audio_element->num_substreams != substream_count)
        return AVERROR_INVALIDDATA;

    order = floor(sqrt(output_channel_count - 1));
    /* incomplete order - some harmonics are missing */
    if ((order + 1) * (order + 1) != output_channel_count)
        return AVERROR_INVALIDDATA;

    ret = avformat_iamf_audio_element_add_layer(stg->params.iamf_audio_element, NULL);
    if (ret < 0)
        return ret;

    layer = stg->params.iamf_audio_element->layers[0];
    layer->ambisonics_mode = ambisonics_mode;
    layer->substream_count = substream_count;
    if (ambisonics_mode == 0) {
        for (int i = 0; i < substream_count; i++) {
            AVStream *st = audio_element->audio_substreams[i];

            st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;

            ret = avformat_stream_group_add_stream(stg, st);
            if (ret < 0)
                return ret;

            ret = update_extradata(s, st);
            if (ret < 0)
                return ret;

            avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
        }

        layer->ch_layout.order = AV_CHANNEL_ORDER_CUSTOM;
        layer->ch_layout.nb_channels = output_channel_count;
        layer->ch_layout.u.map = av_calloc(output_channel_count, sizeof(*layer->ch_layout.u.map));
        if (!layer->ch_layout.u.map)
            return AVERROR(ENOMEM);

        for (int i = 0; i < output_channel_count; i++)
            layer->ch_layout.u.map[i].id = avio_r8(pb) + AV_CHAN_AMBISONIC_BASE;
    } else {
        int coupled_substream_count = avio_r8(pb);  // M
        int nb_demixing_matrix = substream_count + coupled_substream_count;
        int demixing_matrix_size = nb_demixing_matrix * output_channel_count;

        layer->ch_layout = (AVChannelLayout){ .order = AV_CHANNEL_ORDER_AMBISONIC, .nb_channels = output_channel_count };
        layer->demixing_matrix = av_malloc_array(demixing_matrix_size, sizeof(*layer->demixing_matrix));
        if (!layer->demixing_matrix)
            return AVERROR(ENOMEM);

        for (int i = 0; i < demixing_matrix_size; i++)
            layer->demixing_matrix[i] = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);

        for (int i = 0; i < substream_count; i++) {
            AVStream *st = audio_element->audio_substreams[i];

            st->codecpar->ch_layout = coupled_substream_count-- > 0 ? (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO :
                                                                      (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;

            ret = avformat_stream_group_add_stream(stg, st);
            if (ret < 0)
                return ret;

            ret = update_extradata(s, st);
            if (ret < 0)
                return ret;

            avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
        }
    }

    return 0;
}

static int param_parse(AVFormatContext *s, AVIOContext *pb,
                       unsigned int param_definition_type,
                       const IAMFAudioElement *audio_element,
                       AVIAMFParamDefinition **out_param_definition)
{
    IAMFDemuxContext *const c = s->priv_data;
    IAMFParamDefinition *param_definition;
    const IAMFParamDefinition *old_param = NULL;
    unsigned int parameter_id, parameter_rate, param_definition_mode;
    unsigned int duration, constant_subblock_duration, num_subblocks = 0;
    int nb_param_definitions = c->nb_param_definitions, ret;

    ret = leb(pb, &parameter_id);
    if (ret < 0)
        return ret;

    for (int i = 0; i < c->nb_param_definitions; i++)
        if (c->param_definitions[i].param->parameter_id == parameter_id) {
            old_param = param_definition = &c->param_definitions[i];
            break;
        }

    if (!old_param) {
        param_definition = av_dynarray2_add_nofree((void **)&c->param_definitions, &nb_param_definitions,
                                                   sizeof(*c->param_definitions), NULL);
        if (!param_definition)
            return AVERROR(ENOMEM);

        memset(param_definition, 0, sizeof(*param_definition));
    }

    ret = leb(pb, &parameter_rate);
    if (ret < 0)
        return ret;

    param_definition_mode = avio_r8(pb) >> 7;

    if (old_param && (param_definition_mode != old_param->param->param_definition_mode ||
                      param_definition_type != old_param->param->param_definition_type)) {
        av_log(s, AV_LOG_ERROR, "Inconsistent param_definition_mode or param_definition_type values "
                                "for parameter_id %d\n", parameter_id);
        return AVERROR_INVALIDDATA;
    }

    if (param_definition_mode == 0) {
        ret = leb(pb, &duration);
        if (ret < 0)
            return ret;

        ret = leb(pb, &constant_subblock_duration);
        if (ret < 0)
            return ret;

        if (constant_subblock_duration == 0) {
            ret = leb(pb, &num_subblocks);
            if (ret < 0)
                return ret;
        } else
            num_subblocks = duration / constant_subblock_duration;
    } else if (audio_element)
        duration = constant_subblock_duration = audio_element->stream_group->streams[0]->codecpar->frame_size;

    if (old_param) {
        if (num_subblocks != old_param->param->num_subblocks) {
            av_log(s, AV_LOG_ERROR, "Inconsistent num_subblocks values for parameter_id %d\n", parameter_id);
            return AVERROR_INVALIDDATA;
        }
    } else {
        param_definition->param = avformat_iamf_param_definition_alloc(param_definition_type, NULL, num_subblocks,
                                                                       NULL, &param_definition->param_size);
        if (!param_definition->param)
            return AVERROR(ENOMEM);
        if (audio_element)
            param_definition->audio_element = audio_element->stream_group->params.iamf_audio_element;
    }

    for (int i = 0; i < num_subblocks; i++) {
        void *subblock = avformat_iamf_param_definition_get_subblock(param_definition->param, i);
        unsigned int subblock_duration = constant_subblock_duration;

        if (constant_subblock_duration == 0) {
            ret = leb(pb, &subblock_duration);
            if (ret < 0) {
                if (!old_param)
                    av_freep(&param_definition->param);
                return ret;
            }
        }

        switch (param_definition_type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
            AVIAMFMixGainParameterData *mix = subblock;
            mix->subblock_duration = subblock_duration;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
            AVIAMFDemixingInfoParameterData *demix = subblock;
            demix->subblock_duration = subblock_duration;
            // DemixingInfoParameterData
            demix->dmixp_mode = avio_r8(pb) >> 5;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
            AVIAMFReconGainParameterData *recon = subblock;
            recon->subblock_duration = subblock_duration;
            break;
        }
        default:
            if (!old_param)
                av_freep(&param_definition->param);
            return AVERROR_INVALIDDATA;
        }
    }

    param_definition->param->parameter_id = parameter_id;
    param_definition->param->parameter_rate = parameter_rate;
    param_definition->param->param_definition_mode = param_definition_mode;
    param_definition->param->duration = duration;
    param_definition->param->constant_subblock_duration = constant_subblock_duration;
    param_definition->param->num_subblocks = num_subblocks;

    av_assert0(out_param_definition);
    *out_param_definition = old_param ? av_memdup(param_definition->param, param_definition->param_size) :
                                        param_definition->param;
    if (!*out_param_definition)
        return AVERROR(ENOMEM);
    if (!old_param)
        c->nb_param_definitions = nb_param_definitions;

    return 0;
}

static int audio_element_obu(AVFormatContext *s, int len)
{
    IAMFDemuxContext *const c = s->priv_data;
    const IAMFCodecConfig *codec_config = NULL;
    AVIAMFAudioElement *avaudio_element;
    IAMFAudioElement *audio_element;
    FFIOContext b;
    AVIOContext *pb;
    uint8_t *buf;
    unsigned audio_element_id, codec_config_id, num_substreams, num_parameters;
    int audio_element_type, ret;

    buf = av_malloc(len);
    if (!buf)
        return AVERROR(ENOMEM);

    ret = avio_read(s->pb, buf, len);
    if (ret != len) {
        if (ret >= 0)
            ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    ffio_init_context(&b, buf, len, 0, NULL, NULL, NULL, NULL);
    pb = &b.pub;

    ret = leb(pb, &audio_element_id);
    if (ret < 0)
        goto fail;

    for (int i = 0; i < c->nb_audio_elements; i++)
        if (c->audio_elements[i].stream_group->id == audio_element_id) {
            av_log(s, AV_LOG_ERROR, "Duplicate audio_element_id %d\n", audio_element_id);
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

    audio_element_type = avio_r8(pb) >> 5;

    ret = leb(pb, &codec_config_id);
    if (ret < 0)
        goto fail;

    for (int i = 0; i < c->nb_codec_configs; i++) {
        if (c->codec_configs[i].codec_config_id == codec_config_id) {
            codec_config = &c->codec_configs[i];
            break;
        }
    }

    if (!codec_config) {
        av_log(s, AV_LOG_ERROR, "Non existant codec config id %d referenced in an audio element\n", codec_config_id);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    if (codec_config->codec_id == AV_CODEC_ID_NONE) {
        av_log(s, AV_LOG_DEBUG, "Unknown codec id referenced in an audio element. Ignoring\n");
        ret = 0;
        goto fail;
    }

    ret = leb(pb, &num_substreams);
    if (ret < 0)
        goto fail;

    audio_element = av_dynarray2_add_nofree((void **)&c->audio_elements, &c->nb_audio_elements,
                                            sizeof(*c->audio_elements), NULL);
    if (!audio_element) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(audio_element, 0, sizeof(*audio_element));

    audio_element->audio_substreams = av_calloc(num_substreams, sizeof(*audio_element->audio_substreams));
    if (!audio_element->audio_substreams) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    audio_element->stream_group = avformat_stream_group_create(s, AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT, NULL);
    if (!audio_element->stream_group)
        return AVERROR(ENOMEM);
    audio_element->stream_group->id = audio_element_id;
    avaudio_element = audio_element->stream_group->params.iamf_audio_element;
    avaudio_element->codec_config_id = codec_config_id;
    avaudio_element->audio_element_type = audio_element_type;

    audio_element->num_substreams = num_substreams;

    for (int i = 0; i < num_substreams; i++) {
        AVStream *st = audio_element->audio_substreams[i] = avformat_new_stream(s, NULL);
        unsigned audio_substream_id;

        if (!st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = leb(pb, &audio_substream_id);
        if (ret < 0)
            goto fail;

        st->id = audio_substream_id;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id   = codec_config->codec_id;
        st->codecpar->frame_size = codec_config->nb_samples;
        st->codecpar->sample_rate = codec_config->sample_rate;
        st->codecpar->seek_preroll = codec_config->seek_preroll;
        ffstream(st)->need_parsing = AVSTREAM_PARSE_HEADERS;

        switch(st->codecpar->codec_id) {
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_FLAC:
        case AV_CODEC_ID_OPUS:
            st->codecpar->extradata = av_malloc(codec_config->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!st->codecpar->extradata) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            memcpy(st->codecpar->extradata, codec_config->extradata, codec_config->extradata_size);
            memset(st->codecpar->extradata + codec_config->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            st->codecpar->extradata_size = codec_config->extradata_size;
            break;
        }
    }

    ret = leb(pb, &num_parameters);
    if (ret < 0)
        goto fail;

    if (num_parameters && audio_element_type != 0) {
        av_log(s, AV_LOG_ERROR, "Audio Element parameter count %u is invalid for Scene representations\n", num_parameters);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    for (int i = 0; i < num_parameters; i++) {
        unsigned param_definition_type;

        ret = leb(pb, &param_definition_type);
        if (ret < 0)
            goto fail;

        if (param_definition_type == AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        } else if (param_definition_type == AV_IAMF_PARAMETER_DEFINITION_DEMIXING) {
            ret = param_parse(s, pb, param_definition_type, audio_element, &avaudio_element->demixing_info);
            if (ret < 0)
                goto fail;

            avaudio_element->default_w = avio_r8(pb) >> 4;
        } else if (param_definition_type == AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN) {
            ret = param_parse(s, pb, param_definition_type, audio_element, &avaudio_element->recon_gain_info);
            if (ret < 0)
                goto fail;
        } else {
            unsigned param_definition_size;
            ret = leb(pb, &param_definition_size);
            if (ret < 0)
                goto fail;

            avio_skip(pb, param_definition_size);
        }
    }

    if (audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL) {
        ret = scalable_channel_layout_config(s, pb, audio_element, codec_config);
        if (ret < 0)
            goto fail;
    } else if (audio_element_type == AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE) {
        ret = ambisonics_config(s, pb, audio_element, codec_config);
        if (ret < 0)
            goto fail;
    } else {
        unsigned audio_element_config_size;
        ret = leb(pb, &audio_element_config_size);
        if (ret < 0)
            goto fail;
    }

    len -= avio_tell(pb);
    if (len) {
       int level = (s->error_recognition & AV_EF_EXPLODE) ? AV_LOG_ERROR : AV_LOG_WARNING;
       av_log(s, level, "Underread in audio_element_obu. %d bytes left at the end\n", len);
    }

    ret = 0;
fail:
    av_free(buf);

    return ret;
}

static int label_string(AVFormatContext *s, AVIOContext *pb, char **label)
{
    uint8_t buf[128];

    avio_get_str(pb, sizeof(buf), buf, sizeof(buf));

    if (pb->error)
        return pb->error;
    if (pb->eof_reached)
        return AVERROR_INVALIDDATA;
    *label = av_strdup(buf);
    if (!*label)
        return AVERROR(ENOMEM);

    return 0;
}

static int mix_presentation_obu(AVFormatContext *s, int len)
{
    IAMFDemuxContext *const c = s->priv_data;
    AVIAMFMixPresentation *mix_presentation;
    IAMFMixPresentation *mixi;
    FFIOContext b;
    AVIOContext *pb;
    uint8_t *buf;
    unsigned mix_presentation_id;
    int ret;

    buf = av_malloc(len);
    if (!buf)
        return AVERROR(ENOMEM);

    ret = avio_read(s->pb, buf, len);
    if (ret != len) {
        if (ret >= 0)
            ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    ffio_init_context(&b, buf, len, 0, NULL, NULL, NULL, NULL);
    pb = &b.pub;

    ret = leb(pb, &mix_presentation_id);
    if (ret < 0)
        goto fail;

    for (int i = 0; i < c->nb_mix_presentations; i++)
        if (c->mix_presentations[i].stream_group->id == mix_presentation_id) {
            av_log(s, AV_LOG_ERROR, "Duplicate mix_presentation_id %d\n", mix_presentation_id);
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

    mixi = av_dynarray2_add_nofree((void **)&c->mix_presentations, &c->nb_mix_presentations,
                                   sizeof(*c->mix_presentations), NULL);
    if (!mixi) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(mixi, 0, sizeof(*mixi));

    mixi->stream_group = avformat_stream_group_create(s, AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION, NULL);
    if (!mixi->stream_group) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    mixi->stream_group->id = mix_presentation_id;

    mix_presentation = mixi->stream_group->params.iamf_mix_presentation;

    ret = leb(pb, &mixi->count_label);
    if (ret < 0)
        goto fail;

    mixi->language_label = av_calloc(mixi->count_label, sizeof(*mixi->language_label));
    if (!mixi->language_label) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (int i = 0; i < mixi->count_label; i++) {
        ret = label_string(s, pb, &mixi->language_label[i]);
        if (ret < 0)
            goto fail;
    }

    for (int i = 0; i < mixi->count_label; i++) {
        char *annotation = NULL;
        ret = label_string(s, pb, &annotation);
        if (ret < 0)
            goto fail;
        ret = av_dict_set(&mix_presentation->annotations, mixi->language_label[i], annotation,
                          AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);
        if (ret < 0)
            goto fail;
    }

    ret = leb(pb, &mix_presentation->num_submixes);
    if (ret < 0)
        goto fail;

    mix_presentation->submixes = av_calloc(mix_presentation->num_submixes, sizeof(*mix_presentation->submixes));
    if (!mix_presentation->submixes) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (int i = 0; i < mix_presentation->num_submixes; i++) {
        AVIAMFSubmix *sub_mix;

        sub_mix = mix_presentation->submixes[i] = av_mallocz(sizeof(*sub_mix));
        if (!sub_mix) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = leb(pb, &sub_mix->num_elements);
        if (ret < 0)
            goto fail;

        sub_mix->elements = av_calloc(sub_mix->num_elements, sizeof(*sub_mix->elements));
        if (!sub_mix->elements) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        for (int j = 0; j < sub_mix->num_elements; j++) {
            AVIAMFSubmixElement *submix_element;
            IAMFAudioElement *audio_element = NULL;
            unsigned int audio_element_id, rendering_config_extension_size;

            submix_element = sub_mix->elements[j] = av_mallocz(sizeof(*submix_element));
            if (!submix_element) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            ret = leb(pb, &audio_element_id);
            if (ret < 0)
                goto fail;

            for (int k = 0; k < c->nb_audio_elements; k++)
                if (c->audio_elements[k].stream_group->id == audio_element_id) {
                    audio_element = &c->audio_elements[k];
                    submix_element->audio_element = audio_element->stream_group;
                }

            if (!audio_element) {
                av_log(s, AV_LOG_ERROR, "Invalid Audio Element with id %u referenced by Mix Parameters %u\n", audio_element_id, mix_presentation_id);
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }

            for (int k = 0; k < audio_element->num_substreams; k++) {
                ret = avformat_stream_group_add_stream(mixi->stream_group, audio_element->audio_substreams[k]);
                if (ret < 0 && ret != AVERROR(EEXIST))
                    goto fail;
            }

            for (int k = 0; k < mixi->count_label; k++) {
                char *annotation = NULL;
                ret = label_string(s, pb, &annotation);
                if (ret < 0)
                    goto fail;
                ret = av_dict_set(&submix_element->annotations, mixi->language_label[k], annotation,
                                  AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);
                if (ret < 0)
                    goto fail;
            }

            submix_element->headphones_rendering_mode = avio_r8(pb) >> 6;

            ret = leb(pb, &rendering_config_extension_size);
            if (ret < 0)
                goto fail;
            avio_skip(pb, rendering_config_extension_size);

            ret = param_parse(s, pb, AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, audio_element,
                              &submix_element->element_mix_config);
            if (ret < 0)
                goto fail;
            submix_element->default_mix_gain = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
        }
        ret = param_parse(s, pb, AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN, NULL, &sub_mix->output_mix_config);
        if (ret < 0)
            goto fail;
        sub_mix->default_mix_gain = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);

        ret = leb(pb, &sub_mix->num_layouts);
        if (ret < 0)
            goto fail;

        sub_mix->layouts = av_calloc(sub_mix->num_layouts, sizeof(*sub_mix->layouts));
        if (!sub_mix->layouts) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        for (int j = 0; j < sub_mix->num_layouts; j++) {
            AVIAMFSubmixLayout *submix_layout;
            int info_type;
            int byte = avio_r8(pb);

            submix_layout = sub_mix->layouts[j] = av_mallocz(sizeof(*submix_layout));
            if (!submix_layout) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            submix_layout->layout_type = byte >> 6;
            if (submix_layout->layout_type < AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS &&
                submix_layout->layout_type > AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL) {
                av_log(s, AV_LOG_ERROR, "Invalid Layout type %u in a submix from Mix Presentation %u\n", submix_layout->layout_type, mix_presentation_id);
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            if (submix_layout->layout_type == 2) {
                int sound_system;
                sound_system = (byte >> 2) & 0xF;
                av_channel_layout_copy(&submix_layout->sound_system, &ff_iamf_sound_system_map[sound_system].layout);
            }

            info_type = avio_r8(pb);
            submix_layout->integrated_loudness = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
            submix_layout->digital_peak = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);

            if (info_type & 1)
                submix_layout->true_peak = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);

            if (info_type & 2) {
                unsigned int num_anchored_loudness = avio_r8(pb);

                for (int k = 0; k < num_anchored_loudness; k++) {
                    unsigned int anchor_element = avio_r8(pb);
                    AVRational anchored_loudness = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
                    if (anchor_element >= AV_IAMF_ANCHOR_ELEMENT_DIALOGUE &&
                        anchor_element <= AV_IAMF_ANCHOR_ELEMENT_ALBUM) {
                        submix_layout->anchored_loudness[anchor_element] = anchored_loudness;
                    }
                }
            }

            if (info_type & 0xFC) {
                unsigned int info_type_size;
                ret = leb(pb, &info_type_size);
                if (ret < 0)
                    goto fail;

                avio_skip(pb, info_type_size);
            }
        }
    }

    len -= avio_tell(pb);
    if (len) {
       int level = (s->error_recognition & AV_EF_EXPLODE) ? AV_LOG_ERROR : AV_LOG_WARNING;
       av_log(s, level, "Underread in mix_presentation_obu. %d bytes left at the end\n", len);
    }

    ret = 0;
fail:
    av_free(buf);

    return ret;
}

static int iamf_read_header(AVFormatContext *s)
{
    IAMFDemuxContext *const c = s->priv_data;
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    int ret;

    while (1) {
        unsigned obu_size;
        enum IAMF_OBU_Type type;
        int start_pos, len, size;

        if ((ret = ffio_ensure_seekback(s->pb, MAX_IAMF_OBU_HEADER_SIZE)) < 0)
            return ret;
        size = avio_read(s->pb, header, MAX_IAMF_OBU_HEADER_SIZE);
        if (size < 0)
            return size;

        len = parse_obu_header(header, size, &obu_size, &start_pos, &type, NULL, NULL);
        if (len < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to read obu\n");
            return len;
        }

        if (type >= IAMF_OBU_IA_PARAMETER_BLOCK && type < IAMF_OBU_IA_SEQUENCE_HEADER) {
            avio_seek(s->pb, -size, SEEK_CUR);
            break;
        }

        avio_seek(s->pb, -(size - start_pos), SEEK_CUR);
        switch (type) {
        case IAMF_OBU_IA_CODEC_CONFIG:
            ret = codec_config_obu(s, obu_size);
            break;
        case IAMF_OBU_IA_AUDIO_ELEMENT:
            ret = audio_element_obu(s, obu_size);
            break;
        case IAMF_OBU_IA_MIX_PRESENTATION:
            ret = mix_presentation_obu(s, obu_size);
            break;
        case IAMF_OBU_IA_TEMPORAL_DELIMITER:
            av_freep(&c->mix);
            c->mix_size = 0;
            av_freep(&c->demix);
            c->demix_size = 0;
            av_freep(&c->recon);
            c->recon_size = 0;
            break;
        default: {
            int64_t offset = avio_skip(s->pb, obu_size);
            if (offset < 0)
                ret = offset;
            break;
        }
        }
        if (ret < 0)
            return ret;
    }

    return 0;
}

static AVStream *find_stream_by_id(AVFormatContext *s, int id)
{
    for (int i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == id)
            return s->streams[i];

    av_log(s, AV_LOG_ERROR, "Invalid stream id %d\n", id);
    return NULL;
}

static int audio_frame_obu(AVFormatContext *s, AVPacket *pkt, int len,
                           enum IAMF_OBU_Type type,
                           unsigned skip_samples, unsigned discard_padding,
                           int id_in_bitstream)
{
    const IAMFDemuxContext *const c = s->priv_data;
    AVStream *st;
    int ret, audio_substream_id;

    if (id_in_bitstream) {
        unsigned explicit_audio_substream_id;
        ret = leb(s->pb, &explicit_audio_substream_id);
        if (ret < 0)
            return ret;
        len -= ret;
        audio_substream_id = explicit_audio_substream_id;
    } else
        audio_substream_id = type - IAMF_OBU_IA_AUDIO_FRAME_ID0;

    st = find_stream_by_id(s, audio_substream_id);
    if (!st)
        return AVERROR_INVALIDDATA;

    ret = av_get_packet(s->pb, pkt, len);
    if (ret < 0)
        return ret;
    if (ret != len)
        return AVERROR_INVALIDDATA;

    if (skip_samples || discard_padding) {
        uint8_t *side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
        if (!side_data)
            return AVERROR(ENOMEM);
        AV_WL32(side_data, skip_samples);
        AV_WL32(side_data + 4, discard_padding);
    }
    if (c->mix) {
        uint8_t *side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_IAMF_MIX_GAIN_PARAM, c->mix_size);
        if (!side_data)
            return AVERROR(ENOMEM);
        memcpy(side_data, c->mix, c->mix_size);
    }
    if (c->demix) {
        uint8_t *side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_IAMF_DEMIXING_INFO_PARAM, c->demix_size);
        if (!side_data)
            return AVERROR(ENOMEM);
        memcpy(side_data, c->demix, c->demix_size);
    }
    if (c->recon) {
        uint8_t *side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_IAMF_RECON_GAIN_INFO_PARAM, c->recon_size);
        if (!side_data)
            return AVERROR(ENOMEM);
        memcpy(side_data, c->recon, c->recon_size);
    }

    pkt->stream_index = st->index;
    return 0;
}

static const IAMFParamDefinition *get_param_definition(AVFormatContext *s, unsigned int parameter_id)
{
    const IAMFDemuxContext *const c = s->priv_data;
    const IAMFParamDefinition *param_definition = NULL;

    for (int i = 0; i < c->nb_param_definitions; i++)
        if (c->param_definitions[i].param->parameter_id == parameter_id) {
            param_definition = &c->param_definitions[i];
            break;
        }

    return param_definition;
}

static int parameter_block_obu(AVFormatContext *s, int len)
{
    IAMFDemuxContext *const c = s->priv_data;
    const IAMFParamDefinition *param_definition;
    const AVIAMFParamDefinition *param;
    AVIAMFParamDefinition *out_param = NULL;
    FFIOContext b;
    AVIOContext *pb;
    uint8_t *buf;
    unsigned int duration, constant_subblock_duration;
    unsigned int num_subblocks;
    unsigned int parameter_id;
    size_t out_param_size;
    int ret;

    buf = av_malloc(len);
    if (!buf)
        return AVERROR(ENOMEM);

    ret = avio_read(s->pb, buf, len);
    if (ret != len) {
        if (ret >= 0)
            ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    ffio_init_context(&b, buf, len, 0, NULL, NULL, NULL, NULL);
    pb = &b.pub;

    ret = leb(pb, &parameter_id);
    if (ret < 0)
        goto fail;

    param_definition = get_param_definition(s, parameter_id);
    if (!param_definition) {
        ret = 0;
        goto fail;
    }

    param = param_definition->param;
    if (param->param_definition_mode) {
        ret = leb(pb, &duration);
        if (ret < 0)
            goto fail;

        ret = leb(pb, &constant_subblock_duration);
        if (ret < 0)
            goto fail;

        if (constant_subblock_duration == 0) {
            ret = leb(pb, &num_subblocks);
            if (ret < 0)
                goto fail;
        } else
            num_subblocks = duration / constant_subblock_duration;
    } else {
        duration = param->duration;
        constant_subblock_duration = param->constant_subblock_duration;
        num_subblocks = param->num_subblocks;
        if (!num_subblocks)
            num_subblocks = duration / constant_subblock_duration;
    }

    out_param = avformat_iamf_param_definition_alloc(param->param_definition_type, NULL, num_subblocks,
                                                     NULL, &out_param_size);
    if (!out_param) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    out_param->parameter_id = param->parameter_id;
    out_param->param_definition_type = param->param_definition_type;
    out_param->parameter_rate = param->parameter_rate;
    out_param->param_definition_mode = param->param_definition_mode;
    out_param->duration = duration;
    out_param->constant_subblock_duration = constant_subblock_duration;
    out_param->num_subblocks = num_subblocks;

    for (int i = 0; i < num_subblocks; i++) {
        void *subblock = avformat_iamf_param_definition_get_subblock(out_param, i);
        unsigned int subblock_duration;

        if (param->param_definition_mode && !constant_subblock_duration) {
            ret = leb(pb, &subblock_duration);
            if (ret < 0)
                goto fail;
        } else {
            switch (param->param_definition_type) {
            case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN:
                subblock_duration = ((AVIAMFMixGainParameterData *)subblock)->subblock_duration;
                break;
            case AV_IAMF_PARAMETER_DEFINITION_DEMIXING:
                subblock_duration = ((AVIAMFDemixingInfoParameterData *)subblock)->subblock_duration;
                break;
            case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN:
                subblock_duration = ((AVIAMFReconGainParameterData *)subblock)->subblock_duration;
                break;
            default:
                av_assert0(0);
            }
        }

        switch (param->param_definition_type) {
        case AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN: {
            AVIAMFMixGainParameterData *mix = subblock;

            ret = leb(pb, &mix->animation_type);
            if (ret < 0)
                goto fail;

            if (mix->animation_type > AV_IAMF_ANIMATION_TYPE_BEZIER) {
                ret = 0;
                av_free(out_param);
                goto fail;
            }

            mix->start_point_value = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
            if (mix->animation_type >= AV_IAMF_ANIMATION_TYPE_LINEAR) {
                mix->end_point_value = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
            }
            if (mix->animation_type == AV_IAMF_ANIMATION_TYPE_BEZIER) {
                mix->control_point_value = av_make_q(sign_extend(avio_rb16(pb), 16), 1 << 8);
                mix->control_point_relative_time = avio_r8(pb);
            }
            mix->subblock_duration = subblock_duration;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_DEMIXING: {
            AVIAMFDemixingInfoParameterData *demix = subblock;

            demix->dmixp_mode = avio_r8(pb) >> 5;
            demix->subblock_duration = subblock_duration;
            break;
        }
        case AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN: {
            AVIAMFReconGainParameterData *recon = subblock;
            const AVIAMFAudioElement *audio_element = param_definition->audio_element;

            av_assert0(audio_element);
            for (int i = 0; i < audio_element->num_layers; i++) {
                const AVIAMFLayer *layer = audio_element->layers[i];
                if (layer->recon_gain_is_present) {
                    unsigned int recon_gain_flags, bitcount;
                    ret = leb(pb, &recon_gain_flags);
                    if (ret < 0)
                        goto fail;

                    bitcount = 7 + 5 * !!(recon_gain_flags & 0x80);
                    recon_gain_flags = (recon_gain_flags & 0x7F) | ((recon_gain_flags & 0xFF00) >> 1);
                    for (int j = 0; j < bitcount; j++) {
                        if (recon_gain_flags & (1 << j))
                            recon->recon_gain[i][j] = avio_r8(pb);
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

    len -= avio_tell(pb);
    if (len) {
       int level = (s->error_recognition & AV_EF_EXPLODE) ? AV_LOG_ERROR : AV_LOG_WARNING;
       av_log(s, level, "Underread in parameter_block_obu. %d bytes left at the end\n", len);
    }

    switch (param->param_definition_type) {
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
    av_free(buf);

    return ret;
}

static int iamf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    uint8_t header[MAX_IAMF_OBU_HEADER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    unsigned obu_size;
    int ret;

    while (1) {
        enum IAMF_OBU_Type type;
        unsigned skip_samples, discard_padding;
        int len, size, start_pos;

        if ((ret = ffio_ensure_seekback(s->pb, MAX_IAMF_OBU_HEADER_SIZE)) < 0)
            return ret;
        size = avio_read(s->pb, header, MAX_IAMF_OBU_HEADER_SIZE);
        if (size < 0)
            return size;

        len = parse_obu_header(header, size, &obu_size, &start_pos, &type,
                               &skip_samples, &discard_padding);
        if (len < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to read obu\n");
            return len;
        }
        avio_seek(s->pb, -(size - start_pos), SEEK_CUR);

        if (type == IAMF_OBU_IA_AUDIO_FRAME)
            return audio_frame_obu(s, pkt, obu_size, type,
                                   skip_samples, discard_padding, 1);
        else if (type >= IAMF_OBU_IA_AUDIO_FRAME_ID0 && type <= IAMF_OBU_IA_AUDIO_FRAME_ID17)
            return audio_frame_obu(s, pkt, obu_size, type,
                                   skip_samples, discard_padding, 0);
        else if (type == IAMF_OBU_IA_PARAMETER_BLOCK) {
            ret = parameter_block_obu(s, obu_size);
            if (ret < 0)
                return ret;
        } else {
            int64_t offset = avio_skip(s->pb, obu_size);
            if (offset < 0)
                ret = offset;
            break;
        }
    }

    return ret;
}

static int iamf_read_close(AVFormatContext *s)
{
    IAMFDemuxContext *const c = s->priv_data;

    for (int i = 0; i < c->nb_codec_configs; i++)
        av_free(c->codec_configs[i].extradata);
    av_freep(&c->codec_configs);
    c->nb_codec_configs = 0;

    for (int i = 0; i < c->nb_audio_elements; i++)
        av_free(c->audio_elements[i].audio_substreams);
    av_freep(&c->audio_elements);
    c->nb_audio_elements = 0;

    for (int i = 0; i < c->nb_mix_presentations; i++) {
        for (int j = 0; j < c->mix_presentations[i].count_label; j++)
            av_free(c->mix_presentations[i].language_label[j]);
        av_free(c->mix_presentations[i].language_label);
    }
    av_freep(&c->mix_presentations);
    c->nb_mix_presentations = 0;

    av_freep(&c->param_definitions);
    c->nb_param_definitions = 0;

    av_freep(&c->mix);
    c->mix_size = 0;
    av_freep(&c->demix);
    c->demix_size = 0;
    av_freep(&c->recon);
    c->recon_size = 0;
    return 0;
}

const AVInputFormat ff_iamf_demuxer = {
    .name           = "iamf",
    .long_name      = NULL_IF_CONFIG_SMALL("Raw Immersive Audio Model and Formats"),
    .priv_data_size = sizeof(IAMFDemuxContext),
    .flags_internal = FF_FMT_INIT_CLEANUP,
    .read_probe     = iamf_probe,
    .read_header    = iamf_read_header,
    .read_packet    = iamf_read_packet,
    .read_close     = iamf_read_close,
    .extensions     = "iamf",
    .flags          = AVFMT_GENERIC_INDEX | AVFMT_NO_BYTE_SEEK | AVFMT_NOTIMESTAMPS | AVFMT_SHOW_IDS,
};
