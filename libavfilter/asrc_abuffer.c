/*
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram
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
 * Memory buffer source filter for audio
 */

#include "libavutil/audioconvert.h"
#include "libavutil/eval.h"
#include "asrc_abuffer.h"

typedef struct {
    // Audio format of incoming buffers
    int sample_rate;
    unsigned int sample_fmt;
    int64_t channel_layout;
    int planar;
    // FIFO buffer of audio frame pointers
    AVFifoBuffer *fifo;
    // Normalization filters
    AVFilterContext *aconvert;
    AVFilterContext *aresample;
} ABufferSourceContext;

#define FIFO_SIZE 8

static void buf_free(AVFilterBuffer *ptr)
{
    av_free(ptr);
    return;
}

static inline void setup_link(ABufferSourceContext *abuffer, AVFilterLink *link)
{
    link->format         = abuffer->sample_fmt;
    link->channel_layout = abuffer->channel_layout;
    link->planar         = abuffer->planar;
    link->sample_rate    = abuffer->sample_rate;
}

static int insert_filter(ABufferSourceContext *abuffer,
                         AVFilterLink *link, AVFilterContext **filter,
                         const char *filt_name, const char *args)
{
    int ret;

    if ((ret = avfilter_open(filter, avfilter_get_by_name(filt_name),
                            "audio filter")) < 0)
        return ret;

    link->src->outputs[0] = NULL;
    if ((ret = avfilter_link(link->src, 0, *filter, 0)) < 0) {
        link->src->outputs[0] = link;
        return ret;
    }

    link->src             = *filter;
    link->srcpad          = &((*filter)->output_pads[0]);
    (*filter)->outputs[0] = link;

    setup_link(abuffer, (*filter)->inputs[0]);

    if ((ret = avfilter_init_filter(*filter, args,  NULL)) < 0)
    {
        avfilter_free(*filter);
        return ret;
    }

    (*filter)->outputs[0]->srcpad->config_props((*filter)->outputs[0]);

    return 0;
}

static int remove_filter(AVFilterContext *ctx, AVFilterContext **filter)
{
    int ret;
    AVFilterContext *dst = (*filter)->outputs[0]->dst;
    unsigned dstpad_idx = (*filter)->outputs[0]->dstpad - dst->input_pads;

    avfilter_free(*filter);
    *filter = NULL;

    if ((ret = avfilter_link(ctx, 0, dst, dstpad_idx)) < 0)
        return ret;

    setup_link(ctx->priv, ctx->outputs[0]);

    return 0;
}

static int reconfigure_filter(AVFilterContext *filter)
{
    int ret;

    filter->filter->uninit(filter);
    if ((ret = filter->filter->init(filter, NULL, NULL)) < 0)
        return ret;
    if ((ret = filter->outputs[0]->srcpad->config_props(
                                               filter->outputs[0])) < 0)
        return ret;
    if ((ret = filter->inputs[0]->srcpad->config_props(
                                               filter->inputs[0])) < 0)
        return ret;

    return 0;
}

int av_asrc_buffer_add_audio_buffer_ref(AVFilterContext *ctx,
                                        AVFilterBufferRef *samplesref,
                                        int av_unused flags)
{
    ABufferSourceContext *abuffer = ctx->priv;
    AVFilterLink *link;
    int ret;

    if (av_fifo_space(abuffer->fifo) < sizeof(samplesref)) {
        av_log(ctx, AV_LOG_ERROR,
               "Buffering limit reached. Please consume some available frames "
               "before adding new ones.\n");
        return AVERROR(ENOMEM);
    }

    // Normalize input

    link = ctx->outputs[0];
    if (!link->sample_rate)
        abuffer->sample_rate = link->sample_rate = samplesref->audio->sample_rate;
    if (samplesref->audio->sample_rate != link->sample_rate) {
        av_log(ctx, AV_LOG_INFO, "Audio sample rate changed, normalizing\n");
        if (!abuffer->aresample) {
            char args[16];
            snprintf(args, sizeof(args), "%i", abuffer->sample_rate);
            ret = insert_filter(abuffer, link, &abuffer->aresample, "aresample", args);
            if (ret < 0) return ret;
        } else {
            link = abuffer->aresample->outputs[0];
            if (samplesref->audio->sample_rate == link->sample_rate)
                remove_filter(ctx, &abuffer->aresample);
            else
                reconfigure_filter(abuffer->aresample);
        }
    }

    link = ctx->outputs[0];
    if (samplesref->format                != link->format         ||
        samplesref->audio->channel_layout != link->channel_layout ||
        samplesref->audio->planar         != link->planar) {

        abuffer->sample_fmt     = samplesref->format;
        abuffer->channel_layout = samplesref->audio->channel_layout;
        abuffer->planar         = samplesref->audio->planar;

        av_log(ctx, AV_LOG_INFO, "Audio input format changed, normalizing\n");

        if (!abuffer->aconvert) {
            ret = insert_filter(abuffer, link, &abuffer->aconvert, "aconvert", NULL);
            if (ret < 0) return ret;
        } else {
            link = abuffer->aconvert->outputs[0];
            if (samplesref->format                == link->format         &&
                samplesref->audio->channel_layout == link->channel_layout &&
                samplesref->audio->planar         == link->planar
               )
                remove_filter(ctx, &abuffer->aconvert);
            else
                reconfigure_filter(abuffer->aconvert);
        }
    }

    if (sizeof(samplesref) != av_fifo_generic_write(abuffer->fifo, &samplesref,
                                                    sizeof(samplesref), NULL))
        return AVERROR(ENOMEM);

    return 0;
}

int av_asrc_buffer_add_samples(AVFilterContext *ctx,
                               uint8_t *data[8], int linesize[8],
                               int nb_samples, int sample_rate,
                               int sample_fmt, int64_t channel_layout, int planar,
                               int64_t pts, int av_unused flags)
{
    AVFilterBufferRef *samplesref;

    samplesref = avfilter_get_audio_buffer_ref_from_arrays(
                     data, linesize, AV_PERM_WRITE,
                     nb_samples,
                     sample_fmt, channel_layout, planar);
    if (!samplesref)
        return AVERROR(ENOMEM);

    samplesref->buf->free  = buf_free;
    samplesref->pts = pts;
    samplesref->audio->sample_rate = sample_rate;

    return av_asrc_buffer_add_audio_buffer_ref(ctx, samplesref, 0);
}

int av_asrc_buffer_add_buffer(AVFilterContext *ctx,
                              uint8_t *buf, int buf_size, int sample_rate,
                              int sample_fmt, int64_t channel_layout, int planar,
                              int64_t pts, int av_unused flags)
{
    uint8_t *data[8];
    int linesize[8];
    int nb_channels = av_get_channel_layout_nb_channels(channel_layout),
        nb_samples  = buf_size / nb_channels / av_get_bytes_per_sample(sample_fmt);

    av_samples_fill_arrays(data, linesize,
                           buf, nb_channels, nb_samples,
                           sample_fmt, planar, 16); //FIXME align?

    return av_asrc_buffer_add_samples(ctx,
                                      data, linesize, nb_samples,
                                      sample_rate,
                                      sample_fmt, channel_layout, planar,
                                      pts, flags);
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ABufferSourceContext *abuffer = ctx->priv;
    char sample_fmt_str[8],   chlayout_str[16],
         sample_rate_str[16], packing_str[16];
    int n = 0;
    char *tail;
    double sample_rate;

    n = sscanf(args, "%15[^:]:%7[^:]:%15[^:]:%15s",
               sample_rate_str, sample_fmt_str, chlayout_str, packing_str );
    if (!args || n != 4) {
        av_log(ctx, AV_LOG_ERROR,
               "Expected 4 arguments, but only %d found in '%s'\n", n, args);
        return AVERROR(EINVAL);
    }

    abuffer->sample_fmt = av_get_sample_fmt(sample_fmt_str);
    if (abuffer->sample_fmt == AV_SAMPLE_FMT_NONE) {
        char *tail;
        abuffer->sample_fmt = strtol(sample_fmt_str, &tail, 0);
        if (*tail || (unsigned)abuffer->sample_fmt >= AV_SAMPLE_FMT_NB) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sample format '%s'\n",
                   sample_fmt_str);
            return AVERROR(EINVAL);
        }
    }

    abuffer->channel_layout = av_get_channel_layout(chlayout_str);
    if (abuffer->channel_layout < AV_CH_LAYOUT_STEREO) {
        char *tail;
        abuffer->channel_layout = strtol(chlayout_str, &tail, 0);
        if (*tail || abuffer->channel_layout < AV_CH_LAYOUT_STEREO) {
            av_log(ctx, AV_LOG_ERROR, "Invalid channel layout '%s'\n",
                   chlayout_str);
            return AVERROR(EINVAL);
        }
    }

    abuffer->planar = (strcmp(packing_str, "packed") != 0);

    sample_rate = av_strtod(sample_rate_str, &tail);
    if (*tail || sample_rate < 0 || (int)sample_rate != sample_rate) {
        av_log(ctx, AV_LOG_ERROR, "Invalid value '%s' for rate",
               sample_rate_str);
        return AVERROR(EINVAL);
    }
    abuffer->sample_rate = sample_rate;

    abuffer->fifo = av_fifo_alloc(FIFO_SIZE*sizeof(AVFilterBufferRef*));
    if (!abuffer->fifo) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate fifo, filter init failed.\n");
        return AVERROR(ENOMEM);
    }

    av_get_channel_layout_string(chlayout_str, sizeof(chlayout_str),
                                 -1, abuffer->channel_layout);
    av_log(ctx, AV_LOG_INFO, "fmt:%s channel_layout:%s rate:%d\n",
           av_get_sample_fmt_name(abuffer->sample_fmt), chlayout_str,
           abuffer->sample_rate);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ABufferSourceContext *abuffer = ctx->priv;
    av_fifo_free(abuffer->fifo);
}

static int query_formats(AVFilterContext *ctx)
{
    ABufferSourceContext *abuffer = ctx->priv;
    AVFilterFormats *formats;

    formats = NULL;
    avfilter_add_format(&formats, abuffer->sample_fmt);
    avfilter_set_common_sample_formats(ctx, formats);

    formats = NULL;
    avfilter_add_format(&formats, abuffer->channel_layout);
    avfilter_set_common_channel_layouts(ctx, formats);

    formats = NULL;
    avfilter_add_format(&formats,
        abuffer->planar ? AVFILTER_PLANAR : AVFILTER_PACKED);
    avfilter_set_common_packing_formats(ctx, formats);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    ABufferSourceContext *abuffer = outlink->src->priv;
    outlink->sample_rate = abuffer->sample_rate;
    return 0;
}

static int request_frame(AVFilterLink *inlink)
{
    ABufferSourceContext *abuffer = inlink->src->priv;
    AVFilterBufferRef *samplesref;

    if (!av_fifo_size(abuffer->fifo)) {
        av_log(inlink->src, AV_LOG_ERROR,
               "request_frame() called with no available frames!\n");
        return AVERROR(EINVAL);
    }

    av_fifo_generic_read(abuffer->fifo, &samplesref, sizeof(samplesref), NULL);
    avfilter_filter_samples(inlink, avfilter_ref_buffer(samplesref, ~0));
    avfilter_unref_buffer(samplesref);

    return 0;
}

static int poll_frame(AVFilterLink *link)
{
    ABufferSourceContext *abuffer = link->src->priv;
    return av_fifo_size(abuffer->fifo)/sizeof(AVFilterBufferRef*);
}

AVFilter avfilter_asrc_abuffer = {
    .name        = "abuffer",
    .description = NULL_IF_CONFIG_SMALL("Buffer audio frames, and make them accessible to the filterchain."),
    .priv_size   = sizeof(ABufferSourceContext),
    .query_formats = query_formats,

    .init        = init,
    .uninit      = uninit,

    .inputs      = (AVFilterPad[]) {{ .name = NULL }},
    .outputs     = (AVFilterPad[]) {{ .name            = "default",
                                      .type            = AVMEDIA_TYPE_AUDIO,
                                      .request_frame   = request_frame,
                                      .poll_frame      = poll_frame,
                                      .config_props    = config_output, },
                                    { .name = NULL}},
};
