/*
 * Copyright (c) 2011 Mina Nagy Zaki
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

/**
 * @file
 * interleave/deinterleave audio data
 */

#include <string.h>
#include "interleave.h"

void deinterleave(int16_t **outp, int16_t *in,
                         int nb_channels, int nb_samples)
{
    int16_t *out[8];
    memcpy(out, outp, nb_channels * sizeof(int16_t*));

    if        (nb_channels == 2) {
        while (nb_samples--) {
            *out[0]++ = *in++;
            *out[1]++ = *in++;
        }
    } else if (nb_channels == 3) {
        while (nb_samples--) {
            *out[0]++ = *in++;
            *out[1]++ = *in++;
            *out[2]++ = *in++;
        }
    } else if (nb_channels == 4) {
        while (nb_samples--) {
            *out[0]++ = *in++;
            *out[1]++ = *in++;
            *out[2]++ = *in++;
            *out[3]++ = *in++;
        }
    } else if (nb_channels == 5) {
        while (nb_samples--) {
            *out[0]++ = *in++;
            *out[1]++ = *in++;
            *out[2]++ = *in++;
            *out[3]++ = *in++;
            *out[4]++ = *in++;
        }
    } else if (nb_channels == 6) {
        while (nb_samples--) {
            *out[0]++ = *in++;
            *out[1]++ = *in++;
            *out[2]++ = *in++;
            *out[3]++ = *in++;
            *out[4]++ = *in++;
            *out[5]++ = *in++;
        }
    } else if (nb_channels == 8) {
        while (nb_samples--) {
            *out[0]++ = *in++;
            *out[1]++ = *in++;
            *out[2]++ = *in++;
            *out[3]++ = *in++;
            *out[4]++ = *in++;
            *out[5]++ = *in++;
            *out[6]++ = *in++;
            *out[7]++ = *in++;
        }
    }
}

void interleave(int16_t *out, int16_t **inp,
        int nb_channels, int nb_samples)
{
    int16_t *in[8];
    memcpy(in, inp, nb_channels * sizeof(int16_t*));

    if        (nb_channels == 2) {
        while (nb_samples--) {
            *out++ = *in[0]++;
            *out++ = *in[1]++;
        }
    } else if (nb_channels == 3) {
        while (nb_samples--) {
            *out++ = *in[0]++;
            *out++ = *in[1]++;
            *out++ = *in[2]++;
        }
    } else if (nb_channels == 4) {
        while (nb_samples--) {
            *out++ = *in[0]++;
            *out++ = *in[1]++;
            *out++ = *in[2]++;
            *out++ = *in[3]++;
        }
    } else if (nb_channels == 5) {
        while (nb_samples--) {
            *out++ = *in[0]++;
            *out++ = *in[1]++;
            *out++ = *in[2]++;
            *out++ = *in[3]++;
            *out++ = *in[4]++;
        }
    } else if (nb_channels == 6) {
        while (nb_samples--) {
            *out++ = *in[0]++;
            *out++ = *in[1]++;
            *out++ = *in[2]++;
            *out++ = *in[3]++;
            *out++ = *in[4]++;
            *out++ = *in[5]++;
        }
    } else if (nb_channels == 8) {
        while (nb_samples--) {
            *out++ = *in[0]++;
            *out++ = *in[1]++;
            *out++ = *in[2]++;
            *out++ = *in[3]++;
            *out++ = *in[4]++;
            *out++ = *in[5]++;
            *out++ = *in[6]++;
            *out++ = *in[7]++;
        }
    }

}

