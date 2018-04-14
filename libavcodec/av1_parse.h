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

#ifndef AVCODEC_AV1_PARSE_H
#define AVCODEC_AV1_PARSE_H

#include <stdint.h>

#include "avcodec.h"
#include "get_bits.h"

typedef struct AV1OBU {
    /** Size of payload */
    int size;
    const uint8_t *data;

    /** Size of entire OBU, including header */
    int raw_size;
    const uint8_t *raw_data;

    /** GetBitContext initialized to the start of the payload */
    GetBitContext gb;

    int type;

    int temporal_id;
    int spatial_id;
} AV1OBU;

/** An input packet split into OBUs */
typedef struct AV1Packet {
    AV1OBU *obus;
    int nb_obus;
    int obus_allocated;
} AV1Packet;

/**
 * Extract an OBU from a raw bitstream.
 *
 * @note This function does not copy or store any bistream data. All
 *       the pointers in the AV1OBU structure will be valid as long
 *       as the input buffer also is.
 */
int ff_av1_extract_obu(AV1OBU *obu, const uint8_t *buf, int length,
                       void *logctx);

/**
 * Split an input packet into OBUs.
 *
 * @note This function does not copy or store any bistream data. All
 *       the pointers in the AV1Packet structure will be valid as
 *       long as the input buffer also is.
 */
int ff_av1_packet_split(AV1Packet *pkt, const uint8_t *buf, int length,
                        void *logctx);

/**
 * Free all the allocated memory in the packet.
 */
void ff_av1_packet_uninit(AV1Packet *pkt);


static inline int64_t leb128(GetBitContext *gb) {
    int64_t ret = 0;
    int i;

    for (i = 0; i < 8; i++) {
        int byte = get_bits(gb, 8);
        ret |= (int64_t)(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80))
            break;
    }
    return ret;
}

#endif /* AVCODEC_AV1_PARSE_H */
