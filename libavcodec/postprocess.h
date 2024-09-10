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

#ifndef AVCODEC_POSTPROCESS_H
#define AVCODEC_POSTPROCESS_H

#include <stdint.h>

struct FFPostProcType;

enum FFPostProcEnum {
    FF_POSTPROC_TYPE_NONE,
    FF_POSTPROC_TYPE_CUSTOM,
    FF_POSTPROC_TYPE_LCEVC,
    FF_POSTPROC_TYPE_NB,
};

#define FF_POSTPROC_CAP_GET_BUFFER (1 << 0)

typedef struct FFPostProc {
    const struct FFPostProcType *type;
    unsigned int caps;
    void *priv_data;
} FFPostProc;

FFPostProc *ff_postproc_alloc(void);

int ff_postproc_init(FFPostProc *pp, void *opaque, enum FFPostProcEnum type);
int ff_postproc_init_custom(FFPostProc *pp,
                            int (*process)(FFPostProc *pp, void *opaque, void *obj));

int ff_postproc_is_open(FFPostProc *pp);

int ff_postproc_process(FFPostProc *pp, void *opaque, void *obj);
int ff_postproc_get_buffer(FFPostProc *pp, void *opaque, void *obj, int flags);

#endif /* AVCODEC_POSTPROCESS_H */
