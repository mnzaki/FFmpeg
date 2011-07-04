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

#include "libavcodec/audioconvert.h"
#include "libavutil/audioconvert.h"
#include "avfilter.h"
#include "internal.h"

#define SFMT_TYPE uint8_t
#define REMATRIX_FUNC_NAME(NAME) NAME ## _u8
#include "af_aconvert_rematrix.c"

#define SFMT_TYPE int16_t
#define REMATRIX_FUNC_NAME(NAME) NAME ## _s16
#include "af_aconvert_rematrix.c"

#define SFMT_TYPE int32_t
#define REMATRIX_FUNC_NAME(NAME) NAME ## _s32
#include "af_aconvert_rematrix.c"

#define FLOATING

#define SFMT_TYPE float
#define REMATRIX_FUNC_NAME(NAME) NAME ## _flt
#include "af_aconvert_rematrix.c"

#define SFMT_TYPE double
#define REMATRIX_FUNC_NAME(NAME) NAME ## _dbl
#include "af_aconvert_rematrix.c"

typedef struct {
    int max_nb_samples;                     ///< maximum number of buffered samples
    enum AVSampleFormat out_sample_fmt;     ///< output sample format
    int64_t out_chlayout;                   ///< output channel layout

    int out_strides[8], in_strides [8];     ///< input/output strides for av_audio_convert

    AVFilterBufferRef *mix_samplesref;      ///< rematrixed buffer
    AVFilterBufferRef *out_samplesref;      ///< output buffer after required conversions
    uint8_t *packed_data[8];                ///< pointers for packing conversion
    uint8_t **in_data, **out_data;          ///< input/output for av_audio_convert

    AVAudioConvert *audioconvert_ctx;       ///< context for conversion to output sample format

    void (*convert_chlayout) ();
} AConvertContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AConvertContext *aconvert = ctx->priv;
    char *arg;
    aconvert->out_sample_fmt = aconvert->out_chlayout = -1;

    /* the special argument 'copy' means no conversion */

    if ((arg = strsep(&args, ",")) && strcmp(arg, "copy")) {
        aconvert->out_sample_fmt = ff_parse_sample_format(arg, ctx);
        if (aconvert->out_sample_fmt == -1) return AVERROR(EINVAL);
    }

    if ((arg = strsep(&args, ",")) && strcmp(arg, "copy")) {
        aconvert->out_chlayout = ff_parse_channel_layout(arg, ctx);
        if (aconvert->out_chlayout == -1) return AVERROR(EINVAL);
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

    avfilter_formats_ref(avfilter_all_packing_formats(),
                        &ctx->outputs[0]->in_packing);
    avfilter_formats_ref(avfilter_all_packing_formats(),
                        &ctx->inputs[0]->out_packing);

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

#define SET_CONVERT_CHLAYOUT_SFMT(FUNC)                     \
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

#define SET_CONVERT_CHLAYOUT(OUT, FUNC)                     \
    if (aconvert->out_chlayout == OUT) {                    \
        if (inlink->planar)                                 \
            SET_CONVERT_CHLAYOUT_SFMT(FUNC ## _planar)      \
        else                                                \
            SET_CONVERT_CHLAYOUT_SFMT(FUNC ## _packed)      \
    }

#define SET_CONVERT_CHLAYOUT2(IN, OUT, FUNC)                \
    if (inlink->channel_layout == IN &&                     \
        aconvert->out_chlayout == OUT) {                    \
        if (inlink->planar)                                 \
            SET_CONVERT_CHLAYOUT_SFMT(FUNC ## _planar)      \
        else                                                \
            SET_CONVERT_CHLAYOUT_SFMT(FUNC ## _packed)      \
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
    av_log(outlink->src, AV_LOG_INFO, "fmt:%s cl:%s planar:%i -> fmt:%s cl:%s planar:%i\n",
           av_get_sample_fmt_name(inlink ->format), buf1, inlink->planar,
           av_get_sample_fmt_name(outlink->format), buf2, outlink->planar);

    /* handle stereo_to_mono and mono_to_stereo separately because there are
     * no planar versions */
    if (!inlink->planar                                &&
         inlink->channel_layout == AV_CH_LAYOUT_STEREO &&
         aconvert->out_chlayout == AV_CH_LAYOUT_MONO) {
       SET_CONVERT_CHLAYOUT_SFMT(stereo_to_mono_packed);
    }
    else
    if (!outlink->planar                               &&
         inlink->channel_layout == AV_CH_LAYOUT_MONO   &&
         aconvert->out_chlayout == AV_CH_LAYOUT_STEREO) {
       SET_CONVERT_CHLAYOUT_SFMT(mono_to_stereo_packed);
    }

    if (!aconvert->convert_chlayout &&
        inlink->channel_layout != outlink->channel_layout) {
             SET_CONVERT_CHLAYOUT2(AV_CH_LAYOUT_STEREO,  AV_CH_LAYOUT_5POINT1, stereo_to_surround_5p1)
        else SET_CONVERT_CHLAYOUT2(AV_CH_LAYOUT_5POINT1, AV_CH_LAYOUT_STEREO,  surround_5p1_to_stereo)
        else SET_CONVERT_CHLAYOUT(                       AV_CH_LAYOUT_MONO,    mono_downmix)
    }

    /* If there's no channel conversion function and output is stereo,
     * we can do generic stereo downmixing:
     * if there's a format conversion then stereo downmixing is implicitly
     * done by av_audio_convert.
     * if there's no format conversion then packed stereo downmixing is
     * explicitly done by av_audio_convert, while planar is done in
     * filter_samples
     */
    if (!aconvert->convert_chlayout                       &&
        outlink->channel_layout != inlink->channel_layout &&
        outlink->channel_layout != AV_CH_LAYOUT_STEREO) {
        av_log(outlink->src, AV_LOG_ERROR,
                "Unsupported channel layout conversion requested!\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int init_buffers(AVFilterLink *inlink, int nb_samples)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterLink * const outlink = inlink->dst->outputs[0];
    int i, packed_stride = 0;
    int in_channels  =
            av_get_channel_layout_nb_channels(inlink->channel_layout),
        out_channels =
            av_get_channel_layout_nb_channels(outlink->channel_layout);
    const short
        stereo_downmix = out_channels   == 2               &&
                         !aconvert->convert_chlayout,
        format_conv    = inlink->format != outlink->format,
        packing_conv   = inlink->planar != outlink->planar &&
                         in_channels    != 1               &&
                         out_channels   != 1;

    aconvert->max_nb_samples = nb_samples;
    uninit(inlink->dst);

    // rematrixing
    if (aconvert->convert_chlayout) {
        aconvert->mix_samplesref =
                avfilter_get_audio_buffer(outlink,
                                          AV_PERM_WRITE | AV_PERM_REUSE2,
                                          inlink->format,
                                          nb_samples,
                                          outlink->channel_layout,
                                          inlink->planar);
        if (!aconvert->mix_samplesref)
            return AVERROR(ENOMEM);
        in_channels = out_channels;
    }

    /* If there's any conversion left to do, we need a buffer */
    if (format_conv || packing_conv || stereo_downmix) {
        aconvert->out_samplesref = avfilter_get_audio_buffer(outlink,
                                          AV_PERM_WRITE | AV_PERM_REUSE2,
                                          outlink->format,
                                          nb_samples,
                                          outlink->channel_layout,
                                          outlink->planar);
        if (!aconvert->out_samplesref)
            return AVERROR(ENOMEM);
    }

    /* if there's a format/mode conversion or packed stereo downmixing,
     * we need an audio_convert context
     */
    if (format_conv || packing_conv || (stereo_downmix && !outlink->planar)) {
        aconvert->in_strides[0]  = av_get_bytes_per_sample(inlink->format);
        aconvert->out_strides[0] = av_get_bytes_per_sample(outlink->format);

        aconvert->out_data = aconvert->out_samplesref->data;
        if (aconvert->mix_samplesref)
            aconvert->in_data  = aconvert->mix_samplesref->data;

        if (packing_conv) {
            if (outlink->planar) {
                if (aconvert->mix_samplesref)
                    aconvert->packed_data[0] =
                        aconvert->mix_samplesref->data[0];
                aconvert->in_data         = aconvert->packed_data;
                packed_stride             = aconvert->in_strides[0];
                aconvert->in_strides[0]  *= in_channels;
            } else {
                aconvert->packed_data[0]  = aconvert->out_samplesref->data[0];
                aconvert->out_data        = aconvert->packed_data;
                packed_stride             = aconvert->out_strides[0];
                aconvert->out_strides[0] *= out_channels;
            }
        } else if (!outlink->planar || (stereo_downmix && in_channels == 1)) {
            out_channels = 1;
        }

        for (i = 1; i < out_channels; i++) {
            aconvert->packed_data[i] = aconvert->packed_data[i-1] +
                                           packed_stride;
            aconvert->in_strides[i]  = aconvert->in_strides[0];
            aconvert->out_strides[i] = aconvert->out_strides[0];
        }

        aconvert->audioconvert_ctx =
                av_audio_convert_alloc(outlink->format, out_channels,
                                       inlink->format,  out_channels, NULL, 0);
        if (!aconvert->audioconvert_ctx)
            return AVERROR(ENOMEM);
    }

    return 0;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterBufferRef *curbuf = insamplesref;
    AVFilterLink * const outlink = inlink->dst->outputs[0];
    int nb_channels = av_get_channel_layout_nb_channels(
                          curbuf->audio->channel_layout);

    if (!aconvert->max_nb_samples ||
        (curbuf->audio->nb_samples > aconvert->max_nb_samples))
        if(init_buffers(inlink, curbuf->audio->nb_samples))
            return;

    if (aconvert->mix_samplesref) {
        if (inlink->planar && nb_channels != 1)
            aconvert->convert_chlayout(aconvert->mix_samplesref->data,
                                       curbuf->data,
                                       curbuf->audio->nb_samples,
                                       nb_channels);
        else
            aconvert->convert_chlayout(aconvert->mix_samplesref->data[0],
                                       curbuf->data[0],
                                       curbuf->audio->nb_samples,
                                       nb_channels);

        curbuf = aconvert->mix_samplesref;
    }

    if (aconvert->audioconvert_ctx) {
        if (!aconvert->mix_samplesref) {
            if (aconvert->in_data == aconvert->packed_data) {
                int i, packed_stride = av_get_bytes_per_sample(inlink->format);
                aconvert->packed_data[0] = curbuf->data[0];
                for (i = 1; i < nb_channels; i++)
                    aconvert->packed_data[i] =
                                aconvert->packed_data[i-1] + packed_stride;
            } else {
                aconvert->in_data = curbuf->data;
            }
        }

        if (inlink->planar == outlink->planar && !outlink->planar)
            nb_channels = av_get_channel_layout_nb_channels(
                              curbuf->audio->channel_layout);
        else
            nb_channels = 1;

        av_audio_convert(aconvert->audioconvert_ctx,
                         (void * const *) aconvert->out_data,
                         aconvert->out_strides,
                         (const void * const *) aconvert->in_data,
                         aconvert->in_strides,
                         curbuf->audio->nb_samples * nb_channels);

        curbuf = aconvert->out_samplesref;
    }

    /* Handle generic planar stereo downmixing by simply copying streams */
    if (outlink->channel_layout == AV_CH_LAYOUT_STEREO &&
        !aconvert->convert_chlayout) {
        int size =
            av_get_bytes_per_sample(curbuf->format) * curbuf->audio->nb_samples;

        if (!aconvert->audioconvert_ctx)
            memcpy(aconvert->out_samplesref->data[0], curbuf->data[0], size);

        memcpy(aconvert->out_samplesref->data[1],
               nb_channels == 1 ? curbuf->data[0] : curbuf->data[1],
               size);

        curbuf = aconvert->out_samplesref;
    }

    avfilter_copy_buffer_ref_props(curbuf, insamplesref);
    curbuf->audio->channel_layout = outlink->channel_layout;
    curbuf->audio->planar         = outlink->planar;

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

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples  = filter_samples,
                                    .min_perms       = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_AUDIO,
                                    .config_props    = config_output, },
                                  { .name = NULL}},
};
