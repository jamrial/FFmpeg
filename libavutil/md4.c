/*
 * Copyright (C) 2006 Michael Niedermayer (michaelni@gmx.at)
 * Copyright (C) 2003-2005 by Christopher R. Hertel (crh@ubiqx.mn.org)
 * Copyright (C) 2013 by James Almer
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

#include <stdint.h>
#include "bswap.h"
#include "intreadwrite.h"
#include "md4.h"
#include "mem.h"

typedef struct AVMD4{
    uint64_t len;
    uint8_t  block[64];
    uint32_t ABCD[4];
} AVMD4;

const int av_md4_size = sizeof(AVMD4);

struct AVMD4 *av_md4_alloc(void)
{
    return av_mallocz(sizeof(struct AVMD4));
}

static const uint8_t S[3][4] = {
    { 3,  7, 11, 19 },  /* round 1 */
    { 3,  5,  9, 13 },  /* round 2 */
    { 3,  9, 11, 15 },  /* round 3 */
};

static const uint32_t T[3] = {
    0, 0x5A827999, 0x6ED9EBA1
};

static uint8_t W2[16] = {
    0, 4,  8, 12, 1, 5,  9, 13,
    2, 6, 10, 14, 3, 7, 11, 15
};

static uint8_t W3[16] = {
    0, 8, 4, 12, 2, 10, 6, 14,
    1, 9, 5, 13, 3, 11, 7, 15
};

#define CORE(i, a, b, c, d) do {                                          \
        t = S[i >> 4][i & 3];                                             \
        a += T[(i >> 4) & 3];                                             \
                                                                          \
        if (i < 32) {                                                     \
            if (i < 16) a += (d ^ (b & (c ^ d)))           + X[i];        \
            else        a += ((b & c) | (b & d) | (c & d)) + X[W2[i-16]]; \
        } else                                                            \
                        a += (b ^ c ^ d)                   + X[W3[i-32]]; \
        a = (a << t | a >> (32 - t));                                     \
    } while (0)

static void body(uint32_t ABCD[4], uint32_t *src, int nblocks)
{
    int i av_unused;
    int n;
    uint32_t a, b, c, d, t, *X;

    for (n = 0; n < nblocks; n++) {
        a = ABCD[3];
        b = ABCD[2];
        c = ABCD[1];
        d = ABCD[0];

        X = src + n * 16;

#if HAVE_BIGENDIAN
    for (i = 0; i < 16; i++)
        X[i] = av_bswap32(X[i]);
#endif

#if CONFIG_SMALL
    for (i = 0; i < 48; i++) {
        CORE(i, a, b, c, d);
        t = d;
        d = c;
        c = b;
        b = a;
        a = t;
    }
#else
#define CORE2(i)                                                          \
    CORE( i,   a,b,c,d); CORE((i+1),d,a,b,c);                             \
    CORE((i+2),c,d,a,b); CORE((i+3),b,c,d,a)
#define CORE4(i) CORE2(i); CORE2((i+4)); CORE2((i+8)); CORE2((i+12))
    CORE4(0); CORE4(16); CORE4(32);
#endif

    ABCD[0] += d;
    ABCD[1] += c;
    ABCD[2] += b;
    ABCD[3] += a;
    }
}

void av_md4_init(AVMD4 *ctx)
{
    ctx->len     = 0;

    ctx->ABCD[0] = 0x10325476;
    ctx->ABCD[1] = 0x98badcfe;
    ctx->ABCD[2] = 0xefcdab89;
    ctx->ABCD[3] = 0x67452301;
}

void av_md4_update(AVMD4 *ctx, const uint8_t *src, int len)
{
    unsigned int i, j;

    j = ctx->len & 63;
    ctx->len += len;
#if CONFIG_SMALL
    for (i = 0; i < len; i++) {
        ctx->block[j++] = src[i];
        if (64 == j) {
            body(ctx->ABCD, (uint32_t *)ctx->block, 1);
            j = 0;
        }
    }
#else
    if ((j + len) > 63) {
        int nblocks;
        memcpy(&ctx->block[j], src, (i = 64 - j));
        body(ctx->ABCD, (uint32_t *)ctx->block, 1);
        nblocks = (len - i) >> 6;
        body(ctx->ABCD, (uint32_t *)&src[i], nblocks);
        i += nblocks * 64;
        j = 0;
    } else
        i = 0;
    memcpy(&ctx->block[j], &src[i], len - i);
#endif
}

void av_md4_final(AVMD4 *ctx, uint8_t *dst)
{
    int i;
    uint64_t finalcount = av_le2ne64(ctx->len << 3);

    av_md4_update(ctx, "\200", 1);
    while ((ctx->len & 63) != 56)
        av_md4_update(ctx, "", 1);

    av_md4_update(ctx, (uint8_t *)&finalcount, 8);

    for (i = 0; i < 4; i++)
        AV_WL32(dst + 4*i, ctx->ABCD[3 - i]);
}

void av_md4_sum(uint8_t *dst, const uint8_t *src, const int len)
{
    AVMD4 ctx;

    av_md4_init(&ctx);
    av_md4_update(&ctx, src, len);
    av_md4_final(&ctx, dst);
}

#ifdef TEST
#include <stdio.h>

static void print_md4(uint8_t *md4)
{
    int i;
    for (i = 0; i < 16; i++)
        printf("%02x", md4[i]);
    printf("\n");
}

int main(void){
    uint8_t md4val[16];
    // RFC 1320 Test vectors
    av_md4_sum(md4val, "abc",   3); print_md4(md4val);
    av_md4_sum(md4val, "message digest",   14); print_md4(md4val);
    av_md4_sum(md4val, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                       "abcdefghijklmnopqrstuvwxyz"
                       "0123456789", 62); print_md4(md4val);
    av_md4_sum(md4val, "1234567890123456789012345678901234567890"
                       "1234567890123456789012345678901234567890", 80); print_md4(md4val);

    return 0;
}
#endif
