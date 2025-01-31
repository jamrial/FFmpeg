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

#include <stddef.h>
#include <stdint.h>

#include "avassert.h"
#include "mem.h"
#include "ref_displays_info.h"

AV3DReferenceDisplaysInfo *av_ref_displays_info_alloc(unsigned int num_ref_displays,
                                                      size_t *out_size)
{
    struct CombinedStruct {
        AV3DReferenceDisplaysInfo i;
        AV3DReferenceDisplay      r;
    };
    const size_t ref_offset = offsetof(struct CombinedStruct, r);
    size_t size = ref_offset;
    AV3DReferenceDisplaysInfo *rdi;

    if (num_ref_displays > (SIZE_MAX - size) / sizeof(AV3DReferenceDisplay))
        return NULL;
    size += sizeof(AV3DReferenceDisplay) * num_ref_displays;

    rdi = av_mallocz(size);
    if (!rdi)
        return NULL;

    rdi->num_ref_displays = num_ref_displays;
    rdi->ref_size         = sizeof(AV3DReferenceDisplay);
    rdi->ref_offset       = ref_offset;

    if (out_size)
        *out_size = size;

    return rdi;
}
