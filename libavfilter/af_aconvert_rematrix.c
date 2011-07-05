#if defined(FLOATING)
# define DIV2 /2
#else
# define DIV2 >>1
#endif

#define REMATRIX_FUNC(FUNC) static void REMATRIX(FUNC) (SFMT_t *out, SFMT_t *in,  \
                           int nb_samples, int in_channels)

REMATRIX_FUNC(stereo_to_mono)
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
    while (nb_samples > 0) {
        out[0] = (in[0] + in[1]) DIV2;
        out++;
        in += 2;
        nb_samples--;
    }
}


REMATRIX_FUNC(mono_to_stereo)
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
    while (nb_samples > 0) {
        out[0] = out[1] = in[0];
        out += 2;
        in += 1;
        nb_samples--;
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to
 * stereo and do not have a conversion formula available.  We just use first
 * two input channels - left and right. This is a placeholder until more
 * conversion functions are written.
 */
REMATRIX_FUNC(stereo_downmix)
{
    while (nb_samples--) {
        out[0] = in[0];
        out[1] = in[1];
        in  += in_channels;
        out += 2;
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to mono
 * and do not have a conversion formula available.  We just use first two input
 * channels - left and right. This is a placeholder until more conversion
 * functions are written.
 */
REMATRIX_FUNC(mono_downmix)
{
    while (nb_samples--) {
        out[0] = (in[0] + in[1]) DIV2;
        in += in_channels;
        out++;
    }
}

/* Stereo to 5.1 output */
REMATRIX_FUNC(ac3_5p1_mux)
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

#undef DIV2
#undef REMATRIX
#undef REMATRIX_FUNC
#undef SFMT_t
