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

#ifndef AVCODEC_LCEVCDEC_H
#define AVCODEC_LCEVCDEC_H

#include "config_components.h"

#include <stdint.h>
#if CONFIG_LIBLCEVC_DEC
#include <LCEVC/lcevc_dec.h>
#else
typedef uintptr_t LCEVC_DecoderHandle;
#endif

typedef struct FFLCEVCContext {
    LCEVC_DecoderHandle decoder;
} FFLCEVCContext;

struct AVFrame;

int ff_lcevc_process(void *logctx, struct AVFrame *frame);
int ff_lcevc_send_frame(void *logctx, FFLCEVCContext *lcevc, const AVFrame *in);
int ff_lcevc_receive_frame(void *logctx, FFLCEVCContext *lcevc, AVFrame *out);
int ff_lcevc_init(FFLCEVCContext *lcevc, void *logctx);
void ff_lcevc_unref(void *opaque);
void ff_lcevc_free(void *opaque, uint8_t *data);

#endif /* AVCODEC_LCEVCDEC_H */
