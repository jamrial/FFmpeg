/*
 * copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
 * copyright (c) 2013 James Almer <jamrial@gmail.com>
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

#ifndef AVUTIL_MD4_H
#define AVUTIL_MD4_H

#include <stdint.h>

#include "attributes.h"
#include "version.h"

/**
 * @defgroup lavu_md4 MD4
 * @ingroup lavu_crypto
 * @{
 */

extern const int av_md4_size;

struct AVMD4;

/**
 * Allocate an AVMD4 context.
 */
struct AVMD4 *av_md4_alloc(void);

/**
 * Initialize MD4 hashing.
 *
 * @param ctx pointer to the function context (of size av_md4_size)
 */
void av_md4_init(struct AVMD4 *ctx);

/**
 * Update hash value.
 *
 * @param ctx hash function context
 * @param src input data to update hash with
 * @param len input data length
 */
void av_md4_update(struct AVMD4 *ctx, const uint8_t *src, int len);

/**
 * Finish hashing and output digest value.
 *
 * @param ctx hash function context
 * @param dst buffer where output digest value is stored
 */
void av_md4_final(struct AVMD4 *ctx, uint8_t *dst);

/**
 * Hash an array of data.
 *
 * @param dst The output buffer to write the digest into
 * @param src The data to hash
 * @param len The length of the data, in bytes
 */
void av_md4_sum(uint8_t *dst, const uint8_t *src, const int len);

/**
 * @}
 */

#endif /* AVUTIL_MD4_H */
