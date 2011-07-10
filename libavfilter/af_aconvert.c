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

#define SFMT_t uint8_t
#define REMATRIX(FUNC) FUNC ## _u8
#include "af_aconvert_rematrix.c"

#define SFMT_t int16_t
#define REMATRIX(FUNC) FUNC ## _s16
#include "af_aconvert_rematrix.c"

#define SFMT_t int32_t
#define REMATRIX(FUNC) FUNC ## _s32
#include "af_aconvert_rematrix.c"

#define FLOATING

#define SFMT_t float
#define REMATRIX(FUNC) FUNC ## _flt
#include "af_aconvert_rematrix.c"

#define SFMT_t double
#define REMATRIX(FUNC) FUNC ## _dbl
#include "af_aconvert_rematrix.c"

typedef struct {
    int nb_samples;                         ///< current size of buffers
    enum AVSampleFormat out_sample_fmt;     ///< output sample format
    int64_t out_chlayout;                   ///< output channel layout

    int  out_strides[8],
         in_strides [8];

    AVFilterBufferRef *mix_samplesref;     ///< rematrixed buffer
    AVFilterBufferRef *out_samplesref;     ///< output buffer after required conversions

    AVAudioConvert *audioconvert_ctx;      ///< context for conversion to output sample format

    void (*convert_chlayout) (void*, void*, int , int);
} AConvertContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AConvertContext *aconvert = ctx->priv;
    char sample_fmt_str[8] = "", chlayout_str[32] = "";

    if (args)
        sscanf(args, "%8[^:]:%32s", sample_fmt_str, chlayout_str);

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

    aconvert->out_chlayout = *chlayout_str ?
                                  av_get_channel_layout(chlayout_str) : -1;

    if (*chlayout_str && aconvert->out_chlayout < AV_CH_LAYOUT_STEREO) {
        /* -1 is a valid value for out_chlayout and indicates no change
         * in channel layout. */
        char *tail;
        aconvert->out_chlayout = strtol(chlayout_str, &tail, 10);
        if (*tail || (aconvert->out_chlayout < AV_CH_LAYOUT_STEREO &&
                      aconvert->out_chlayout != -1)) {
            av_log(ctx, AV_LOG_ERROR, "Invalid channel layout %s\n",
                   chlayout_str);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AConvertContext *aconvert = ctx->priv;
    avfilter_unref_buffer(aconvert->mix_samplesref);
    avfilter_unref_buffer(aconvert->out_samplesref);
    if (aconvert->audioconvert_ctx)
        av_audio_convert_free(aconvert->audioconvert_ctx);
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

#define CHOOSE_FUNC_SFMT(FUNC)                              \
    switch (inlink->format) {                               \
    case AV_SAMPLE_FMT_U8:                                  \
        aconvert->convert_chlayout = FUNC ## _u8;  break;   \
    case AV_SAMPLE_FMT_S16:                                 \
        aconvert->convert_chlayout = FUNC ## _s16; break;   \
    case AV_SAMPLE_FMT_S32:                                 \
        aconvert->convert_chlayout = FUNC ## _s32; break;   \
    case AV_SAMPLE_FMT_FLT:                                 \
        aconvert->convert_chlayout = FUNC ## _flt; break;   \
    case AV_SAMPLE_FMT_DBL:                                 \
        aconvert->convert_chlayout = FUNC ## _dbl; break;   \
    }

#define CHOOSE_FUNC(OUT, FUNC)                              \
    if (aconvert->out_chlayout == OUT) {                    \
        if (inlink->planar)                                 \
            CHOOSE_FUNC_SFMT(FUNC ## _planar)               \
        else                                                \
            CHOOSE_FUNC_SFMT(FUNC ## _packed)               \
    }

#define CHOOSE_FUNC2(IN, OUT, FUNC)                         \
    if (inlink->channel_layout == IN &&                     \
        aconvert->out_chlayout == OUT) {                    \
        if (inlink->planar)                                 \
            CHOOSE_FUNC_SFMT(FUNC ## _planar)               \
        else                                                \
            CHOOSE_FUNC_SFMT(FUNC ## _packed)               \
    }

static int config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    AConvertContext *aconvert = outlink->src->priv;
    char buf1[32], buf2[32];

    /* if not specified in args, use the format and layout of the output */
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

    /* handle stereo_to_mono separately because there is no planar version
     * as it is handled by mono_downmix_planar */
    if (!inlink->planar &&
         inlink->channel_layout == AV_CH_LAYOUT_STEREO &&
         aconvert->out_chlayout == AV_CH_LAYOUT_MONO)
        CHOOSE_FUNC_SFMT(stereo_to_mono_packed);

         CHOOSE_FUNC2(AV_CH_LAYOUT_MONO,   AV_CH_LAYOUT_STEREO,  mono_to_stereo)
    else CHOOSE_FUNC2(AV_CH_LAYOUT_STEREO, AV_CH_LAYOUT_5POINT1, ac3_5p1_mux)
    else CHOOSE_FUNC(                      AV_CH_LAYOUT_STEREO,  stereo_downmix)
    else CHOOSE_FUNC(                      AV_CH_LAYOUT_MONO,    mono_downmix)

    if (!aconvert->convert_chlayout) {
        av_log(outlink->src, AV_LOG_ERROR,
                "Unsupported channel layout conversion requested!\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static void init_buffers(AVFilterLink *inlink, int nb_samples)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterLink * const outlink = inlink->dst->outputs[0];

    uninit(inlink->dst);
    
    aconvert->nb_samples = nb_samples;

    if (inlink->channel_layout != outlink->channel_layout) {
        aconvert->out_samplesref = aconvert->mix_samplesref =
                avfilter_get_audio_buffer(inlink,
                                          AV_PERM_WRITE | AV_PERM_REUSE2,
                                          inlink->format,
                                          nb_samples,
                                          outlink->channel_layout,
                                          oulink->planar);
    }

    if (inlink->format != outlink->format || inlink->planar != outlink->planar) {
        int i, nb_channels;
        i = nb_channels = 
            av_get_channel_layout_nb_channels(outlink->channel_layout);

        aconvert->in_strides[0]  = av_get_bytes_per_sample(inlink->format);
        aconvert->out_strides[0] = av_get_bytes_per_sample(oulink->format);

        if (inlink->planar != outlink->planar) {
            if (outlink->planar)
                aconvert->in_strides[0]  *= nb_channels;
            else
                aconvert->out_strides[0] *= nb_channels;
        } else if (!outlink->planar) {
            nb_channels = 1;
        }

        while (i--) {
            aconvert->in_strides[i]  = aconvert->in_strides[0];
            aconvert->out_strides[i] = aconvert->out_strides[0];
        }
        
        aconvert->audioconvert_ctx =
                av_audio_convert_alloc(outlink->format, nb_channels,
                                       inlink->format,  nb_channels, NULL, 0);
    }
    
    if (aconvert->audioconvert_ctx)
        aconvert->out_samplesref = avfilter_get_audio_buffer(inlink,
                                          AV_PERM_WRITE | AV_PERM_REUSE2,
                                          outlink->format,
                                          nb_samples,
                                          outlink->channel_layout,
                                          outlink->planar);

}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterBufferRef *curbuf = insamplesref;
    int nb_channels;

    if (!aconvert->nb_samples ||
        (curbuf->audio->nb_samples > aconvert->nb_samples))
        init_buffers(inlink, curbuf->audio->nb_samples);
    
    if (aconvert->out_samplesref) {
        if (aconvert->mix_samplesref) {
            if (inlink->planar)
                aconvert->convert_chlayout(aconvert->mix_samplesref->data,
                                           curbuf->data,
                                           curbuf->nb_samples);
            else
                aconvert->convert_chlayout(aconvert->mix_samplesref->data[0],
                                           curbuf->data[0],
                                           curbuf->nb_samples);

            curbuf = aconvert->mix_samplesref;
        }

        if (inlink->planar == outlink->planar == 0)
            nb_channels = av_get_channel_layout_nb_channels(
                              curbuf->audio->channel_layout);
        else
            nb_channels = 1;

        av_audio_convert(aconvert->audioconvert_ctx,
                         (void * const *) aconvert->out_samplesref->data,
                         aconvert->out_strides,
                         (const void * const *) curbuf->data,
                         aconvert->in_strides,
                         nb_samples * nb_channels);

        curbuf = aconvert->out_samplesref;
    }

    curbuf->nb_samples = nb_samples;

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
