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

/**
 * @file
 * @ingroup lavu_video_3d_reference_displays_info
 * 3D Reference Displays Information
 */

#ifndef AVUTIL_REF_DISPLAYS_INFO_H
#define AVUTIL_REF_DISPLAYS_INFO_H

#include <stddef.h>
#include <stdint.h>

#include "libavutil/avassert.h"

/**
 * @defgroup lavu_video_3d_reference_displays_info 3D Reference Displays Information
 * @ingroup lavu_video
 *
 * The 3D Reference Displays Information describes information about the reference display
 * width(s) and reference viewing distance(s) as well as information about the corresponding
 * reference stereo pair(s).
 * @{
 */

/**
 * This structure describes information about the reference display width(s) and reference
 * viewing distance(s) as well as information about the corresponding reference stereo pair(s).
 * See section G.14.3.2.3 of ITU-T H.265 for more information.
 *
 * @note The struct must be allocated with av_ref_displays_info_alloc() and its size is not a
 *       part of the public ABI.
 */
typedef struct AV3DReferenceDisplaysInfo {
    /**
     * The number of reference displays that are signalled in this struct.
     * Allowed range is 1 to 32, inclusive.
     */
    unsigned int num_ref_displays;

    /**
     * Offset in bytes from the beginning of this structure at which the array
     * of reference displays starts.
     */
    size_t ref_offset;

    /**
     * Size of each reference display entry in bytes. May not match sizeof(AV3DReferenceDisplay).
     */
    size_t ref_size;

    /**
     * The exponent of the maximum allowable truncation error for
     * {exponent,mantissa}_ref_display_width as given by 2<sup>(-prec_ref_display_width)</sup>.
     */
    uint8_t prec_ref_display_width;

    /**
     * A flag to indicate the presence of reference viewing distance.
     * If false, the values of prec_ref_viewing_dist, exponent_ref_viewing_distance,
     * and mantissa_ref_viewing_distance are undefined.
     */
    uint8_t ref_viewing_distance_flag;

    /**
     * The exponent of the maximum allowable truncation error for
     * {exponent,mantissa}_ref_viewing_distance as given by 2<sup>^(-prec_ref_viewing_dist)</sup>.
     * The value of prec_ref_viewing_dist shall be in the range of 0 to 31, inclusive.
     */
    uint8_t prec_ref_viewing_dist;
} AV3DReferenceDisplaysInfo;

/**
 * Data structure for storing a reference display information.
 * It is allocated as a part of AV3DReferenceDisplaysInfo and should be retrieved with
 * av_ref_displays_info_display().
 *
 * sizeof(AV3DReferenceDisplay) is not a part of the ABI and new fields may be
 * added to it.
 */
typedef struct AV3DReferenceDisplay {
    /**
     * The ViewId of the left view of a stereo pair corresponding to the n-th reference display.
     */
    uint16_t left_view_id;

    /**
     * The ViewId of the left view of a stereo pair corresponding to the n-th reference display.
     */
    uint16_t right_view_id;

    /**
     * The exponent part of the reference display width of the n-th reference display.
     */
    uint8_t exponent_ref_display_width;

    /**
     * The mantissa part of the reference display width of the n-th reference display.
     */
    uint8_t mantissa_ref_display_width;

    /**
     * Tthe exponent part of the reference viewing distance of the n-th reference display.
     */
    uint8_t exponent_ref_viewing_distance;

    /**
     * The mantissa part of the reference viewing distance of the n-th reference display.
     */
    uint8_t mantissa_ref_viewing_distance;

    /**
     * An array of flags to indicates that the information about additional horizontal shift of
     * the left and right views for the n-th reference display is present.
     */
    uint8_t additional_shift_present_flag;

    /**
     * The recommended additional horizontal shift for a stereo pair corresponding to the n-th
     * reference baseline and the n-th reference display.
     */
    int16_t num_sample_shift;
} AV3DReferenceDisplay;

/**
 * Get the reference display at the specified {@code idx}.
 * Must be between 0 and num_ref_displays - 1.
 */
static av_always_inline AV3DReferenceDisplay*
av_ref_displays_info_display(AV3DReferenceDisplaysInfo *rdi, unsigned int idx)
{
    av_assert0(idx < rdi->num_ref_displays);
    return (AV3DReferenceDisplay *)((uint8_t *)rdi + rdi->ref_offset +
                                    idx * rdi->ref_size);
}

/**
 * Allocates memory for AV3DReferenceDisplaysInfo, plus an array of
 * {@code num_ref_displays} AV3DReferenceDisplay and initializes the variables.
 * Can be freed with a normal av_free() call.
 *
 * @param num_ref_displays the number of reference displays.
 * @return the newly allocated struct or NULL on failure
 */
AV3DReferenceDisplaysInfo *av_ref_displays_info_alloc(unsigned int num_ref_displays,
                                                      size_t *out_size);

/**
 * @}
 */

#endif /* AVUTIL_REF_DISPLAYS_INFO_H */
