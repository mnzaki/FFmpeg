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
 * audio rematrixing functions
 */

#if defined(FLOATING)
# define DIV2 /2
#else
# define DIV2 >>1
#endif

#define REMATRIX_FUNC_PACKED(FUNC) static void REMATRIX(FUNC) \
    (SFMT_t *out,   SFMT_t *in, int nb_samples)
#define REMATRIX_FUNC_PLANAR(FUNC) static void REMATRIX(FUNC) \
    (SFMT_t *outp[], SFMT_t *inp[], int nb_samples)

REMATRIX_FUNC_PACKED(stereo_to_mono_packed)
{
    while (nb_samples >= 4) {
        out[0] = (in[0] + in[1]) DIV2;
        out[1] = (in[2] + in[3]) DIV2;
        out[2] = (in[4] + in[5]) DIV2;
        out[3] = (in[6] + in[7]) DIV2;
        out += 4;
        in += 8;
        nb_samples -= 4;
    }
    while (nb_samples--) {
        out[0] = (in[0] + in[1]) DIV2;
        out++;
        in += 2;
    }
}

REMATRIX_FUNC_PACKED(mono_to_stereo_packed)
{
    while (nb_samples >= 4) {
        out[0] = out[1] = in[0];
        out[2] = out[3] = in[1];
        out[4] = out[5] = in[2];
        out[6] = out[7] = in[3];
        out += 8;
        in += 4;
        nb_samples -= 4;
    }
    while (nb_samples--) {
        out[0] = out[1] = in[0];
        out += 2;
        in += 1;
    }
}

REMATRIX_FUNC_PLANAR(mono_to_stereo_planar)
{
    SFMT_t *in = inp[0], *out[2];
    out[0] = outp[0];
    out[1] = outp[1];

    while (nb_samples >= 4) {
        out[0][0] = out[1][0] = in[0];
        out[0][1] = out[1][1] = in[1];
        out[0][2] = out[1][2] = in[2];
        out[0][3] = out[1][3] = in[3];
        out[0] += 4;
        out[1] += 4;
        in += 4;
        nb_samples -= 4;
    }
    while (nb_samples--)
        *out[0]++ = *out[1]++ = *in++;
}

/**
 * This is for when we have more than 2 input channels, need to downmix to
 * stereo and do not have a conversion formula available.  We just use first
 * two input channels - left and right. This is a placeholder until more
 * conversion functions are written.
 */
REMATRIX_FUNC_PACKED(stereo_downmix_packed)
{
    while (nb_samples--) {
        out[0] = in[0];
        out[1] = in[1];
        in  += in_channels;
        out += 2;
    }
}

REMATRIX_FUNC_PLANAR(stereo_downmix_planar)
{
    SFMT_t *in[2], *out[2];
    in[0]  = inp[0];  in[1]  = inp[1];
    out[0] = outp[0]; out[1] = outp[1];
    while (nb_samples--) {
        out[0][nb_samples] = in[0][nb_samples];
        out[1][nb_samples] = in[1][nb_samples];
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to mono
 * and do not have a conversion formula available.  We just use first two input
 * channels - left and right. This is a placeholder until more conversion
 * functions are written.
 */
mono_downmix_packed
    (SFMT_t *out, SFMT_t *in, int nb_samples, int in_channels)
{
    while (nb_samples--) {
        out[0] = (in[0] + in[1]) DIV2;
        in += in_channels;
        out++;
    }
}

mono_downmix_planar
    (SFMT_t *outp[], SFMT_t *inp[], int nb_samples, int in_channels)
{
    SFMT_t *in[2], *out = outp[0];
    in[0] = inp[0];
    in[1] = inp[1];

    while (nb_samples >= 4) {
        out[0] = (in[0][0] + in[1][0]) DIV2;
        out[1] = (in[0][1] + in[1][1]) DIV2;
        out[2] = (in[0][2] + in[1][2]) DIV2;
        out[3] = (in[0][3] + in[1][3]) DIV2;
        out += 4;
        in[0] += 4;
        in[1] += 4;
        nb_samples -= 4;
    }
    while (nb_samples--) {
        out[0] = (in[0][0] + in[1][0]) DIV2;
        out++;
        in[0]++;
        in[1]++;
    }

}

/* Stereo to 5.1 output */
REMATRIX_FUNC_PACKED(ac3_5p1_mux_packed)
{
    while (nb_samples--) {
      out[0] = in[0];                /* left */
      out[1] = in[1];                /* right */
      out[2] = (in[0] + in[1]) DIV2; /* center */
      out[3] = 0;                    /* low freq */
      out[4] = 0;                    /* FIXME: left surround: -3dB or -6dB or -9dB of stereo left  */
      out[5] = 0;                    /* FIXME: right surroud: -3dB or -6dB or -9dB of stereo right */
      in  += 2;
      out += 6;
    }
}

REMATRIX_FUNC_PLANAR(ac3_5p1_mux_planar)
{
    SFMT_t *in[2], *out[6];
    in[0]  = inp[0];  in[1]  = inp[1];
    out[0] = outp[0]; out[1] = outp[1];
    out[2] = outp[2]; out[3] = outp[3];
    out[4] = outp[4]; out[5] = outp[5];

    while (nb_samples--) {
      *out[0]++ = *in[0];               /* left */
      *out[1]++ = *in[1];               /* right */
      *out[2]++ = (*in[0]++ + *in[1]++) DIV2; /* center */
      *out[3]++ = 0;                    /* low freq */
      *out[4]++ = 0;                    /* FIXME: left surround: -3dB or -6dB or -9dB of stereo left  */
      *out[5]++ = 0;                    /* FIXME: right surroud: -3dB or -6dB or -9dB of stereo right */
    }
}

#undef DIV2
#undef REMATRIX
#undef REMATRIX_FUNC
#undef SFMT_t
