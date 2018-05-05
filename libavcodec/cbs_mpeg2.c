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

#include "libavutil/avassert.h"

#include "cbs.h"
#include "cbs_internal.h"
#include "cbs_mpeg2.h"
#include "internal.h"


#define HEADER(name) do { \
        ff_cbs_trace_header(ctx, name); \
    } while (0)

#define CHECK(call) do { \
        err = (call); \
        if (err < 0) \
            return err; \
    } while (0)

#define FUNC_NAME(rw, codec, name) cbs_ ## codec ## _ ## rw ## _ ## name
#define FUNC_MPEG2(rw, name) FUNC_NAME(rw, mpeg2, name)
#define FUNC(name) FUNC_MPEG2(READWRITE, name)

#define SUBSCRIPTS(subs, ...) (subs > 0 ? ((int[subs + 1]){ subs, __VA_ARGS__ }) : NULL)

#define ui(width, name) \
        xui(width, name, current->name, 0)
#define uis(width, name, subs, ...) \
        xui(width, name, current->name, subs, __VA_ARGS__)


#define READ
#define READWRITE read
#define RWContext GetBitContext

#define xui(width, name, var, subs, ...) do { \
        uint32_t value = 0; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, width, #name, \
                                   SUBSCRIPTS(subs, __VA_ARGS__), \
                                   &value, 0, (1 << width) - 1)); \
        var = value; \
    } while (0)

#define marker_bit() do { \
        av_unused uint32_t one; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, 1, "marker_bit", NULL, &one, 1, 1)); \
    } while (0)

#define nextbits(width, compare, var) \
    (get_bits_left(rw) >= width && \
     (var = show_bits(rw, width)) == (compare))

#include "cbs_mpeg2_syntax_template.c"

#undef READ
#undef READWRITE
#undef RWContext
#undef xui
#undef marker_bit
#undef nextbits


#define WRITE
#define READWRITE write
#define RWContext PutBitContext

#define xui(width, name, var, subs, ...) do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, width, #name, \
                                    SUBSCRIPTS(subs, __VA_ARGS__), \
                                    var, 0, (1 << width) - 1)); \
    } while (0)

#define marker_bit() do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, 1, "marker_bit", NULL, 1, 1, 1)); \
    } while (0)

#define nextbits(width, compare, var) (var)

#include "cbs_mpeg2_syntax_template.c"

#undef READ
#undef READWRITE
#undef RWContext
#undef xui
#undef marker_bit
#undef nextbits


static void cbs_mpeg2_free_user_data(void *unit, uint8_t *content)
{
    MPEG2RawUserData *user = (MPEG2RawUserData*)content;
    av_buffer_unref(&user->user_data_ref);
    av_freep(&content);
}

static void cbs_mpeg2_free_slice(void *unit, uint8_t *content)
{
    MPEG2RawSlice *slice = (MPEG2RawSlice*)content;
    av_buffer_unref(&slice->header.extra_information_ref);
    av_buffer_unref(&slice->data_ref);
    av_freep(&content);
}

static int cbs_mpeg2_split_fragment(CodedBitstreamContext *ctx,
                                    CodedBitstreamFragment *frag,
                                    int header)
{
    const uint8_t *start, *end;
    uint8_t *unit_data;
    uint32_t start_code = -1, next_start_code = -1;
    size_t unit_size;
    int err, i, unit_type;

    start = avpriv_find_start_code(frag->data, frag->data + frag->data_size,
                                   &start_code);
    for (i = 0;; i++) {
        end = avpriv_find_start_code(start, frag->data + frag->data_size,
                                     &next_start_code);

        unit_type = start_code & 0xff;

        // The start and end pointers point at to the byte following the
        // start_code_identifier in the start code that they found.
        if (end == frag->data + frag->data_size) {
            // We didn't find a start code, so this is the final unit.
            unit_size = end - (start - 1);
        } else {
            // Unit runs from start to the beginning of the start code
            // pointed to by end (including any padding zeroes).
            unit_size = (end - 4) - (start - 1);
        }

        unit_data = (uint8_t *)start - 1;

        err = ff_cbs_insert_unit_data(ctx, frag, i, unit_type,
                                      unit_data, unit_size, frag->data_ref);
        if (err < 0)
            return err;

        if (end == frag->data + frag->data_size)
            break;

        start_code = next_start_code;
        start = end;
    }

    return 0;
}

static int cbs_mpeg2_read_unit(CodedBitstreamContext *ctx,
                               CodedBitstreamUnit *unit)
{
    GetBitContext gbc;
    int err;

    err = init_get_bits(&gbc, unit->data, 8 * unit->data_size);
    if (err < 0)
        return err;

    if (MPEG2_START_IS_SLICE(unit->type)) {
        MPEG2RawSlice *slice;
        int pos, len;

        err = ff_cbs_alloc_unit_content(ctx, unit, sizeof(*slice),
                                        &cbs_mpeg2_free_slice);
        if (err < 0)
            return err;
        slice = unit->content;

        err = cbs_mpeg2_read_slice_header(ctx, &gbc, &slice->header);
        if (err < 0)
            return err;

        pos = get_bits_count(&gbc);
        len = unit->data_size;

        slice->data_size = len - pos / 8;
        slice->data_ref  = av_buffer_ref(unit->data_ref);
        if (!slice->data_ref)
            return AVERROR(ENOMEM);
        slice->data = unit->data + pos / 8;

        slice->data_bit_start = pos % 8;

    } else {
        switch (unit->type) {
#define START(start_code, type, read_func, free_func) \
        case start_code: \
            { \
                type *header; \
                err = ff_cbs_alloc_unit_content(ctx, unit, \
                                                sizeof(*header), free_func); \
                if (err < 0) \
                    return err; \
                header = unit->content; \
                err = cbs_mpeg2_read_ ## read_func(ctx, &gbc, header); \
                if (err < 0) \
                    return err; \
            } \
            break;
            START(0x00, MPEG2RawPictureHeader,  picture_header,  NULL);
            START(0xb2, MPEG2RawUserData,       user_data,
                                            &cbs_mpeg2_free_user_data);
            START(0xb3, MPEG2RawSequenceHeader, sequence_header, NULL);
            START(0xb5, MPEG2RawExtensionData,  extension_data,  NULL);
            START(0xb8, MPEG2RawGroupOfPicturesHeader,
                                       group_of_pictures_header, NULL);
#undef START
        default:
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Unknown start code %02"PRIx32".\n",
                   unit->type);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int cbs_mpeg2_write_header(CodedBitstreamContext *ctx,
                                  CodedBitstreamUnit *unit,
                                  PutBitContext *pbc,
                                  AVBufferRef **pbuf)
{
    CodedBitstreamMPEG2Context *priv = ctx->priv_data;
    AVBufferRef *buf;
    int err;

    switch (unit->type) {
#define START(start_code, type, func) \
    case start_code: \
        buf = av_buffer_pool_get(priv->func##_pool); \
        if (!buf) \
            return AVERROR(ENOMEM); \
        *pbuf = buf; \
        init_put_bits(pbc, buf->data, buf->size); \
        err = cbs_mpeg2_write_ ## func(ctx, pbc, unit->content); \
        break;
        START(0x00, MPEG2RawPictureHeader,  picture_header);
        START(0xb3, MPEG2RawSequenceHeader, sequence_header);
        START(0xb5, MPEG2RawExtensionData,  extension_data);
        START(0xb8, MPEG2RawGroupOfPicturesHeader, group_of_pictures_header);
#undef START
    case 0xb2:
        {
            MPEG2RawUserData *user_data = unit->content;

            buf = av_buffer_dyn_pool_get(priv->dyn_pool,
                                         user_data->user_data_length + 1);
            if (!buf)
                return AVERROR(ENOMEM);
            *pbuf = buf;
            init_put_bits(pbc, buf->data, buf->size);
            err = cbs_mpeg2_write_user_data(ctx, pbc, user_data);
            break;
        }
    default:
        av_log(ctx->log_ctx, AV_LOG_ERROR, "Write unimplemented for start "
               "code %02"PRIx32".\n", unit->type);
        return AVERROR_PATCHWELCOME;
    }

    return err;
}

static int cbs_mpeg2_write_slice(CodedBitstreamContext *ctx,
                                 CodedBitstreamUnit *unit,
                                 PutBitContext *pbc,
                                 AVBufferRef **pbuf)
{
    CodedBitstreamMPEG2Context *priv = ctx->priv_data;
    MPEG2RawSlice *slice = unit->content;
    GetBitContext gbc;
    AVBufferRef *buf;
    size_t bits_left;
    int err;

    buf = av_buffer_dyn_pool_get(priv->dyn_pool,
                                 sizeof(slice->header) + slice->data_size +
                                 slice->header.extra_information_length);
    if (!buf)
        return AVERROR(ENOMEM);
    *pbuf = buf;

    init_put_bits(pbc, buf->data, buf->size);

    err = cbs_mpeg2_write_slice_header(ctx, pbc, &slice->header);
    if (err < 0)
        return err;

    if (slice->data) {
        init_get_bits(&gbc, slice->data, slice->data_size * 8);
        skip_bits_long(&gbc, slice->data_bit_start);

        while (get_bits_left(&gbc) > 15)
            put_bits(pbc, 16, get_bits(&gbc, 16));

        bits_left = get_bits_left(&gbc);
        put_bits(pbc, bits_left, get_bits(&gbc, bits_left));

        // Align with zeroes.
        while (put_bits_count(pbc) % 8 != 0)
            put_bits(pbc, 1, 0);
    }

    return 0;
}

static int cbs_mpeg2_write_unit(CodedBitstreamContext *ctx,
                                CodedBitstreamUnit *unit)
{
    PutBitContext pbc;
    AVBufferRef *buf = NULL;
    int err;

    if (unit->type >= 0x01 && unit->type <= 0xaf)
        err = cbs_mpeg2_write_slice(ctx, unit, &pbc, &buf);
    else
        err = cbs_mpeg2_write_header(ctx, unit, &pbc, &buf);

    if (err < 0) {
        av_buffer_unref(&buf);
        return err;
    }

    if (put_bits_count(&pbc) % 8)
        unit->data_bit_padding = 8 - put_bits_count(&pbc) % 8;
    else
        unit->data_bit_padding = 0;

    unit->data = buf->data;
    unit->data_ref = buf;
    unit->data_size = (put_bits_count(&pbc) + 7) / 8;
    flush_put_bits(&pbc);

    return 0;
}

static int cbs_mpeg2_assemble_fragment(CodedBitstreamContext *ctx,
                                       CodedBitstreamFragment *frag)
{
    uint8_t *data;
    size_t size, dp;
    int i;

    size = 0;
    for (i = 0; i < frag->nb_units; i++)
        size += 3 + frag->units[i].data_size;

    frag->data_ref = av_buffer_alloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!frag->data_ref)
        return AVERROR(ENOMEM);
    data = frag->data_ref->data;

    dp = 0;
    for (i = 0; i < frag->nb_units; i++) {
        CodedBitstreamUnit *unit = &frag->units[i];

        data[dp++] = 0;
        data[dp++] = 0;
        data[dp++] = 1;

        memcpy(data + dp, unit->data, unit->data_size);
        dp += unit->data_size;
    }

    av_assert0(dp == size);

    memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    frag->data      = data;
    frag->data_size = size;

    return 0;
}

static int cbs_mpeg2_init(CodedBitstreamContext *ctx)
{
    CodedBitstreamMPEG2Context *priv = ctx->priv_data;

    priv->picture_header_pool =
        av_buffer_pool_init(sizeof(MPEG2RawPictureHeader), NULL);
    priv->sequence_header_pool =
        av_buffer_pool_init(sizeof(MPEG2RawSequenceHeader), NULL);
    priv->extension_data_pool =
        av_buffer_pool_init(sizeof(MPEG2RawExtensionData), NULL);
    priv->group_of_pictures_header_pool =
        av_buffer_pool_init(sizeof(MPEG2RawGroupOfPicturesHeader),NULL);

    priv->dyn_pool = av_buffer_dyn_pool_init(NULL);

    if (!priv->picture_header_pool || !priv->sequence_header_pool ||
        !priv->extension_data_pool || !priv->group_of_pictures_header_pool ||
        !priv->dyn_pool)
        return AVERROR(ENOMEM);

    return 0;
}

static void cbs_mpeg2_close(CodedBitstreamContext *ctx)
{
    CodedBitstreamMPEG2Context *priv = ctx->priv_data;

    av_buffer_pool_uninit(&priv->picture_header_pool);
    av_buffer_pool_uninit(&priv->sequence_header_pool);
    av_buffer_pool_uninit(&priv->extension_data_pool);
    av_buffer_pool_uninit(&priv->group_of_pictures_header_pool);
    av_buffer_dyn_pool_uninit(&priv->dyn_pool);
}

const CodedBitstreamType ff_cbs_type_mpeg2 = {
    .codec_id          = AV_CODEC_ID_MPEG2VIDEO,

    .priv_data_size    = sizeof(CodedBitstreamMPEG2Context),

    .split_fragment    = &cbs_mpeg2_split_fragment,
    .read_unit         = &cbs_mpeg2_read_unit,
    .write_unit        = &cbs_mpeg2_write_unit,
    .assemble_fragment = &cbs_mpeg2_assemble_fragment,

    .init              = &cbs_mpeg2_init,
    .close             = &cbs_mpeg2_close,
};
