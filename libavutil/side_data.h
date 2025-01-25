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

#ifndef AVUTIL_SIDE_DATA_H
#define AVUTIL_SIDE_DATA_H

#include "buffer.h"
#include "frame.h"

AVFrameSideData *ff_frame_side_data_add_from_buf(AVFrameSideData ***sd,
                                                 int *nb_sd,
                                                 enum AVFrameSideDataType type,
                                                 AVBufferRef *buf);

AVFrameSideData *ff_frame_side_data_copy(AVFrameSideData ***sd, int *nb_sd,
                                         const AVFrameSideData *src);

void ff_mdm_get_defaults(void *obj);
void ff_ave_get_defaults(void *obj);
void ff_spherical_get_defaults(void *obj);

#endif // AVUTIL_SIDE_DATA_H