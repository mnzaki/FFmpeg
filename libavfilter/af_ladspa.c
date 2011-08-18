 /**
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
 * LADSPA plugin audio filter
 */

#include <dlfcn.h>
#include <ladspa.h>
#include "libavcodec/audioconvert.h"
#include "avfilter.h"
#include "internal.h"

#define LADSPA_SRC_NB_SAMPLES 1024

typedef struct LADSPAContext {
    const LADSPA_Descriptor *desc;
    unsigned nb_handles;
    LADSPA_Handle *handles[8];

    unsigned sample_rate;

    unsigned nb_ctls;
    LADSPA_PortDescriptor *ctl_ports_map;
    LADSPA_Data *ctl_values;
    int *ctl_needs_value;

    LADSPA_Data out_ctl_value;

    unsigned nb_ins;
    LADSPA_PortDescriptor *in_ports_map;

    unsigned nb_outs;
    LADSPA_PortDescriptor *out_ports_map;

    AVFilterBufferRef *outsamplesref;
} LADSPAContext;

static void *try_load(const char *dir, const char *soname)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.so", dir, soname);
    return dlopen(path, RTLD_NOW);
}

static inline void set_default_ctl_value(LADSPAContext *ladspa, int ctl)
{
    const LADSPA_PortRangeHint *h =
        ladspa->desc->PortRangeHints + ladspa->ctl_ports_map[ctl];

    const LADSPA_Data lower = h->LowerBound;
    const LADSPA_Data upper = h->UpperBound;

    if      (LADSPA_IS_HINT_DEFAULT_MINIMUM(h->HintDescriptor))
        ladspa->ctl_values[ctl] = lower;

    else if (LADSPA_IS_HINT_DEFAULT_MAXIMUM(h->HintDescriptor))
            ladspa->ctl_values[ctl] = upper;

    else if (LADSPA_IS_HINT_DEFAULT_0(h->HintDescriptor))
            ladspa->ctl_values[ctl] = 0.0;

    else if (LADSPA_IS_HINT_DEFAULT_1(h->HintDescriptor))
            ladspa->ctl_values[ctl] = 1.0;

    else if (LADSPA_IS_HINT_DEFAULT_100(h->HintDescriptor))
            ladspa->ctl_values[ctl] = 100.0;

    else if (LADSPA_IS_HINT_DEFAULT_440(h->HintDescriptor))
            ladspa->ctl_values[ctl] = 440.0;

    else if (LADSPA_IS_HINT_DEFAULT_LOW(h->HintDescriptor)) {
        if (LADSPA_IS_HINT_LOGARITHMIC(h->HintDescriptor))
            ladspa->ctl_values[ctl] =
                exp(log(lower) * 0.75 + log(upper) * 0.25);
        else
            ladspa->ctl_values[ctl] = lower * 0.75 + upper * 0.25;
    }

    else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(h->HintDescriptor)) {
        if (LADSPA_IS_HINT_LOGARITHMIC(h->HintDescriptor))
            ladspa->ctl_values[ctl] =
                exp(log(lower) * 0.5 + log(upper) * 0.5);
        else
            ladspa->ctl_values[ctl] = lower * 0.5 + upper * 0.5;
    }

    else if (LADSPA_IS_HINT_DEFAULT_HIGH(h->HintDescriptor)) {
        if (LADSPA_IS_HINT_LOGARITHMIC(h->HintDescriptor))
            ladspa->ctl_values[ctl] =
                exp(log(lower) * 0.25 + log(upper) * 0.75);
        else
            ladspa->ctl_values[ctl] = lower * 0.25 + upper * 0.75;
    }

}

static void print_ctl_info(void *ctx, int level, LADSPAContext *ladspa, int ctl)
{
    const LADSPA_PortRangeHint *h =
        ladspa->desc->PortRangeHints + ladspa->ctl_ports_map[ctl];
    av_log(ctx, level, "c%i: %s [", ctl,
           ladspa->desc->PortNames[ladspa->ctl_ports_map[ctl]]);
    if (LADSPA_IS_HINT_TOGGLED(h->HintDescriptor)) {
        av_log(ctx, level, "Toggled (1 or 0)");
        if (LADSPA_IS_HINT_HAS_DEFAULT(h->HintDescriptor))
            av_log(ctx, level, ", Default %i",
                   (int)ladspa->ctl_values[ctl]);
    } else {
        if (LADSPA_IS_HINT_INTEGER(h->HintDescriptor)) {
            av_log(ctx, level, "Integer");
            if (LADSPA_IS_HINT_BOUNDED_BELOW(h->HintDescriptor))
                av_log(ctx, level, ", Min: %i", (int)h->LowerBound);
            if (LADSPA_IS_HINT_BOUNDED_ABOVE(h->HintDescriptor))
                av_log(ctx, level, ", Max: %i", (int)h->UpperBound);
            if (LADSPA_IS_HINT_HAS_DEFAULT(h->HintDescriptor))
                av_log(ctx, level, ", Default %i",
                       (int)ladspa->ctl_values[ctl]);
        } else {
            av_log(ctx, level, "Decimal");
            if (LADSPA_IS_HINT_BOUNDED_BELOW(h->HintDescriptor))
                av_log(ctx, level, ", Min: %f", h->LowerBound);
            if (LADSPA_IS_HINT_BOUNDED_ABOVE(h->HintDescriptor))
                av_log(ctx, level, ", Max: %f", h->UpperBound);
            if (LADSPA_IS_HINT_HAS_DEFAULT(h->HintDescriptor))
                av_log(ctx, level, ", Default %f",
                       ladspa->ctl_values[ctl]);
        }
        if (LADSPA_IS_HINT_SAMPLE_RATE(h->HintDescriptor)) {
            av_log(ctx, level, ", multiple of sample rate");
        }
    }
    av_log(ctx, level, "]\n");
}

/* Usage:
 * to list plugins in a library: ladspa=soname
 * to list a plugin's ports: ladspa=soname:plugin:help
 * to use a plugin: ladspa=soname:plugin:ctrl1=val1:ctrl2=val2;
 */
static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    LADSPAContext *ladspa = ctx->priv;
    char *arg = NULL, *arg1 = NULL, *soname = NULL, *tail, *ptr;
    void *so = NULL;
    int i;
    LADSPA_Descriptor_Function ladspa_desc_fn = NULL;

    soname = strtok_r(args, ":", &ptr);
    if (!soname) {
        av_log(ctx, AV_LOG_ERROR,
               "Usage: ladspa=soname:plugin[:c0=VAL:c1=VAL:...]\n");
        return AVERROR(EINVAL);
    }

    // Load the plugin library
    dlerror();
    if (soname[0] == '/' || soname[0] == '.') {
        // argument is a path
        so = dlopen(soname, RTLD_NOW);
    } else {
        // argument is a shared object name
        char *paths = av_strdup(getenv("LADSPA_PATH"));
        char *ptr2;
        arg = strtok_r(paths, ":", &ptr2);
        while ((arg = strtok_r(NULL, ":", &ptr2)) && !so)
            so = try_load(arg, soname);
        if (!so) so = try_load("/usr/lib/ladspa", soname);
        if (!so) so = try_load("/usr/local/lib/ladspa", soname);
        av_free(paths);
    }
    if (!so) {
        av_log(ctx, AV_LOG_ERROR, "Could not load '%s.so'\n", soname);
        return AVERROR(EINVAL);
    }
    ladspa_desc_fn = dlsym(so, "ladspa_descriptor");
    if (!ladspa_desc_fn) {
        av_log(ctx, AV_LOG_ERROR, "Loading '%s' failed: %s\n",
               soname, dlerror());
        return AVERROR(EINVAL);
    }

    // Find the requested plugin, or list plugins
    arg = strtok_r(NULL, ":", &ptr);
    if (!arg) {
        av_log(ctx, AV_LOG_INFO,
               "The '%s' library contains the following plugins:\n", soname);
        for (i = 0; ladspa->desc = ladspa_desc_fn(i); i++)
            av_log(NULL, AV_LOG_INFO, "%s: %s\n",
                   ladspa->desc->Label, ladspa->desc->Name);
        return AVERROR(EINVAL);
    } else {
        for (i = 0; ladspa->desc = ladspa_desc_fn(i); i++)
            if (!strcmp(arg, ladspa->desc->Label)) break;
        if (!ladspa->desc) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unable to find '%s' in the '%s' bundle. "
                   "Use -af ladspa=%s for a list of plugins.\n",
                   arg, soname, soname);
            return AVERROR(EINVAL);
        }
    }

    // Alloc the port maps. We are being lazy and over-allocing
    ladspa->ctl_ports_map =
        av_malloc(ladspa->desc->PortCount * sizeof(*ladspa->ctl_ports_map));
    ladspa->in_ports_map  =
        av_malloc(ladspa->desc->PortCount * sizeof(*ladspa->ctl_ports_map));
    ladspa->out_ports_map =
        av_malloc(ladspa->desc->PortCount * sizeof(*ladspa->ctl_ports_map));
    // The control ports' values
    ladspa->ctl_values =
        av_mallocz(ladspa->desc->PortCount * sizeof(*ladspa->ctl_values));
    ladspa->ctl_needs_value =
        av_mallocz(ladspa->desc->PortCount * sizeof(*ladspa->ctl_needs_value));
    if (!ladspa->ctl_ports_map || !ladspa->in_ports_map || !ladspa->out_ports_map ||
        !ladspa->ctl_values    || !ladspa->ctl_needs_value) {
        av_log(ctx, AV_LOG_ERROR, "Coult not allocate memory.\n");
        return AVERROR(ENOMEM);
    }

    // Fill the maps and give the controls a default value
    for (i = 0; i < ladspa->desc->PortCount; i++) {
        if (LADSPA_IS_PORT_AUDIO(ladspa->desc->PortDescriptors[i])) {
            if (LADSPA_IS_PORT_INPUT(ladspa->desc->PortDescriptors[i]))
                ladspa->in_ports_map[ladspa->nb_ins++] = i;
            if (LADSPA_IS_PORT_OUTPUT(ladspa->desc->PortDescriptors[i]))
                ladspa->out_ports_map[ladspa->nb_outs++] = i;
        } else if (LADSPA_IS_PORT_INPUT(ladspa->desc->PortDescriptors[i])) {
            ladspa->ctl_ports_map[ladspa->nb_ctls] = i;
            if (!LADSPA_IS_HINT_HAS_DEFAULT(ladspa->desc->PortRangeHints[i].HintDescriptor))
                ladspa->ctl_needs_value[ladspa->nb_ctls] = 1;
            else
                set_default_ctl_value(ladspa, ladspa->nb_ctls);
            ladspa->nb_ctls++;
        }
    }

    arg = strtok_r(NULL, ":", &ptr);
    // List Control Ports if ":help" is specified
    if (arg && !strcmp(arg, "help")) {
        if (ladspa->nb_ctls) {
            av_log(ctx, AV_LOG_INFO,
                   "The '%s' plugin has the following controls:\n",
                   ladspa->desc->Label);
            for (i = 0; i < ladspa->nb_ctls; i++)
                print_ctl_info(NULL, AV_LOG_INFO, ladspa, i);
        } else {
            av_log(ctx, AV_LOG_INFO,
                   "The '%s' plugin does not have any controls.\n",
                   ladspa->desc->Label);
        }
        return AVERROR(EINVAL);
    }

    // Deal with source filters
    if (!ladspa->nb_ins) {
        if (arg && !strncmp(arg, "rate", 4)) {
            arg = strchr(arg, '=') + 1;
            if (ff_parse_sample_rate(&ladspa->sample_rate, arg, ctx) < 0)
                return AVERROR(EINVAL);
            arg = strtok_r(NULL, ":", &ptr);
        } else {
            ladspa->sample_rate = 44100;
        }
    }

    // We don't support sinks (does ladspa even have those?) or filters with
    // unqeual inputs and outputs (FIXME?)
    if (!ladspa->nb_outs || (ladspa->nb_ins && ladspa->nb_outs != ladspa->nb_ins)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported plugin.\n");
        return AVERROR(EINVAL);
    }

    // Parse control parameters
    i = 0;
    do {
        arg1 = strchr(arg, '=');
        *arg1++ = '\0';
        i = strtol(arg+1, &tail, 10);
        if (!arg1 || arg[0] != 'c' || *tail ||
            i < 0 || i >= ladspa->nb_ctls) {
            av_log(ctx, AV_LOG_ERROR,
                   "Bad control '%s'. Use -af ladspa=%s:%s:help "
                   "for a list of controls\n",
                   arg, soname, ladspa->desc->Label);
            return AVERROR(EINVAL);
        }
        //FIXME check against hints or allow out of bounds values??
        ladspa->ctl_values[i] = strtod(arg1, &tail);
        ladspa->ctl_needs_value[i] = 0;
    } while (arg = strtok_r(NULL, ":", &ptr));

    // Check if any controls are not set
    for (i = 0; i < ladspa->nb_ctls; i++) {
        if (ladspa->ctl_needs_value[i]) {
            av_log(ctx, AV_LOG_ERROR,
                   "Control c%i must be set.\n", i);
            print_ctl_info(ctx, AV_LOG_ERROR, ladspa, i);
            return AVERROR(EINVAL);
        }
    }
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    LADSPAContext *ladspa = ctx->priv;

    avfilter_add_format(&formats, AV_SAMPLE_FMT_FLT);
    if (!formats)
        return AVERROR(ENOMEM);
    avfilter_set_common_sample_formats(ctx, formats);

    if (ladspa->nb_ins) {
        if (ladspa->nb_ins == 1) {
            // We will instantiate multiple instances, one over each channel
            formats = avfilter_all_channel_layouts();
            if (!formats)
                return AVERROR(ENOMEM);
        } else {
            formats = NULL;
            avfilter_add_format(&formats,
                avcodec_guess_channel_layout(ladspa->nb_ins, 0, NULL));
        }
    } else {
        // Source plugin
        formats = NULL;
        avfilter_add_format(&formats,
            avcodec_guess_channel_layout(ladspa->nb_outs, 0, NULL));
    }
    avfilter_set_common_channel_layouts(ctx, formats);

    formats = NULL;
    avfilter_add_format(&formats, AVFILTER_PLANAR);
    if (!formats)
        return AVERROR(ENOMEM);
    avfilter_set_common_packing_formats(ctx, formats);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    LADSPAContext *ladspa = outlink->src->priv;
    int i, j;

    // Instantiate the plugin and connect its input control ports
    if (ladspa->nb_ins == 1)
        ladspa->nb_handles =
            av_get_channel_layout_nb_channels(
                outlink->src->inputs[0]->channel_layout);
    else
        ladspa->nb_handles = 1;
    
    if (ladspa->nb_ins)
        ladspa->sample_rate   = outlink->src->inputs[0]->sample_rate;
    else
        ladspa->outsamplesref = avfilter_get_audio_buffer(
                                    outlink,
                                    AV_PERM_WRITE | AV_PERM_REUSE2,
                                    AV_SAMPLE_FMT_FLT,
                                    LADSPA_SRC_NB_SAMPLES,
                                    outlink->channel_layout,
                                    AVFILTER_PLANAR);
    for (i = 0; i < ladspa->nb_handles; i++) {
        ladspa->handles[i] =
            ladspa->desc->instantiate(ladspa->desc, ladspa->sample_rate);
        if (!ladspa->handles[i]) {
            av_log(outlink->src, AV_LOG_ERROR, "Could not instantiate plugin.\n");
            return AVERROR(EINVAL);
        }

        // Connect the input control ports
        for (j = 0; j < ladspa->nb_ctls; j++)
            ladspa->desc->connect_port(ladspa->handles[i],
                                       ladspa->ctl_ports_map[j],
                                       ladspa->ctl_values+j);
        // Connect the output control ports to a dummy output
        for (j = 0; j < ladspa->desc->PortCount; j++)
            if (LADSPA_IS_PORT_CONTROL(ladspa->desc->PortDescriptors[j]) &&
                LADSPA_IS_PORT_OUTPUT(ladspa->desc->PortDescriptors[j]))
                ladspa->desc->connect_port(ladspa->handles[i],
                                           i, &ladspa->out_ctl_value);

        // Connect the output ports if this is a source plugin
        if (!ladspa->nb_ins) {
            for (j = 0; j < ladspa->nb_outs; j++)
                ladspa->desc->connect_port(ladspa->handles[i],
                                           ladspa->out_ports_map[j],
                                           (LADSPA_Data*)ladspa->outsamplesref->data[j]);
        }

        if (ladspa->desc->activate)
            ladspa->desc->activate(ladspa->handles[i]);
    }

    outlink->sample_rate = ladspa->sample_rate;

    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    LADSPAContext *ladspa = outlink->src->priv;

    ladspa->desc->run(ladspa->handles[0], LADSPA_SRC_NB_SAMPLES);
    avfilter_filter_samples(outlink,
        avfilter_ref_buffer(ladspa->outsamplesref, ~0));
    return 0;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamplesref)
{
    //FIXME check LADSPA_PROPERTY_INPLACE_BROKEN
    LADSPAContext *ladspa = inlink->dst->priv;
    int i;
    if (ladspa->nb_ins == 1) {
        for (i = 0; i < ladspa->nb_handles; i++) {
            ladspa->desc->connect_port(ladspa->handles[i],
                                       ladspa->in_ports_map[0],
                                       (LADSPA_Data*)insamplesref->data[i]);
            ladspa->desc->connect_port(ladspa->handles[i],
                                       ladspa->out_ports_map[0],
                                       (LADSPA_Data*)insamplesref->data[i]);
            ladspa->desc->run(ladspa->handles[i], insamplesref->audio->nb_samples);
        }
    } else {
        for (i = 0; i < ladspa->nb_outs; i++) {
            ladspa->desc->connect_port(ladspa->handles[0],
                                       ladspa->in_ports_map[i],
                                       (LADSPA_Data*)insamplesref->data[i]);
            ladspa->desc->connect_port(ladspa->handles[0],
                                       ladspa->out_ports_map[i],
                                       (LADSPA_Data*)insamplesref->data[i]);
        }
        ladspa->desc->run(ladspa->handles[0], insamplesref->audio->nb_samples);
    }
    avfilter_filter_samples(inlink->dst->outputs[0], insamplesref);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    LADSPAContext *ladspa = ctx->priv;
    int i;
    for (i = 0; i < ladspa->nb_handles; i++) {
        if (ladspa->desc->deactivate)
            ladspa->desc->deactivate(ladspa->handles[i]);
        if (ladspa->desc->cleanup)
            ladspa->desc->cleanup(ladspa->handles[i]);
    }
    av_freep(&ladspa->ctl_ports_map);
    av_freep(&ladspa->ctl_needs_value);
    av_freep(&ladspa->in_ports_map);
    av_freep(&ladspa->out_ports_map);
    av_freep(&ladspa->ctl_values);
}

AVFilter avfilter_af_ladspa = {
    .name          = "ladspa",
    .description   = NULL_IF_CONFIG_SMALL("Apply a LADSPA effect."),
    .priv_size     = sizeof(LADSPAContext),
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

AVFilter avfilter_asrc_ladspa_src = {
    .name          = "ladspa_src",
    .description   = NULL_IF_CONFIG_SMALL("Apply a LADSPA effect."),
    .priv_size     = sizeof(LADSPAContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name = NULL}},

    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_AUDIO,
                                    .config_props    = config_output,
                                    .request_frame   = request_frame, },
                                  { .name = NULL}},
};
