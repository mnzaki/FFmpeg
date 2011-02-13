/*
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
 * resampling audio filter
 */

#include "libavutil/eval.h"
#include "libavcodec/avcodec.h"
#include "avfilter.h"

typedef struct {
    struct AVResampleContext *resample;
    int out_rate;
    double ratio; ///< output conversion ratio
    AVFilterBufferRef *outsamplesref;
    int unconsumed_nb_samples,
        cached_nb_samples;
    int16_t *cached_data[8],
            *resampled_data[8];

} ResampleContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ResampleContext *resample = ctx->priv;
    char rate_str[128] = "", *tail;
    resample->out_rate = -1;

    if (args)
        sscanf(args, "%127[a-z0-9]", rate_str);

    if (*rate_str) {
        double d = av_strtod(rate_str, &tail);
        if (*tail || d < 0 || (int)d != d) {
            av_log(ctx, AV_LOG_ERROR, "Invalid value '%s' for rate",
                   rate_str);
            return AVERROR(EINVAL);
        }
        resample->out_rate = d;
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ResampleContext *resample = ctx->priv;
    int nb_channels =
        av_get_channel_layout_nb_channels(
            resample->outsamplesref->audio->channel_layout);
    avfilter_unref_buffer(resample->outsamplesref);

    while (nb_channels--) {
        av_freep(&resample->cached_data[nb_channels]);
        av_freep(&resample->resampled_data[nb_channels]);
    }

    av_resample_close(resample->resample);
    resample->resample = NULL;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    ResampleContext *resample = ctx->priv;

    /* if not specified uses the same sample format and layout as
     * specified in output */
    if (resample->out_rate == -1)
        resample->out_rate = inlink->sample_rate;
    outlink->sample_rate = resample->out_rate;

    /* fixme: make the resampling parameters configurable */
    resample->resample = av_resample_init(resample->out_rate, inlink->sample_rate,
                                          16, 10, 0, 0.8);

    resample->ratio = (double)outlink->sample_rate / inlink->sample_rate;

    av_log(ctx, AV_LOG_INFO, "r:%"PRId64" -> r:%"PRId64"\n",
           inlink->sample_rate, outlink->sample_rate);
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE
    };

    avfilter_set_common_sample_formats(ctx, avfilter_make_format_list(sample_fmts));
    avfilter_set_common_channel_layouts(ctx, avfilter_all_channel_layouts());
    return 0;
}

static void deinterleave(int16_t **outp, int16_t *in,
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

static void interleave(int16_t *out, int16_t **inp,
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

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    ResampleContext *resample    = inlink->dst->priv;
    AVFilterLink * const outlink = inlink->dst->outputs[0];
    int i,
        in_nb_samples            = insamplesref->audio->nb_samples,
        cached_nb_samples        = in_nb_samples + resample->unconsumed_nb_samples,
        requested_out_nb_samples = resample->ratio * cached_nb_samples,
        nb_channels              =
            av_get_channel_layout_nb_channels(inlink->channel_layout);
    if (cached_nb_samples > resample->cached_nb_samples) {
        for (i = 0; i < nb_channels; i++) {
            resample->cached_data[i]    =
                av_realloc(resample->cached_data[i], cached_nb_samples * sizeof(int16_t));
            resample->resampled_data[i] =
                av_realloc(resample->resampled_data[i], 4 * requested_out_nb_samples + 16);

            if (resample->cached_data[i] == NULL || resample->resampled_data[i] == NULL)
                return;
        }
        if (resample->outsamplesref)
            avfilter_unref_buffer(resample->outsamplesref);
        resample->outsamplesref = avfilter_get_audio_buffer(outlink,
                                                            AV_PERM_WRITE | AV_PERM_REUSE2,
                                                            inlink->format,
                                                            requested_out_nb_samples,
                                                            insamplesref->audio->channel_layout,
                                                            insamplesref->audio->planar);
        resample->outsamplesref->audio->sample_rate = outlink->sample_rate;
        resample->cached_nb_samples = cached_nb_samples;
        outlink->out_buf = resample->outsamplesref;
    }

    /* av_resample() works with planar audio buffers */
    if (!inlink->planar && nb_channels > 1) {
        int16_t *out[8];
        for (i = 0; i < nb_channels; i++)
            out[i] = resample->cached_data[i] + resample->unconsumed_nb_samples;

        deinterleave(out, (int16_t *)insamplesref->data[0],
                     nb_channels, in_nb_samples);
    } else {
        for (i = 0; i < nb_channels; i++)
            memcpy(resample->cached_data[i] + resample->unconsumed_nb_samples,
                   insamplesref->data[i],
                   in_nb_samples * sizeof(int16_t));
    }

    for (i = 0; i < nb_channels; i++) {
        int consumed;
        const int is_last = i+1 == nb_channels;

        resample->outsamplesref->audio->nb_samples =
            av_resample(resample->resample,
                        resample->resampled_data[i], resample->cached_data[i],
                        &consumed,
                        cached_nb_samples,
                        requested_out_nb_samples, is_last);

        /* move unconsumed data back to the beginning of the cache */
        resample->unconsumed_nb_samples = cached_nb_samples - consumed;
        memmove(resample->cached_data[i], resample->cached_data[i] + consumed,
                resample->unconsumed_nb_samples * sizeof(int16_t));
    }


    /* copy resampled data to the output samplesref */
    if (!inlink->planar && nb_channels > 1) {
        interleave((int16_t *)resample->outsamplesref->data[0],
                   resample->resampled_data,
                   nb_channels, resample->outsamplesref->audio->nb_samples);
    } else {
        for (i = 0; i < nb_channels; i++)
            memcpy(resample->outsamplesref->data[i], resample->resampled_data[i],
                   resample->outsamplesref->audio->nb_samples * sizeof(int16_t));
    }

    avfilter_filter_samples(outlink, avfilter_ref_buffer(resample->outsamplesref, ~0));
    avfilter_unref_buffer(insamplesref);
}

AVFilter avfilter_af_aresample = {
    .name          = "aresample",
    .description   = NULL_IF_CONFIG_SMALL("Resample audio data."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(ResampleContext),

    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples   = filter_samples,
                                    .min_perms        = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .config_props     = config_props,
                                    .type             = AVMEDIA_TYPE_AUDIO, },
                                  { .name = NULL}},
};
