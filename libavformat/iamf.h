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

#ifndef AVFORMAT_IAMF_H
#define AVFORMAT_IAMF_H

/**
 * @file
 * Immersive Audio Model and Formats API header
 */

#include <stdint.h>
#include <stddef.h>

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/rational.h"

struct AVStreamGroup;

enum AVIAMFAudioElementType {
    AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL,
    AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE,
};

/**
 * @defgroup lavf_iamf_params Parameter Definition
 * @{
 * Parameters as defined in section 3.6.1 and 3.8
 * @}
 * @defgroup lavf_iamf_audio Audio Element
 * @{
 * Audio Elements as defined in section 3.6
 * @}
 * @defgroup lavf_iamf_mix Mix Presentation
 * @{
 * Mix Presentations as defined in section 3.7
 * @}
 *
 * @}
 * @addtogroup lavf_iamf_params
 * @{
 */
enum AVIAMFAnimationType {
    AV_IAMF_ANIMATION_TYPE_STEP,
    AV_IAMF_ANIMATION_TYPE_LINEAR,
    AV_IAMF_ANIMATION_TYPE_BEZIER,
};

/**
 * Mix Gain Parameter Data as defined in section 3.8.1
 *
 * Subblocks in AVIAMFParamDefinition use this struct when the value or
 * @ref AVIAMFParamDefinition.param_definition_type param_definition_type is
 * AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN.
 */
typedef struct AVIAMFMixGainParameterData {
    const AVClass *av_class;

    // AVOption enabled fields
    unsigned int subblock_duration;
    enum AVIAMFAnimationType animation_type;
    AVRational start_point_value;
    AVRational end_point_value;
    AVRational control_point_value;
    unsigned int control_point_relative_time;
} AVIAMFMixGainParameterData;

/**
 * Demixing Info Parameter Data as defined in section 3.8.2
 *
 * Subblocks in AVIAMFParamDefinition use this struct when the value or
 * @ref AVIAMFParamDefinition.param_definition_type param_definition_type is
 * AV_IAMF_PARAMETER_DEFINITION_DEMIXING.
 */
typedef struct AVIAMFDemixingInfoParameterData {
    const AVClass *av_class;

    // AVOption enabled fields
    unsigned int subblock_duration;
    unsigned int dmixp_mode;
} AVIAMFDemixingInfoParameterData;

/**
 * Recon Gain Info Parameter Data as defined in section 3.8.3
 *
 * Subblocks in AVIAMFParamDefinition use this struct when the value or
 * @ref AVIAMFParamDefinition.param_definition_type param_definition_type is
 * AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN.
 */
typedef struct AVIAMFReconGainParameterData {
    const AVClass *av_class;

    // AVOption enabled fields
    unsigned int subblock_duration;
    // End of AVOption enabled fields
    uint8_t recon_gain[6][12];
} AVIAMFReconGainParameterData;

enum AVIAMFParamDefinitionType {
    AV_IAMF_PARAMETER_DEFINITION_MIX_GAIN,
    AV_IAMF_PARAMETER_DEFINITION_DEMIXING,
    AV_IAMF_PARAMETER_DEFINITION_RECON_GAIN,
};

/**
 * Parameters as defined in section 3.6.1
 */
typedef struct AVIAMFParamDefinition {
    const AVClass *av_class;

    size_t subblocks_offset;
    size_t subblock_size;

    enum AVIAMFParamDefinitionType param_definition_type;
    unsigned int num_subblocks;

    // AVOption enabled fields
    unsigned int parameter_id;
    unsigned int parameter_rate;
    unsigned int param_definition_mode;
    unsigned int duration;
    unsigned int constant_subblock_duration;
} AVIAMFParamDefinition;

const AVClass *avformat_iamf_param_definition_get_class(void);

AVIAMFParamDefinition *avformat_iamf_param_definition_alloc(enum AVIAMFParamDefinitionType param_definition_type,
                                                            AVDictionary **options,
                                                            unsigned int num_subblocks, AVDictionary **subblock_options,
                                                            size_t *size);

/**
 * Get the subblock at the specified {@code idx}. Must be between 0 and num_subblocks - 1.
 *
 * The @ref AVIAMFParamDefinition.param_definition_type "param definition type" defines
 * the struct type of the returned pointer.
 */
static av_always_inline void*
avformat_iamf_param_definition_get_subblock(AVIAMFParamDefinition *par, unsigned int idx)
{
    av_assert0(idx < par->num_subblocks);
    return (void *)((uint8_t *)par + par->subblocks_offset + idx * par->subblock_size);
}

/**
 * @}
 * @addtogroup lavf_iamf_audio
 * @{
 */

enum AVIAMFAmbisonicsMode {
    AV_IAMF_AMBISONICS_MODE_MONO,
    AV_IAMF_AMBISONICS_MODE_PROJECTION,
};

/**
 * A layer defining a Channel Layout in the Audio Element.
 *
 * When audio_element_type is AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL, this
 * corresponds to an Scalable Channel Layout layer as defined in section 3.6.2.
 * For AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE, it is an Ambisonics channel
 * layout as defined in section 3.6.3
 */
typedef struct AVIAMFLayer {
    const AVClass *av_class;

    // AVOption enabled fields
    AVChannelLayout ch_layout;
    unsigned int substream_count;

    unsigned int recon_gain_is_present;
    /**
     * Output gain flags as defined in section 3.6.2
     *
     * This field is defined only if audio_element_type is
     * AV_IAMF_AUDIO_ELEMENT_TYPE_CHANNEL, must be 0 otherwise.
     */
    unsigned int output_gain_flags;
    /**
     * Output gain as defined in section 3.6.2
     *
     * Must be 0 if @ref output_gain_flags is 0.
     */
    AVRational output_gain;
    /**
     * Ambisonics mode as defined in section 3.6.3
     *
     * This field is defined only if audio_element_type is
     * AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE, must be 0 otherwise.
     *
     * If 0, channel_mapping is defined implicitly (Ambisonic Order)
     * or explicitly (Custom Order with ambi channels) in @ref ch_layout.
     * If 1, @ref demixing_matrix must be set.
     */
    enum AVIAMFAmbisonicsMode ambisonics_mode;

    // End of AVOption enabled fields
    /**
     * Demixing matrix as defined in section 3.6.3
     *
     * Set only if @ref ambisonics_mode == 1, must be NULL otherwise.
     */
    AVRational *demixing_matrix;
} AVIAMFLayer;

typedef struct AVIAMFAudioElement {
    const AVClass *av_class;

    AVIAMFLayer **layers;
    /**
     * Number of layers, or channel groups, in the Audio Element.
     * For audio_element_type AV_IAMF_AUDIO_ELEMENT_TYPE_SCENE, there
     * may be exactly 1.
     *
     * Set by avformat_iamf_audio_element_add_layer(), must not be
     * modified by any other code.
     */
    unsigned int num_layers;

    unsigned int codec_config_id;

    AVIAMFParamDefinition *demixing_info;
    AVIAMFParamDefinition *recon_gain_info;

    // AVOption enabled fields
    /**
     * Audio element type as defined in section 3.6
     */
    enum AVIAMFAudioElementType audio_element_type;

    /**
     * Default weight value as defined in section 3.6
     */
    unsigned int default_w;
} AVIAMFAudioElement;

const AVClass *avformat_iamf_audio_element_get_class(void);

AVIAMFAudioElement *avformat_iamf_audio_element_alloc(void);

int avformat_iamf_audio_element_add_layer(AVIAMFAudioElement *audio_element, AVDictionary **options);

void avformat_iamf_audio_element_free(AVIAMFAudioElement **audio_element);

/**
 * @}
 * @addtogroup lavf_iamf_mix
 * @{
 */

enum AVIAMFHeadphonesMode {
    AV_IAMF_HEADPHONES_MODE_STEREO,
    AV_IAMF_HEADPHONES_MODE_BINAURAL,
};

typedef struct AVIAMFSubmixElement {
    const AVClass *av_class;

    const struct AVStreamGroup *audio_element;

    AVIAMFParamDefinition *element_mix_config;

    // AVOption enabled fields
    enum AVIAMFHeadphonesMode headphones_rendering_mode;

    AVRational default_mix_gain;

    /**
     * A dictionary of string describing the submix. Must have the same
     * amount of entries as @ref AVIAMFMixPresentation.annotations "the
     * mix's annotations".
     *
     * decoding: set by libavformat
     * encoding: set by the user
     */
    AVDictionary *annotations;
} AVIAMFSubmixElement;

enum AVIAMFAnchorElement {
    AV_IAMF_ANCHOR_ELEMENT_UNKNOWN,
    AV_IAMF_ANCHOR_ELEMENT_DIALOGUE,
    AV_IAMF_ANCHOR_ELEMENT_ALBUM,
};

enum AVIAMFSubmixLayoutType {
    AV_IAMF_SUBMIX_LAYOUT_TYPE_LOUDSPEAKERS = 2,
    AV_IAMF_SUBMIX_LAYOUT_TYPE_BINAURAL = 3,
};

typedef struct AVIAMFSubmixLayout {
    const AVClass *av_class;

    // AVOption enabled fields
    enum AVIAMFSubmixLayoutType layout_type;
    AVChannelLayout sound_system;
    AVRational integrated_loudness;
    AVRational digital_peak;
    AVRational true_peak;
    AVRational anchored_loudness[3];
} AVIAMFSubmixLayout;

typedef struct AVIAMFSubmix {
    const AVClass *av_class;

    AVIAMFSubmixElement **elements;
    /**
     * Set by avformat_iamf_mix_presentation_add_submix(), must not be
     * modified by any other code.
     */
    unsigned int num_elements;

    AVIAMFSubmixLayout **layouts;
    /**
     * Set by avformat_iamf_mix_presentation_add_submix(), must not be
     * modified by any other code.
     */
    unsigned int num_layouts;

    AVIAMFParamDefinition *output_mix_config;

    // AVOption enabled fields
    AVRational default_mix_gain;
} AVIAMFSubmix;

typedef struct AVIAMFMixPresentation {
    const AVClass *av_class;

    AVIAMFSubmix **submixes;
    /**
     * Number of submixes in the presentation.
     *
     * Set by avformat_iamf_mix_presentation_add_submix(), must not be
     * modified by any other code.
     */
    unsigned int num_submixes;

    // AVOption enabled fields
    /**
     * A dictionary of string describing the mix. Must have the same
     * amount of entries as every @ref AVIAMFSubmixElement.annotations
     * "Submix element annotations".
     *
     * decoding: set by libavformat
     * encoding: set by the user
     */
    AVDictionary *annotations;
} AVIAMFMixPresentation;

const AVClass *avformat_iamf_mix_presentation_get_class(void);

AVIAMFMixPresentation *avformat_iamf_mix_presentation_alloc(void);

int avformat_iamf_mix_presentation_add_submix(AVIAMFMixPresentation *mix_presentation,
                                              AVDictionary **options);

int avformat_iamf_submix_add_element(AVIAMFSubmix *submix, AVDictionary **options);

int avformat_iamf_submix_add_layout(AVIAMFSubmix *submix, AVDictionary **options);

void avformat_iamf_mix_presentation_free(AVIAMFMixPresentation **mix_presentation);
/**
 * @}
 */

#endif /* AVFORMAT_IAMF_H */
