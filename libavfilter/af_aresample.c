/*
 * Copyright (c) 2011 Stefano Sabatini
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
    int unconsumed_nb_samples;
    int16_t *unconsumed_data[8];
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
    enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE
    };

    avfilter_set_common_sample_formats(ctx, avfilter_make_format_list(sample_fmts));
    avfilter_set_common_channel_layouts(ctx, avfilter_all_channel_layouts());
    return 0;
}

/**
 * Split a packed stereo plane into two distinct mono planes.
 */
static void stereo_split(int16_t *output1, int16_t *output2,
                         int16_t *input, int nb_samples)
{
    while (nb_samples--) {
        *output1++ = *input++;
        *output2++ = *input++;
    }
}

/**
 * Mux two distinct mono planes to a packed stereo plane.
 */
static void stereo_mux(int16_t *output,
                       int16_t *input1, int16_t *input2, int nb_samples)
{
    while (nb_samples--) {
        *output++ = *input1++;
        *output++ = *input2++;
    }
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    ResampleContext *resample = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFilterBufferRef *outsamplesref;
    int i, j, nb_channels =
        av_get_channel_layout_nb_channels(insamplesref->audio->channel_layout),
        in_nb_samples     = insamplesref->audio->nb_samples,
        cached_nb_samples = in_nb_samples + resample->unconsumed_nb_samples,
        requested_out_nb_samples = resample->ratio * cached_nb_samples,
        resampled_data_linesize = 4 * requested_out_nb_samples + 16;
    int16_t *cached_data[8] = { 0 };
    int16_t *resampled_data[8] = { 0 };
    int16_t *datap;
    int out_nb_samples;

    for (i = 0; i < nb_channels; i++) {
        cached_data[i]    = av_malloc(cached_nb_samples * 2);
        resampled_data[i] = av_malloc(resampled_data_linesize);
        /* copy unconsumed data to planar_data */
        memcpy(cached_data[i], resample->unconsumed_data[i],
               resample->unconsumed_nb_samples * 2);
    }

    /* av_resample() works with planar audio buffer, perform rematrixing
     * for having planar audio buffers */
    if (!insamplesref->audio->planar) {
        if (nb_channels == 2) {
            stereo_split(cached_data[0] + resample->unconsumed_nb_samples,
                         cached_data[1] + resample->unconsumed_nb_samples,
                         (int16_t *)insamplesref->data[0], in_nb_samples);
        } else {
            datap = (int16_t *)insamplesref->data[0];
            in_nb_samples += resample->unconsumed_nb_samples;
            for (i = resample->unconsumed_nb_samples; i < in_nb_samples; i++)
                for (j = 0; j < nb_channels; j++)
                    cached_data[j][i] = *(datap++);
            in_nb_samples -= resample->unconsumed_nb_samples;
        }
    } else {
        for (i = 0; i < nb_channels; i++)
            memcpy((uint8_t *)cached_data[i] + resample->unconsumed_nb_samples * 2,
                   insamplesref->data[i],
                   in_nb_samples * 2);
    }

    for (i = 0; i < nb_channels; i++) {
        int consumed_in_nb_samples;
        int is_last = i+1 == nb_channels;

        out_nb_samples =
            av_resample(resample->resample,
                        resampled_data[i], cached_data[i],
                        &consumed_in_nb_samples,
                        cached_nb_samples,
                        requested_out_nb_samples, is_last);

        /* save unconsumed data for the next round */
        resample->unconsumed_nb_samples = cached_nb_samples - consumed_in_nb_samples;
        resample->unconsumed_data[i] =
            av_realloc(resample->unconsumed_data[i], resample->unconsumed_nb_samples * 2);
        memcpy(resample->unconsumed_data[i],
               cached_data[i] + consumed_in_nb_samples,
               resample->unconsumed_nb_samples * 2);
    }

    outsamplesref = avfilter_get_audio_buffer(outlink, AV_PERM_WRITE,
                                              inlink->format,
                                              out_nb_samples,
                                              insamplesref->audio->channel_layout,
                                              insamplesref->audio->planar);
    outsamplesref->audio->sample_rate = outlink->sample_rate;
    outlink->out_buf = outsamplesref;

    /* copy resampled data to the output samplesref */
    if (!outsamplesref->audio->planar) {
        if(nb_channels == 2) {
        stereo_mux((int16_t *)outsamplesref->data[0],
                   resampled_data[0], resampled_data[1],
                   out_nb_samples);
        } else {
            datap = (int16_t *)outsamplesref->data[0];
            for (i = 0; i < out_nb_samples; i++)
                for (j = 0; j < nb_channels; j++)
                    *datap++ = resampled_data[j][i];
        }
    } else {
        for (i = 0; i < nb_channels; i++)
            memcpy(outsamplesref->data[i], resampled_data[i],
                   out_nb_samples * 2);
    }

    for (i = 0; i < nb_channels; i++) {
        av_freep(&cached_data[i]);
        av_freep(&resampled_data[i]);
    }

    avfilter_filter_samples(outlink, outsamplesref);
    avfilter_unref_buffer(insamplesref);
}

AVFilter avfilter_af_aresample = {
    .name        = "aresample",
    .description = NULL_IF_CONFIG_SMALL("Resample input audio."),
    .init      = init,
    .uninit    = uninit,
    .query_formats = query_formats,

    .priv_size = sizeof(ResampleContext),

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
