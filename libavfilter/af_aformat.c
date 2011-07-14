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
 * format and noformat audio filters
 */

#include "libavcodec/audioconvert.h"
#include "avfilter.h"

typedef struct {
    AVFilterFormats *formats, *chlayouts, *packing;
} AFormatContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AFormatContext * const aformat = ctx->priv;
    char *arg, *fmt_str;
    int fmt;

    if (!args) return AVERROR(EINVAL);

    arg = strsep(&args, ":");
    while (fmt_str = strsep(&arg, ",")) {
        if (!strcmp(fmt_str, "all")) {
            aformat->formats = avfilter_all_formats(AVMEDIA_TYPE_AUDIO);
            break;
        }
        fmt = av_get_sample_fmt(fmt_str);
        if (fmt == -1) {
            av_log(ctx, AV_LOG_ERROR, "Bad sample format '%s'\n", fmt_str);
            return AVERROR(EINVAL);
        }
        avfilter_add_format(&aformat->formats, fmt);
    }

    arg = strsep(&args, ":");
    while (fmt_str = strsep(&arg, ",")) {
        if (!strcmp(fmt_str, "all")) {
            aformat->chlayouts = avfilter_all_channel_layouts();
            break;
        }
        fmt = av_get_channel_layout(fmt_str);
        if (fmt == -1) {
            av_log(ctx, AV_LOG_ERROR, "Bad channel layout '%s'\n", fmt_str);
            return AVERROR(EINVAL);
        }
        avfilter_add_format(&aformat->chlayouts, fmt);
    }

    arg = strsep(&args, ":");
    while (fmt_str = strsep(&arg, ",")) {
        if (!strcmp(fmt_str, "all")) {
            aformat->packing = avfilter_all_packing_formats();
            break;
        }
        char *tail;
        fmt = strtol(fmt_str, &tail, 10);
        if (*tail) fmt = strcmp(fmt_str, "packed");
        avfilter_add_format(&aformat->packing,
                            fmt ? AVFILTER_PLANAR : AVFILTER_PACKED);
    }

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    AFormatContext * const aformat = ctx->priv;

    avfilter_set_common_sample_formats (ctx, aformat->formats);
    avfilter_set_common_channel_layouts(ctx, aformat->chlayouts);
    avfilter_set_common_packing_formats(ctx, aformat->packing);
    return 0;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    avfilter_filter_samples(inlink->dst->outputs[0], insamplesref);
}

AVFilter avfilter_af_aformat = {
    .name          = "aformat",
    .description   = NULL_IF_CONFIG_SMALL(""),
    .init          = init,
    .query_formats = query_formats,
    .priv_size     = sizeof(AFormatContext),

    .inputs        = (AVFilterPad[]) {{ .name            = "default",
                                        .type            = AVMEDIA_TYPE_AUDIO,
                                        .filter_samples  = filter_samples},
                                      { .name = NULL}},
    .outputs       = (AVFilterPad[]) {{ .name            = "default",
                                        .type            = AVMEDIA_TYPE_AUDIO},
                                      { .name = NULL}},
};
