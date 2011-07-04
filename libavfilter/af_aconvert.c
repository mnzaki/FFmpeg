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

typedef struct {
    int max_nb_samples;                     ///< maximum number of buffered samples

    enum AVSampleFormat out_sample_fmt;     ///< output sample format
    int64_t out_chlayout;                   ///< output channel layout
    int out_channels;                       ///< number of output channels

    enum AVSampleFormat in_sample_fmt;      ///< input sample format
    int in_channels;                        ///< number of input channels

    AVFilterBufferRef *mix_samplesref;      ///< rematrixed buffer
    AVFilterBufferRef *out_samplesref;      ///< output buffer after required conversions

    uint8_t *in_mix[8], *out_mix[8];        ///< input/output for rematrixing functions
    uint8_t *packed_data[8];                ///< pointers for packing conversion
    int out_strides[8], in_strides[8];      ///< input/output strides for av_audio_convert
    uint8_t **in_conv, **out_conv;          ///< input/output for av_audio_convert

    AVAudioConvert *audioconvert_ctx;       ///< context for conversion to output sample format

    void (*convert_chlayout)();             ///< function to do the requested rematrixing
} AConvertContext;

#define REMATRIX_FUNC_SIG(NAME) static void REMATRIX_FUNC_NAME(NAME) \
    (SFMT_TYPE *outp[], SFMT_TYPE *inp[], int nb_samples, AConvertContext *aconvert)

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

#define SFMT_TYPE uint8_t
#define REMATRIX_FUNC_NAME(NAME) NAME
REMATRIX_FUNC_SIG(stereo_downmix_planar)
{
    int size = av_get_bytes_per_sample(aconvert->in_sample_fmt) * nb_samples;

    memcpy(outp[0], inp[0], size);
    memcpy(outp[1], inp[aconvert->in_channels == 1 ? 0 : 1], size);
}

#define REGISTER_FUNC_PACKING(INCHLAYOUT, OUTCHLAYOUT, FUNC, PACKING)   \
    {INCHLAYOUT, OUTCHLAYOUT, PACKING, AV_SAMPLE_FMT_U8,  FUNC##_u8},   \
    {INCHLAYOUT, OUTCHLAYOUT, PACKING, AV_SAMPLE_FMT_S16, FUNC##_s16},  \
    {INCHLAYOUT, OUTCHLAYOUT, PACKING, AV_SAMPLE_FMT_S32, FUNC##_s32},  \
    {INCHLAYOUT, OUTCHLAYOUT, PACKING, AV_SAMPLE_FMT_FLT, FUNC##_flt},  \
    {INCHLAYOUT, OUTCHLAYOUT, PACKING, AV_SAMPLE_FMT_DBL, FUNC##_dbl},

#define REGISTER_FUNC(INCHLAYOUT, OUTCHLAYOUT, FUNC)                                \
    REGISTER_FUNC_PACKING(INCHLAYOUT, OUTCHLAYOUT, FUNC##_packed, AVFILTER_PACKED)  \
    REGISTER_FUNC_PACKING(INCHLAYOUT, OUTCHLAYOUT, FUNC##_planar, AVFILTER_PLANAR)

static struct RematrixFunctionInfo {
    int64_t in_chlayout, out_chlayout;
    int planar, sfmt;
    void (*func)();
} rematrix_funcs[] = {
    REGISTER_FUNC        (AV_CH_LAYOUT_STEREO,  AV_CH_LAYOUT_5POINT1, stereo_to_surround_5p1)
    REGISTER_FUNC        (AV_CH_LAYOUT_5POINT1, AV_CH_LAYOUT_STEREO,  surround_5p1_to_stereo)
    REGISTER_FUNC_PACKING(AV_CH_LAYOUT_STEREO,  AV_CH_LAYOUT_MONO,    stereo_to_mono_packed, AVFILTER_PACKED)
    REGISTER_FUNC_PACKING(AV_CH_LAYOUT_MONO,    AV_CH_LAYOUT_STEREO,  mono_to_stereo_packed, AVFILTER_PACKED)
    REGISTER_FUNC        (0,                    AV_CH_LAYOUT_MONO,    mono_downmix)
    REGISTER_FUNC_PACKING(0,                    AV_CH_LAYOUT_STEREO,  stereo_downmix_packed, AVFILTER_PACKED)

    // This function works for all sample formats
    {0, AV_CH_LAYOUT_STEREO, AVFILTER_PLANAR, -1, stereo_downmix_planar}
};

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AConvertContext *aconvert = ctx->priv;
    char *arg;
    int ret;
    aconvert->out_sample_fmt = AV_SAMPLE_FMT_NONE;
    aconvert->out_chlayout   = 0;

    if ((arg = strtok(args, ":")) && strcmp(arg, "auto")) {
        if ((ret = ff_parse_sample_format(&aconvert->out_sample_fmt, arg, ctx)) < 0)
            return ret;
    }

    if ((arg = strtok(NULL, ":")) && strcmp(arg, "auto")) {
        if ((ret = ff_parse_channel_layout(&aconvert->out_chlayout, arg, ctx)) < 0)
            return ret;
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
                        &ctx->inputs[0] ->out_packing);

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
    if (aconvert->out_chlayout != 0) {
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

    /* if not specified in args, use the format and layout of the output */
    if (aconvert->out_sample_fmt == AV_SAMPLE_FMT_NONE)
        aconvert->out_sample_fmt = outlink->format;
    if (aconvert->out_chlayout   == 0)
        aconvert->out_chlayout   = outlink->channel_layout;

    aconvert->in_sample_fmt = inlink->format;
    aconvert->in_channels   =
        av_get_channel_layout_nb_channels(inlink->channel_layout);
    aconvert->out_channels  =
        av_get_channel_layout_nb_channels(outlink->channel_layout);

    av_get_channel_layout_string(buf1, sizeof(buf1),
                                 -1, inlink ->channel_layout);
    av_get_channel_layout_string(buf2, sizeof(buf2),
                                 -1, outlink->channel_layout);
    av_log(outlink->src, AV_LOG_INFO, "fmt:%s cl:%s planar:%i -> fmt:%s cl:%s planar:%i\n",
           av_get_sample_fmt_name(inlink ->format), buf1, inlink->planar,
           av_get_sample_fmt_name(outlink->format), buf2, outlink->planar);

    if (inlink->channel_layout != outlink->channel_layout) {
        int i;
        for (i = 0; i < sizeof(rematrix_funcs); i++) {
            const struct RematrixFunctionInfo *f = &rematrix_funcs[i];
            if ((f->in_chlayout  == 0 ||
                 f->in_chlayout  == inlink->channel_layout)      &&
                (f->out_chlayout == 0 ||
                 f->out_chlayout == outlink->channel_layout)     &&
                (f->planar == -1 || f->planar == inlink->planar) &&
                (f->sfmt   == -1 || f->sfmt   == inlink->format)
               ) {
                aconvert->convert_chlayout = f->func;
                break;
            }
        }
        if (!aconvert->convert_chlayout) {
            av_log(outlink->src, AV_LOG_ERROR,
                    "Unsupported channel layout conversion requested!\n");
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int init_buffers(AVFilterLink *inlink, int nb_samples)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterLink * const outlink = inlink->dst->outputs[0];
    int i, packed_stride = 0;
    const unsigned
        packing_conv = inlink->planar != outlink->planar &&
                       aconvert->out_channels != 1,
        sformat_conv = inlink->format != outlink->format;
    int nb_channels  = aconvert->out_channels;

    uninit(inlink->dst);
    aconvert->max_nb_samples = nb_samples;

    if (aconvert->convert_chlayout) {
        aconvert->mix_samplesref =
            avfilter_get_audio_buffer(outlink,
                                      AV_PERM_WRITE | AV_PERM_REUSE2,
                                      inlink->format,
                                      nb_samples,
                                      outlink->channel_layout,
                                      inlink->planar);
        if (!aconvert->mix_samplesref)
            goto fail_no_mem;
    }

    // if there's a format/packing conversion we need an audio_convert context
    if (sformat_conv || packing_conv) {
        aconvert->out_samplesref = avfilter_get_audio_buffer(
                                       outlink,
                                       AV_PERM_WRITE | AV_PERM_REUSE2,
                                       outlink->format,
                                       nb_samples,
                                       outlink->channel_layout,
                                       outlink->planar);
        if (!aconvert->out_samplesref)
            goto fail_no_mem;

        aconvert->in_strides[0]  = av_get_bytes_per_sample(inlink->format);
        aconvert->out_strides[0] = av_get_bytes_per_sample(outlink->format);

        aconvert->out_conv = aconvert->out_samplesref->data;
        if (aconvert->mix_samplesref)
            aconvert->in_conv = aconvert->mix_samplesref->data;

        if (packing_conv) {
            // packed -> planar
            if (outlink->planar == AVFILTER_PLANAR) {
                if (aconvert->mix_samplesref)
                    aconvert->packed_data[0] =
                        aconvert->mix_samplesref->data[0];
                aconvert->in_conv         = aconvert->packed_data;
                packed_stride             = aconvert->in_strides[0];
                aconvert->in_strides[0]  *= nb_channels;
            // planar -> packed
            } else {
                aconvert->packed_data[0]  = aconvert->out_samplesref->data[0];
                aconvert->out_conv        = aconvert->packed_data;
                packed_stride             = aconvert->out_strides[0];
                aconvert->out_strides[0] *= nb_channels;
            }
        } else if (outlink->planar == AVFILTER_PACKED) {
            /* If there's no packing conversion, and the stream is packed
             * then we treat the entire stream as one big channel
             */
            nb_channels = 1;
        }

        for (i = 1; i < nb_channels; i++) {
            aconvert->packed_data[i] = aconvert->packed_data[i-1] +
                                       packed_stride;
            aconvert->in_strides[i]  = aconvert->in_strides[0];
            aconvert->out_strides[i] = aconvert->out_strides[0];
        }

        aconvert->audioconvert_ctx =
                av_audio_convert_alloc(outlink->format, nb_channels,
                                       inlink->format,  nb_channels, NULL, 0);
        if (!aconvert->audioconvert_ctx)
            goto fail_no_mem;
    }

    return 0;

fail_no_mem:
    av_log(inlink->dst, AV_LOG_ERROR, "Could not allocate memory.\n");
    return AVERROR(ENOMEM);
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    AConvertContext *aconvert = inlink->dst->priv;
    AVFilterBufferRef *curbuf = insamplesref;
    AVFilterLink * const outlink = inlink->dst->outputs[0];
    int chan_mult;

    if (!aconvert->max_nb_samples ||
        (curbuf->audio->nb_samples > aconvert->max_nb_samples))
        if (init_buffers(inlink, curbuf->audio->nb_samples) < 0) {
            av_log(inlink->dst, AV_LOG_ERROR, "Could not initialize buffers.\n");
            return;
        }

    if (aconvert->mix_samplesref) {
        memcpy(aconvert->in_mix,  curbuf->data, sizeof(aconvert->in_mix));
        memcpy(aconvert->out_mix, aconvert->mix_samplesref->data, sizeof(aconvert->out_mix));
        aconvert->convert_chlayout(aconvert->out_mix,
                                   aconvert->in_mix,
                                   curbuf->audio->nb_samples,
                                   aconvert);
        curbuf = aconvert->mix_samplesref;
    }

    if (aconvert->audioconvert_ctx) {
        if (!aconvert->mix_samplesref) {
            if (aconvert->in_conv == aconvert->packed_data) {
                int i, packed_stride = av_get_bytes_per_sample(inlink->format);
                aconvert->packed_data[0] = curbuf->data[0];
                for (i = 1; i < aconvert->out_channels; i++)
                    aconvert->packed_data[i] =
                                aconvert->packed_data[i-1] + packed_stride;
            } else {
                aconvert->in_conv = curbuf->data;
            }
        }

        if (inlink->planar == outlink->planar &&
            inlink->planar == AVFILTER_PACKED)
            chan_mult = aconvert->out_channels;
        else
            chan_mult = 1;

        av_audio_convert(aconvert->audioconvert_ctx,
                         (void * const *) aconvert->out_conv,
                         aconvert->out_strides,
                         (const void * const *) aconvert->in_conv,
                         aconvert->in_strides,
                         curbuf->audio->nb_samples * chan_mult);

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
