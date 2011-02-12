/*
 * Copyright (c) 2010 by S.N. Hemanth Meenakshisundaram
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

#ifndef AVFILTER_ASRC_ABUFFER_H
#define AVFILTER_ASRC_ABUFFER_H

#include "avfilter.h"
#include "libavutil/fifo.h"

/**
 * @file
 * memory buffer source filter for audio
 */

/**
 * The Buffer Source Context
 */
typedef struct {
    unsigned int sample_fmt;  ///< initial sample format indicated by client
    int64_t channel_layout;   ///< initial channel layout indicated by client
    int sample_rate;
    AVFifoBuffer *fifo;       ///< FIFO buffer of audio frame pointers
} ABufferSourceContext;

/**
 * Queue an audio buffer to the audio buffer source.
 *
 * @param abufsrc audio source buffer context
 * @param data pointers to the samples planes
 * @param linesize linesizes of each audio buffer plane
 * @param nb_samples number of samples per channel
 * @param sample_fmt sample format of the audio data
 * @param ch_layout channel layout of the audio data
 * @param planar flag to indicate if audio data is planar or packed
 * @param pts presentation timestamp of the audio buffer
 */
int av_asrc_buffer_add_samples(AVFilterContext *abufsrc,
                               uint8_t *data[8], int linesize[8],
                               int nb_samples,
                               int sample_fmt, int64_t ch_layout, int planar,
                               int64_t pts);

/**
 * Queue an audio buffer to the audio buffer source.
 *
 * This is similar to av_asrc_buffer_add_samples(), but the samples
 * are stored in a buffer with known size.
 *
 * @param abufsrc audio source buffer context
 * @param buf pointer to the samples data, packed is assumed
 * @param size the size in bytes of the buffer, it must contain an
 * integer number of samples
 * @param sample_fmt sample format of the audio data
 * @param ch_layout channel layout of the audio data
 * @param pts presentation timestamp of the audio buffer
 */
int av_asrc_buffer_add_buffer(AVFilterContext *abufsrc,
                              uint8_t *buf, int buf_size,
                              int sample_fmt, int64_t ch_layout,
                              int64_t pts);

#endif /* AVFILTER_ASRC_ABUFFER_H */
