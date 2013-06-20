/*
 * copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
 * copyright (C) 2013 James Almer <jamrial@gmail.com>
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

#ifndef AVUTIL_CRC64_H
#define AVUTIL_CRC64_H

#include <stdint.h>
#include <stddef.h>
#include "attributes.h"

/**
 * @defgroup lavu_crc64 CRC64
 * @ingroup lavu_crypto
 * @{
 */

typedef uint64_t AVCRC64;

typedef enum {
    AV_CRC64_64_ECMA,
    AV_CRC64_64_ECMA_LE, /*< reversed bitorder version of AV_CRC64_64_ECMA */
    AV_CRC64_MAX,        /*< Not part of public API! Do not use outside libavutil. */
}AVCRC64Id;

/**
 * Initialize a CRC table.
 * @param ctx must be an array of size sizeof(AVCRC64)*257 or sizeof(AVCRC64)*2048
 * @param le If 1, the lowest bit represents the coefficient for the highest
 *           exponent of the corresponding polynomial (both for poly and
 *           actual CRC).
 *           If 0, you must swap the CRC parameter and the result of av_crc64
 *           if you need the standard representation.
 * @param bits number of bits for the CRC
 * @param poly generator polynomial without the x**bits coefficient, in the
 *             representation as specified by le
 * @param ctx_size size of ctx in bytes
 * @return <0 on failure
 */
int av_crc64_init(AVCRC64 *ctx, int le, int bits, uint64_t poly, int ctx_size);

/**
 * Get an initialized standard CRC table.
 * @param crc_id ID of a standard CRC
 * @return a pointer to the CRC table or NULL on failure
 */
const AVCRC64 *av_crc64_get_table(AVCRC64Id crc_id);

/**
 * Calculate the CRC of a block.
 * @param crc CRC of previous blocks if any or initial value for CRC
 * @return CRC updated with the data from the given block
 *
 * @see av_crc64_init() "le" parameter
 */
uint64_t av_crc64(const AVCRC64 *ctx, uint64_t crc,
                  const uint8_t *buffer, size_t length) av_pure;

#endif /* AVUTIL_CRC64_H */
