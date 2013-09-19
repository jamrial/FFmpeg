/*
 * Copyright (C) 2007 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (C) 2013 James Almer <jamrial@gmail.com>
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

#ifndef AVUTIL_WHIRLPOOL_H
#define AVUTIL_WHIRLPOOL_H

#include <stdint.h>

#include "attributes.h"
#include "version.h"

/**
 * @defgroup lavu_whirlpool Whirlpool
 * @ingroup lavu_crypto
 * @{
 */

extern const int av_whirlpool_size;

struct AVWhirlpool;

/**
 * Allocate an AVWhirlpool context.
 */
struct AVWhirlpool *av_whirlpool_alloc(void);

/**
 * Initialize Whirlpool hashing.
 *
 * @param context pointer to the function context (of size av_whirlpool_size)
 */
void av_whirlpool_init(struct AVWhirlpool* context);

/**
 * Update hash value.
 *
 * @param context hash function context
 * @param data    input data to update hash with
 * @param len     input data length
 */
void av_whirlpool_update(struct AVWhirlpool* context, const uint8_t* data, unsigned int len);

/**
 * Finish hashing and output digest value.
 *
 * @param context hash function context
 * @param digest  buffer where output digest value is stored
 */
void av_whirlpool_final(struct AVWhirlpool* context, uint8_t *digest);

/**
 * Hash an array of data.
 *
 * @param dst The output buffer to write the digest into
 * @param src The data to hash
 * @param len The length of the data, in bytes
 */
void av_whirlpool_sum(uint8_t *dst, const uint8_t *src, const int len);

/**
 * @}
 */

#endif /* AVUTIL_WHIRLPOOL_H */
