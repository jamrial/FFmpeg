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

#include "config_components.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#include "refstruct.h"
#include "postprocess.h"
#include "postprocess_internal.h"

typedef struct FFPostProcInternal {
    FFPostProc pub;
    AVMutex mutex;
} FFPostProcInternal;

static const struct FFPostProcType *const post_process_table[] = {
#if CONFIG_LIBLCEVC_DEC
    &ff_lcevc_postproc,
#endif
};

static av_always_inline FFPostProcInternal *ffpostproci(FFPostProc *pp)
{
    return (FFPostProcInternal*)pp;
}

static void postproc_free(FFRefStructOpaque opaque, void *obj)
{
    FFPostProc *pp = obj;
    FFPostProcInternal *ppi = ffpostproci(pp);

    if (!pp->type)
        return;

    ff_mutex_destroy(&ppi->mutex);

    if (pp->type->close)
        pp->type->close(pp);

    av_freep(&pp->priv_data);
}

FFPostProc *ff_postproc_alloc(void)
{
    FFPostProcInternal *ppi = ff_refstruct_alloc_ext(sizeof(FFPostProcInternal), 0, NULL, postproc_free);

    if (!ppi)
        return NULL;

    if (ff_mutex_init(&ppi->mutex, NULL)) {
        memset(&ppi->mutex, 0, sizeof(ppi->mutex));
        ff_refstruct_unref(&ppi);
        return NULL;
    }

    return &ppi->pub;
}

int ff_postproc_init(FFPostProc *pp, void *opaque, enum FFPostProcEnum type)
{
    FFPostProcInternal *ppi = ffpostproci(pp);
    int i, ret;

    if (type <= FF_POSTPROC_TYPE_CUSTOM)
        return AVERROR(EINVAL);

    ff_mutex_lock(&ppi->mutex);
    if (pp->type) {
        av_assert1(pp->type->type == type);
        ff_mutex_unlock(&ppi->mutex);
        return 0;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(post_process_table); i++) {
        if (post_process_table[i]->type == type)
            break;
    }

    if (i == FF_ARRAY_ELEMS(post_process_table)) {
        ret = AVERROR(ENOSYS);
        goto fail;
    }

    pp->type = post_process_table[i];

    if (pp->type->priv_data_size) {
        pp->priv_data = av_mallocz(pp->type->priv_data_size);
        if (!pp->priv_data) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (pp->type->init) {
        ret = pp->type->init(pp, opaque);
        if (ret < 0)
            goto fail;
    }

    if (pp->type->get_buffer)
        pp->caps |= FF_POSTPROC_CAP_GET_BUFFER;

    ret = 0;
fail:
    if (ret < 0) {
        av_freep(&pp->priv_data);
        pp->type = NULL;
    }
    ff_mutex_unlock(&ppi->mutex);
    return ret;
}

static void postproc_close_custom(FFPostProc *pp)
{
    FFPostProcType *type = (FFPostProcType *)pp->type;

    av_free(type);
}

int ff_postproc_init_custom(FFPostProc *pp,
                            int (*process)(FFPostProc *pp, void *opaque, void *obj))
{
    FFPostProcInternal *ppi = ffpostproci(pp);
    FFPostProcType *type;
    int ret;

    ff_mutex_lock(&ppi->mutex);

    if (pp->type) {
        ret = AVERROR_BUG;
        goto fail;
    }

    type = av_mallocz(sizeof(*type));
    if (!type) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    type->process = process;
    type->close   = postproc_close_custom;
    type->type    = FF_POSTPROC_TYPE_CUSTOM;

    pp->type = type;

    ret = 0;
fail:
    ff_mutex_unlock(&ppi->mutex);
    return ret;
}

int ff_postproc_is_open(FFPostProc *pp)
{
    FFPostProcInternal *ppi = ffpostproci(pp);
    int ret;

    ff_mutex_lock(&ppi->mutex);
    ret = !!pp->type;
    ff_mutex_unlock(&ppi->mutex);

    return ret;
}

int ff_postproc_process(FFPostProc *pp, void *opaque, void *obj)
{
    return pp->type->process(pp, opaque, obj);
}

int ff_postproc_get_buffer(FFPostProc *pp, void *opaque, void *obj, int flags)
{
    return pp->type->get_buffer(pp, opaque, obj, flags);
}
