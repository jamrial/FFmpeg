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

#include "libavutil/intreadwrite.h"

#include "avcodec.h"
#include "rangecoder.h"

static int parse(AVCodecParserContext *s,
                 AVCodecContext *avctx,
                 const uint8_t **poutbuf, int *poutbuf_size,
                 const uint8_t *buf, int buf_size)
{
    RangeCoder c;
    uint8_t keystate = 128;
    ff_init_range_decoder(&c, buf, buf_size);
    ff_build_rac_states(&c, 0.05 * (1LL << 32), 256 - 8);

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    s->key_frame = get_rac(&c, &keystate);
    s->pict_type = AV_PICTURE_TYPE_I; //FIXME I vs. P, see ffv1dec.c
    s->field_order = AV_FIELD_UNKNOWN;
    s->picture_structure = AV_PICTURE_STRUCTURE_UNKNOWN;

    return buf_size;
}

const AVCodecParser ff_ffv1_parser = {
    .codec_ids    = { AV_CODEC_ID_FFV1 },
    .parser_parse = parse,
};
