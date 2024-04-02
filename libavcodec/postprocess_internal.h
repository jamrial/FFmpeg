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

#ifndef AVCODEC_POSTPROCESS_INTERNAL_H
#define AVCODEC_POSTPROCESS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "postprocess.h"

typedef struct FFPostProcType {
    int (*init)(FFPostProc *pp, void *opaque);
    int (*get_buffer)(FFPostProc *pp, void *opaque, void *obj, int flags);
    int (*process)(FFPostProc *pp, void *opaque, void *obj);
    void (*close)(FFPostProc *pp);

    size_t priv_data_size;

    enum FFPostProcEnum type;
} FFPostProcType;

extern const FFPostProcType ff_lcevc_postproc;

#endif /* AVCODEC_POSTPROCESS_H */
