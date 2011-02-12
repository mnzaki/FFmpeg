/*
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram
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

#include "libavutil/eval.h"
#include "libavutil/fifo.h"
#include "libavutil/audioconvert.h"
#include "asrc_abuffer.h"

#define FIFO_SIZE 8

typedef struct {
    unsigned int sample_fmt;  ///< initial sample format indicated by client
    int64_t ch_layout;        ///< initial channel layout indicated by client
    int sample_rate;
    AVFifoBuffer *fifo;       ///< FIFO buffer of audio frame pointers
} ABufferSourceContext;

static void buf_free(AVFilterBuffer *ptr)
{
    av_free(ptr);
    return;
}

int av_asrc_buffer_add_samples(AVFilterContext *ctx,
                               uint8_t *data[8], int linesize[8], int nb_samples,
                               int sample_fmt, int64_t ch_layout, int planar,
                               int64_t pts)
{
    ABufferSourceContext *abuffer = ctx->priv;
    AVFilterBufferRef *samplesref;

    if (av_fifo_space(abuffer->fifo) < sizeof(samplesref)) {
        av_log(ctx, AV_LOG_ERROR,
               "Buffering limit reached. Please consume some available frames "
               "before adding new ones.\n");
        return AVERROR(ENOMEM);
    }

    samplesref = avfilter_get_audio_buffer_ref_from_arrays(data, linesize, AV_PERM_WRITE,
                                                           nb_samples, sample_fmt,
                                                           ch_layout, planar);
    if (!samplesref)
        return AVERROR(ENOMEM);
    samplesref->buf->free  = buf_free;
    samplesref->pts = pts;

    av_fifo_generic_write(abuffer->fifo, &samplesref, sizeof(samplesref), NULL);

    return 0;
}

int av_asrc_buffer_add_buffer(AVFilterContext *ctx,
                              uint8_t *buf, int buf_size,
                              int sample_fmt, int64_t ch_layout,
                              int64_t pts)
{
    /* compute pointers from buffer */
    uint8_t *data[8];
    int linesize[8];
    int nb_channels = av_get_channel_layout_nb_channels(ch_layout),
        sample_size = av_get_bytes_per_sample(sample_fmt),
        nb_samples = buf_size / sample_size / nb_channels;

    memset(data,     0, 8 * sizeof(data[0]));
    memset(linesize, 0, 8 * sizeof(linesize[0]));

    data[0] = buf;
    linesize[0] = nb_samples * sample_size * nb_channels;

    return av_asrc_buffer_add_samples(ctx,
                                      data, linesize, nb_samples,
                                      sample_fmt, ch_layout, 0, pts);
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ABufferSourceContext *abuffer = ctx->priv;
    char sample_fmt_str[32], ch_layout_str[256], sample_rate_str[256];
    int n = 0;
    char *tail;
    double sample_rate;

    if (!args ||
        (n = sscanf(args, "%31[^:]:%255[^:]:%255s", sample_fmt_str, ch_layout_str, sample_rate_str)) != 3) {
        av_log(ctx, AV_LOG_ERROR, "Expected 3 arguments, but only %d found in '%s'\n", n, args);
        return AVERROR(EINVAL);
    }

    abuffer->sample_fmt = av_get_sample_fmt(sample_fmt_str);
    if (abuffer->sample_fmt == AV_SAMPLE_FMT_NONE) {
        char *tail;
        abuffer->sample_fmt = strtol(sample_fmt_str, &tail, 0);
        if (*tail || (unsigned)abuffer->sample_fmt >= AV_SAMPLE_FMT_NB) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sample format '%s'\n", sample_fmt_str);
            return AVERROR(EINVAL);
        }
    }

    abuffer->ch_layout = av_get_channel_layout(ch_layout_str);
    if (abuffer->ch_layout < AV_CH_LAYOUT_STEREO) {
        char *tail;
        abuffer->ch_layout = strtol(ch_layout_str, &tail, 0);
        if (*tail || abuffer->ch_layout < AV_CH_LAYOUT_STEREO) {
            av_log(ctx, AV_LOG_ERROR, "Invalid channel layout '%s'\n", ch_layout_str);
            return AVERROR(EINVAL);
        }
    }

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

    av_get_channel_layout_string(ch_layout_str, sizeof(ch_layout_str),
                                 -1, abuffer->ch_layout);
    av_log(ctx, AV_LOG_INFO, "fmt:%s ch_layout:%s sr:%d\n",
           av_get_sample_fmt_name(abuffer->sample_fmt), ch_layout_str,
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
    enum AVSampleFormat sample_fmts[] = {
        abuffer->sample_fmt, AV_SAMPLE_FMT_NONE
    };
    int64_t ch_layouts[] = { abuffer->ch_layout, -1 };

    avfilter_set_common_sample_formats(ctx, avfilter_make_format_list(sample_fmts));
    avfilter_set_common_channel_layouts(ctx, avfilter_make_format64_list(ch_layouts));
    return 0;
}

static int config_props(AVFilterLink *inlink)
{
    ABufferSourceContext *abuffer = inlink->src->priv;
    inlink->format         = abuffer->sample_fmt;
    inlink->channel_layout = abuffer->ch_layout;
    inlink->sample_rate    = abuffer->sample_rate;
    return 0;
}

static int request_frame(AVFilterLink *inlink)
{
    ABufferSourceContext *abuffer = inlink->src->priv;
    AVFilterBufferRef *samplesref;

    if (!av_fifo_size(abuffer->fifo)) {
        av_log(inlink->src, AV_LOG_ERROR,
               "request_frame() called with no available frames!\n");
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

AVFilter avfilter_asrc_abuffer =
{
    .name      = "abuffer",
    .description = NULL_IF_CONFIG_SMALL("Buffer audio frames, and make them accessible to the filterchain."),
    .priv_size = sizeof(ABufferSourceContext),
    .query_formats = query_formats,

    .init      = init,
    .uninit    = uninit,

    .inputs    = (AVFilterPad[]) {{ .name = NULL }},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_AUDIO,
                                    .request_frame   = request_frame,
                                    .poll_frame      = poll_frame,
                                    .config_props    = config_props, },
                                  { .name = NULL}},
};
