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
 * audio buffer sink
 */

#include "avfilter.h"
#include "asink_abuffer.h"

static void filter_samples(AVFilterLink *link, AVFilterBufferRef *samplesref)
{
}

static int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    if (!opaque)
        return AVERROR(EINVAL);
    memcpy(ctx->priv, opaque, sizeof(ABufferSinkContext));

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    ABufferSinkContext *abuffersink = ctx->priv;
    AVFilterFormats *formats;

    formats = NULL;
    avfilter_add_format(&formats, abuffersink->sample_fmt);
    avfilter_set_common_sample_formats(ctx, formats);

    formats = NULL;
    avfilter_add_format(&formats, abuffersink->channel_layout);
    avfilter_set_common_channel_layouts(ctx, formats);

    formats = NULL;
    avfilter_add_format(&formats, AVFILTER_PACKED); //FIXME
    avfilter_set_common_packing_formats(ctx, formats);

    return 0;
}

int av_asink_abuffer_get_audio_buffer_ref(AVFilterContext *abuffer_asink,
                              AVFilterBufferRef **samplesref)
{
    int ret;

    if ((ret = avfilter_request_frame(abuffer_asink->inputs[0])))
        return ret;
    if (!abuffer_asink->inputs[0]->cur_buf)
        return AVERROR(EINVAL);
    *samplesref = abuffer_asink->inputs[0]->cur_buf;
    abuffer_asink->inputs[0]->cur_buf = NULL;

    return 0;
}

AVFilter avfilter_asink_abuffersink = {
    .name      = "abuffersink",
    .init      = init,
    .priv_size = sizeof(ABufferSinkContext),
    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name           = "default",
                                    .type           = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples = filter_samples,
                                    .min_perms      = AV_PERM_READ, },
                                  { .name = NULL }},
    .outputs   = (AVFilterPad[]) {{ .name = NULL }},
};

