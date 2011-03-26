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
 * Sox wrapper.
 */

#include <sox.h>
#include <string.h>

#include "libavutil/mem.h"
#include "libavcodec/audioconvert.h"
#include "avfilter.h"

static int soxinit = -1;

typedef struct {
    sox_effect_t *effect;
} SoxContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    avfilter_add_format(&formats, AV_SAMPLE_FMT_S32);
    avfilter_set_common_sample_formats(ctx, formats);
    avfilter_set_common_channel_layouts(ctx, avfilter_all_channel_layouts());
    avfilter_set_common_packing_formats(ctx, avfilter_all_packing_formats());

    return 0;
}

#define NUM_ADDED_ARGS 5
static inline int realloc_argv(char ***argv, int *numargs){
    *numargs += NUM_ADDED_ARGS;
    *argv = av_realloc(*argv, numargs * sizeof(**argv));
    if (*argv == NULL) {
        av_free(*argv);
        return 0;
    } else
        return numargs;
}

static av_cold int init(AVFilterContext *ctx, const char *argstr, void *opaque)
{
    char **argv = NULL; int argc = 0, numargs = 0;
    sox_effect_t *effect = NULL;
    sox_encodinginfo_t *enc;

    if (soxinit != SOX_SUCCESS && (soxinit = sox_init()) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n",
            sox_strerror(soxinit));
        return AVERROR(EINVAL);
    }

    realloc_argv(&argv, &numargs);
    argv[argc++] = strtok(argstr, " ");
    while (argv[argc++] = strtok(NULL, " ")) {
        if (argc == numargs) {
            realloc_argv(&argv, &numargs);
            if (!numargs) {
                av_log(ctx, AV_LOG_ERROR, "Could not allocate memory!\n");
                return AVERROR(ENOMEM);
            }
        }
    }

    effect = sox_create_effect(sox_find_effect(argv[0]));
    if (!effect) {
        av_log(ctx, AV_LOG_ERROR, "Could not create Sox effect '%s'.\n",
            argstr);
        return AVERROR(EINVAL);
    }

    if (effect->handler.getopts(effect, argc-1, argv) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Invalid arguments to Sox effect.\n");
        return AVERROR(EINVAL);
    }

    enc = av_malloc(sizeof(sox_encodinginfo_t));
    memset(enc, 0, sizeof(sox_encodinginfo_t));
    enc->bits_per_sample = 32;
    enc->encoding        = SOX_DEFAULT_ENCODING;
    effect->out_encoding = effect->in_encoding = enc;
    effect->clips        = 0;
    effect->imin         = 0;

    ((SoxContext*)ctx->priv)->effect = effect;
    return 0;
}

static int config_input(AVFilterLink *link)
{
    sox_effect_t *effect = ((SoxContext*)link->dst->priv)->effect;

    effect->in_signal.precision = 32;
    effect->in_signal.rate      = link->sample_rate;
    effect->in_signal.channels  =
                       av_get_channel_layout_nb_channels(link->channel_layout);
    return 0;
}

static int config_output(AVFilterLink *link)
{
    sox_effect_t *effect = ((SoxContext*)link->src->priv)->effect;

    if (!(effect->handler.flags & SOX_EFF_CHAN))
        effect->out_signal.channels = effect->in_signal.channels;
    if (!(effect->handler.flags & SOX_EFF_RATE))
        effect->out_signal.rate = effect->in_signal.rate;
    if (!(effect->handler.flags & SOX_EFF_PREC))
        if (effect->handler.flags & SOX_EFF_MODIFY)
            effect->out_signal.precision = effect->in_signal.precision;
        else
            effect->out_signal.precision = SOX_SAMPLE_PRECISION;
    if (!(effect->handler.flags & SOX_EFF_GAIN))
        effect->out_signal.mult = effect->in_signal.mult;

    //FIXME: we depend on the fact that config_input is always
    //called before config_output
    effect->handler.start(effect);

    link->sample_rate    = effect->out_signal.rate;
    link->channel_layout = avcodec_guess_channel_layout(
        effect->out_signal.channels, 0, NULL);
    return 0;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamples)
{
    sox_effect_t *effect = ((SoxContext*)inlink->dst->priv)->effect;
    AVFilterBufferRef *outsamples;
    size_t isamp, osamp;

    //FIXME not handling planar data
    isamp = insamples->audio->nb_samples;
    outsamples = avfilter_get_audio_buffer(
        inlink, AV_PERM_WRITE, AV_SAMPLE_FMT_S32,
        isamp, inlink->channel_layout, 0);

    osamp = isamp = isamp * effect->out_signal.channels;
    //FIXME not handling cases where not all the input is consumed
    effect->handler.flow(effect, (int32_t*)insamples->data[0],
        (int32_t*)outsamples->data[0], &isamp, &osamp);

    outsamples->audio->nb_samples = osamp / effect->out_signal.channels;
    avfilter_filter_samples(inlink->dst->outputs[0], outsamples);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    sox_effect_t *effect = ((SoxContext*)ctx->priv)->effect;
    av_free(effect->in_encoding);
    sox_delete_effect(effect);
    sox_quit();
}

AVFilter avfilter_af_sox = {
    .name          = "sox",
    .description   = NULL_IF_CONFIG_SMALL("SoX effects library wrapper."),
    .priv_size     = sizeof(SoxContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples   = filter_samples,
                                    .config_props     = config_input,
                                    .min_perms        = AV_PERM_READ },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .config_props     = config_output, },
                                  { .name = NULL}},
};
