/*
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram <smeenaks@ucsd.edu>
 * Copyright (c) 2011 Stefano Sabatini
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
 * sample format and channel layout conversion audio filter
 * based on code in libavcodec/resample.c by Fabrice Bellard and
 * libavcodec/audioconvert.c by Michael Niedermayer
 */

#include "avfilter.h"
#include "libavcodec/audioconvert.h"

typedef struct {
    enum AVSampleFormat out_sample_fmt;     ///< output sample format
    int64_t out_chlayout;                   ///< output channel layout

    int  out_strides[8],
         in_strides [8],
         s16_strides[8];

    AVFilterBufferRef *s16_samplesref;     ///< s16 buffer for rematrixing 
    AVFilterBufferRef *mix_samplesref;     ///< s16 buffer after rematrixing
    AVFilterBufferRef *out_samplesref;     ///< output buffer after required conversions

    AVAudioConvert *convert_to_s16_ctx;    ///< context for conversion to s16
    AVAudioConvert *convert_to_out_ctx;    ///< context for conversion to output sample format

    void (*convert_chlayout) (uint8_t *out[], uint8_t *in[], int , int);
} AConvertContext;

static void stereo_to_mono(uint8_t *out[], uint8_t *in[],
                           int nb_samples, int in_channels)
{
    int16_t *input  = (int16_t *) in[0];
    int16_t *output = (int16_t *) out[0];

    while (nb_samples >= 4) {
        output[0] = (input[0] + input[1]) >> 1;
        output[1] = (input[2] + input[3]) >> 1;
        output[2] = (input[4] + input[5]) >> 1;
        output[3] = (input[6] + input[7]) >> 1;
        output += 4;
        input += 8;
        nb_samples -= 4;
    }
    while (nb_samples > 0) {
        output[0] = (input[0] + input[1]) >> 1;
        output++;
        input += 2;
        nb_samples--;
    }
}

static void mono_to_stereo(uint8_t *out[], uint8_t *in[],
                           int nb_samples, int in_channels)
{
    int v;
    int16_t *input  = (int16_t *) in[0];
    int16_t *output = (int16_t *) out[0];

    while (nb_samples >= 4) {
        v = input[0]; output[0] = v; output[1] = v;
        v = input[1]; output[2] = v; output[3] = v;
        v = input[2]; output[4] = v; output[5] = v;
        v = input[3]; output[6] = v; output[7] = v;
        output += 8;
        input += 4;
        nb_samples -= 4;
    }
    while (nb_samples > 0) {
        v = input[0]; output[0] = v; output[1] = v;
        output += 2;
        input += 1;
        nb_samples--;
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to
 * stereo and do not have a conversion formula available.  We just use first
 * two input channels - left and right. This is a placeholder until more
 * conversion functions are written.
 */
static void stereo_downmix(uint8_t *out[], uint8_t *in[],
                           int nb_samples, int in_channels)
{
    int i;
    int16_t *output = (int16_t *)out[0];
    int16_t *input  = (int16_t *)out[0];

    for (i = 0; i < nb_samples; i++) {
        *output++ = *input++;
        *output++ = *input++;
        input += in_channels-2;
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to mono
 * and do not have a conversion formula available.  We just use first two input
 * channels - left and right. This is a placeholder until more conversion
 * functions are written.
 */
static void mono_downmix(uint8_t *out[], uint8_t *in[],
                         int nb_samples, int in_channels)
{
    int i;
    int16_t *input  = (int16_t *) in[0];
    int16_t *output = (int16_t *) out[0];
    int left, right;

    for (i = 0; i < nb_samples; i++) {
        left = *input++;
        right = *input++;
        *output++ = (left+right)>>1;
        input += in_channels-2;
    }
}

/* Stereo to 5.1 output */
static void ac3_5p1_mux(uint8_t *out[], uint8_t *in[],
                        int nb_samples, int in_channels)
{
    int i;
    int16_t *output = (int16_t *) out[0];
    int16_t *input = (int16_t *) in[0];
    int left, right;

    for (i = 0; i < nb_samples; i++) {
      left  = *input++;                 /* Grab next left sample */
      right = *input++;                 /* Grab next right sample */
      *output++ = left;                 /* left */
      *output++ = right;                /* right */
      *output++ = (left+right)>>1;      /* center */
      *output++ = 0;                    /* low freq */
      *output++ = 0;                    /* FIXME: left surround is either -3dB, -6dB or -9dB of stereo left */
      *output++ = 0;                    /* FIXME: right surroud is either -3dB, -6dB or -9dB of stereo right */
    }
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AConvertContext *aconvert = ctx->priv;
    char sample_fmt_str[8] = "", ch_layout_str[32] = "";

    if (args)
        sscanf(args, "%8[^:]:%32s", sample_fmt_str, ch_layout_str);

    aconvert->out_sample_fmt =
        *sample_fmt_str ? av_get_sample_fmt(sample_fmt_str) : AV_SAMPLE_FMT_NONE;

    if (*sample_fmt_str && aconvert->out_sample_fmt == AV_SAMPLE_FMT_NONE) {
        /* -1 is a valid value for out_sample_fmt and indicates no change
         * in sample format. */
        char *tail;
        aconvert->out_sample_fmt = strtol(sample_fmt_str, &tail, 10);
        if (*tail || (aconvert->out_sample_fmt >= AV_SAMPLE_FMT_NB &&
                      aconvert->out_sample_fmt != -1)) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sample format '%s'\n",
                   sample_fmt_str);
            return AVERROR(EINVAL);
        }
    }

    aconvert->out_chlayout = *ch_layout_str ?
                                  av_get_channel_layout(ch_layout_str) : -1;

    if (*ch_layout_str && aconvert->out_chlayout < AV_CH_LAYOUT_STEREO) {
        /* -1 is a valid value for out_chlayout and indicates no change
         * in channel layout. */
        char *tail;
        aconvert->out_chlayout = strtol(ch_layout_str, &tail, 10);
        if (*tail || (aconvert->out_chlayout < AV_CH_LAYOUT_STEREO &&
                      aconvert->out_chlayout != -1)) {
            av_log(ctx, AV_LOG_ERROR, "Invalid channel layout %s\n",
                   ch_layout_str);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AConvertContext *aconvert = ctx->priv;
    avfilter_unref_buffer(aconvert->s16_samplesref);
    avfilter_unref_buffer(aconvert->mix_samplesref);
    avfilter_unref_buffer(aconvert->out_samplesref);
    if (aconvert->convert_to_s16_ctx)
        av_audio_convert_free(aconvert->convert_to_s16_ctx);
    if (aconvert->convert_to_out_ctx)
        av_audio_convert_free(aconvert->convert_to_out_ctx);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AConvertContext *aconvert = ctx->priv;

    ctx->inputs[0]->out_packing = AVFILTER_PACKED_OR_PLANAR;
    ctx->outputs[0]->in_packing = AVFILTER_PACKED_OR_PLANAR;

    avfilter_formats_ref(avfilter_all_formats(AVMEDIA_TYPE_AUDIO),
                         &ctx->inputs[0]->out_formats);
    if (aconvert->out_sample_fmt != AV_SAMPLE_FMT_NONE) {
        avfilter_add_format(&formats, aconvert->out_sample_fmt);
        avfilter_formats_ref(formats, &ctx->outputs[0]->in_formats);
    } else
        avfilter_formats_ref(avfilter_all_formats(AVMEDIA_TYPE_AUDIO),
                             &ctx->outputs[0]->in_formats);

    avfilter_formats_ref(avfilter_all_channel_layouts(),
                         &ctx->inputs[0]->out_chlayouts);
    if (aconvert->out_chlayout != -1) {
        formats = NULL;
        avfilter_add_format(&formats, aconvert->out_chlayout);
        avfilter_formats_ref(formats, &ctx->outputs[0]->in_chlayouts);
    } else
        avfilter_formats_ref(avfilter_all_channel_layouts(),
                             &ctx->outputs[0]->in_chlayouts);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    AConvertContext *aconvert = outlink->src->priv;
    char buf1[32], buf2[32];

    /* if not specified in args, then use the format and layout
     * of the output */
    if (aconvert->out_sample_fmt == AV_SAMPLE_FMT_NONE)
        aconvert->out_sample_fmt = outlink->format;
    if (aconvert->out_chlayout == -1)
        aconvert->out_chlayout = outlink->channel_layout;

    av_get_channel_layout_string(buf1, sizeof(buf1),
                                 -1, inlink ->channel_layout);
    av_get_channel_layout_string(buf2, sizeof(buf2),
                                 -1, outlink->channel_layout);
    av_log(outlink->src, AV_LOG_INFO, "fmt:%s cl:%s -> fmt:%s cl:%s\n",
           av_get_sample_fmt_name(inlink ->format), buf1,
           av_get_sample_fmt_name(outlink->format), buf2);

    if      (inlink->channel_layout == AV_CH_LAYOUT_STEREO &&
             aconvert->out_chlayout == AV_CH_LAYOUT_MONO)
        aconvert->convert_chlayout  =  stereo_to_mono;
    else if (inlink->channel_layout == AV_CH_LAYOUT_STEREO &&
             aconvert->out_chlayout == AV_CH_LAYOUT_5POINT1)
        aconvert->convert_chlayout  =  ac3_5p1_mux;
    else if (inlink->channel_layout == AV_CH_LAYOUT_MONO &&
             aconvert->out_chlayout == AV_CH_LAYOUT_STEREO)
        aconvert->convert_chlayout  =  mono_to_stereo;
    else if (aconvert->out_chlayout == AV_CH_LAYOUT_MONO)
        aconvert->convert_chlayout  =  mono_downmix;
    else if (aconvert->out_chlayout == AV_CH_LAYOUT_STEREO)
        aconvert->convert_chlayout  =  stereo_downmix;
    else {
        av_log(outlink->src, AV_LOG_ERROR,
                "Unsupported channel layout conversion requested!\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static void init_buffers(AVFilterLink *inlink, int nb_samples)
{
    AConvertContext *aconvert = inlink->dst->priv;
    int i, chans;

    // Free All the things
    uninit(inlink->dst);

    // The input buffer
    chans = inlink->planar ?
            av_get_channel_layout_nb_channels(inlink->channel_layout) : 1;
    aconvert->in_strides[0] = av_get_bytes_per_sample(inlink->format);
    for (i = 1; i < chans; i++)
        aconvert->in_strides[i] = aconvert->in_strides[0];

    if (inlink->channel_layout != aconvert->out_chlayout) {
        for (i = 0; i < chans; i++)
            aconvert->s16_strides[i] = 2;

        // S16 buffer for later rematrixing. FIXME?
        if (inlink->format != AV_SAMPLE_FMT_S16) {
            aconvert->convert_to_s16_ctx =
                    av_audio_convert_alloc(AV_SAMPLE_FMT_S16, chans,
                                           inlink->format, chans, NULL, 0);
            aconvert->s16_samplesref = avfilter_get_audio_buffer(inlink,
                                           AV_PERM_WRITE|AV_PERM_REUSE2,
                                           AV_SAMPLE_FMT_S16,
                                           nb_samples,
                                           inlink->channel_layout,
                                           inlink->planar);
        }

        // The rematrixed buffer
        aconvert->mix_samplesref = avfilter_get_audio_buffer(inlink,
                                           AV_PERM_WRITE|AV_PERM_REUSE2,
                                           AV_SAMPLE_FMT_S16,
                                           nb_samples,
                                           aconvert->out_chlayout,
                                           inlink->dst->outputs[0]->planar);
    }

    // The output buffer
    chans = inlink->dst->outputs[0]->planar ?
            av_get_channel_layout_nb_channels(aconvert->out_chlayout) : 1;
    aconvert->convert_to_out_ctx =
            av_audio_convert_alloc(aconvert->out_sample_fmt, chans,
                                   aconvert->s16_samplesref ?
                                       AV_SAMPLE_FMT_S16 : inlink->format,
                                   chans, NULL, 0);

    aconvert->out_samplesref = avfilter_get_audio_buffer(inlink,
                                   AV_PERM_WRITE|AV_PERM_REUSE2,
                                   aconvert->out_sample_fmt,
                                   nb_samples, aconvert->out_chlayout,
                                   inlink->dst->outputs[0]->planar);

    aconvert->out_strides[0] = av_get_bytes_per_sample(aconvert->out_sample_fmt);
    for (i = 1; i < chans; i++)
        aconvert->out_strides[i] = aconvert->out_strides[0];
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterBufferRef *curbuf = insamplesref;
    int chans;

    // if number of samples has changed....
    if ((aconvert->out_samplesref &&
            curbuf->audio->nb_samples !=
                               aconvert->out_samplesref->audio->nb_samples) ||
    // ... or this is our first batch
       (!aconvert->out_samplesref &&
             (curbuf->audio->channel_layout != aconvert->out_chlayout ||
              curbuf->format != aconvert->out_sample_fmt)))
        init_buffers(inlink, curbuf->audio->nb_samples);

    if (aconvert->out_samplesref) {
        if (aconvert->mix_samplesref) {
            chans = av_get_channel_layout_nb_channels(
                        curbuf->audio->channel_layout);
            // We have to rematrix, convert to s16 first if needed.
            if (aconvert->s16_samplesref) {
                av_audio_convert(aconvert->convert_to_s16_ctx,
                                 (void * const *) aconvert->s16_samplesref->data,
                                 aconvert->s16_strides,
                                 (const void * const *) curbuf->data,
                                 aconvert->in_strides,
                                 curbuf->audio->nb_samples *
                                    (curbuf->audio->planar ? 1 : chans));

                curbuf = aconvert->s16_samplesref;
            }

            aconvert->convert_chlayout(aconvert->mix_samplesref->data,
                                       curbuf->data,
                                       curbuf->audio->nb_samples,
                                       chans);

            curbuf = aconvert->mix_samplesref;
        }

        chans = av_get_channel_layout_nb_channels(
                    curbuf->audio->channel_layout);

        // Convert to desired sample format
        av_audio_convert(aconvert->convert_to_out_ctx,
                         (void * const *) aconvert->out_samplesref->data,
                         aconvert->out_strides,
                         (const void * const *) curbuf->data,
                         curbuf == aconvert->mix_samplesref ?
                             aconvert->s16_strides : aconvert->in_strides,
                         curbuf->audio->nb_samples * 
                            (curbuf->audio->planar ? 1 : chans));

        curbuf = aconvert->out_samplesref;
    }

    avfilter_filter_samples(inlink->dst->outputs[0],
                            avfilter_ref_buffer(curbuf, ~0));
    avfilter_unref_buffer(insamplesref);
}

AVFilter avfilter_af_aconvert = {
    .name          = "aconvert",
    .description   = NULL_IF_CONFIG_SMALL("Convert the input audio to sample_fmt:channel_layout."),
    .priv_size     = sizeof(AConvertContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples   = filter_samples,
                                    .min_perms        = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .config_props     = config_output, },
                                  { .name = NULL}},
};
