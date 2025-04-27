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

#include "config.h"
#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/x86/asm.h"
#include "libavutil/x86/cpu.h"
#include "libavcodec/apv_dsp.h"

#if ARCH_X86_64

void ff_apv_decode_transquant_avx2(void *output,
                                   ptrdiff_t pitch,
                                   const int16_t *input,
                                   const int16_t *qmatrix,
                                   int bit_depth,
                                   int qp_shift);

void ff_apv_scale_qmatrix_sse2(uint16_t *output, const uint8_t *input, int scale);
void ff_apv_scale_qmatrix_ssse3(uint16_t *output, const uint8_t *input, int scale);

av_cold void ff_apv_dsp_init_x86_64(APVDSPContext *dsp)
{
    int cpu_flags = av_get_cpu_flags();

    if (EXTERNAL_SSE2_FAST(cpu_flags)) {
        dsp->scale_qmatrix = ff_apv_scale_qmatrix_sse2;
    }

    if (EXTERNAL_SSSE3(cpu_flags)) {
        dsp->scale_qmatrix = ff_apv_scale_qmatrix_ssse3;
    }

    if (EXTERNAL_AVX2_FAST(cpu_flags)) {
        dsp->decode_transquant = ff_apv_decode_transquant_avx2;
    }
}

#endif /* ARCH_X86_64 */
