/*
 * AV1 common parsing code
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

#include "config.h"

#include "libavutil/mem.h"

#include "av1_parse.h"
#include "bytestream.h"

static int parse_obu_header(AV1OBU *obu, const uint8_t *buf, int buf_size,
                            int64_t *obu_size, int *start_pos)
{
    GetBitContext gb;
    int ret, extension_flag, has_size_flag;

    ret = init_get_bits8(&gb, buf, buf_size);
    if (ret < 0)
        return ret;

    if (get_bits1(&gb) != 0) // obu_forbidden_bit
        return AVERROR_INVALIDDATA;

    obu->type      = get_bits(&gb, 4);
    extension_flag = get_bits1(&gb);
    has_size_flag  = get_bits1(&gb);
    skip_bits1(&gb); // obu_reserved_1bit

    if (extension_flag) {
        obu->temporal_id = get_bits(&gb, 3);
        obu->spatial_id  = get_bits(&gb, 2);
        skip_bits(&gb, 3); // extension_header_reserved_3bits
    } else {
        obu->temporal_id = obu->spatial_id = 0;
    }

    *obu_size  = has_size_flag ? leb128(&gb)
                               : buf_size - 1 - extension_flag;
    *start_pos = get_bits_count(&gb) / 8;

    return 0;
}

int ff_av1_extract_obu(AV1OBU *obu, const uint8_t *buf, int length, void *logctx)
{
    int ret, start_pos;
    int64_t obu_size;

    ret = parse_obu_header(obu, buf, length, &obu_size, &start_pos);
    if (ret < 0)
        return ret;

    if (obu_size > INT_MAX / 8 || obu_size < 0)
        return AVERROR(ERANGE);

    length = obu_size + start_pos;

    obu->data     = buf + start_pos;
    obu->size     = obu_size;
    obu->raw_data = buf;
    obu->raw_size = length;

    ret = init_get_bits(&obu->gb, obu->data, obu->size * 8);
    if (ret < 0)
        return ret;

    av_log(logctx, AV_LOG_DEBUG,
           "obu_type: %d, temporal_id: %d, spatial_id: %d, payload size: %d\n",
           obu->type, obu->temporal_id, obu->spatial_id, obu->size);

    return length;
}

int ff_av1_packet_split(AV1Packet *pkt, const uint8_t *buf, int length, void *logctx)
{
    GetByteContext bc;
    int consumed;

    bytestream2_init(&bc, buf, length);
    pkt->nb_obus = 0;

    while (bytestream2_get_bytes_left(&bc) >= 1) {
        AV1OBU *obu;

        if (pkt->obus_allocated < pkt->nb_obus + 1) {
            int new_size = pkt->obus_allocated + 1;
            AV1OBU *tmp = av_realloc_array(pkt->obus, new_size, sizeof(*tmp));
            if (!tmp)
                return AVERROR(ENOMEM);

            pkt->obus = tmp;
            memset(pkt->obus + pkt->obus_allocated, 0,
                   (new_size - pkt->obus_allocated) * sizeof(*tmp));
            pkt->obus_allocated = new_size;
        }
        obu = &pkt->obus[pkt->nb_obus];

        consumed = ff_av1_extract_obu(obu, bc.buffer, bytestream2_get_bytes_left(&bc), logctx);
        if (consumed < 0)
            return consumed;

        pkt->nb_obus++;

        bytestream2_skip(&bc, consumed);
    }

    return 0;
}

void ff_av1_packet_uninit(AV1Packet *pkt)
{
    av_freep(&pkt->obus);
    pkt->obus_allocated = 0;
}
