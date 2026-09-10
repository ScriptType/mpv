/* Header timing fallback for libavformat formats which skip stream probing.
 * Copyright (C) 2026 mpv developers
 * License: LGPL-2.1-or-later
 */
#ifndef MPV_LAVF_TIMING_H
#define MPV_LAVF_TIMING_H

#include <math.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

struct mp_lavf_timing {
    int64_t start_time; // AV_TIME_BASE units, or AV_NOPTS_VALUE
    double duration;
};

static inline struct mp_lavf_timing
mp_lavf_resolve_timing(const AVFormatContext *format, double duration)
{
    int64_t earliest = AV_NOPTS_VALUE, latest_end = AV_NOPTS_VALUE;
    for (unsigned n = 0; n < format->nb_streams; n++) {
        const AVStream *stream = format->streams[n];
        enum AVMediaType type = stream->codecpar->codec_type;
        // Header-only inference must not let a cover picture or subtitle/data
        // track invent the playback origin or extend the audiovisual program.
        if ((type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
            stream->start_time == AV_NOPTS_VALUE ||
            stream->time_base.num <= 0 || stream->time_base.den <= 0)
            continue;
        int64_t start = av_rescale_q(stream->start_time, stream->time_base,
                                     AV_TIME_BASE_Q);
        if (start == AV_NOPTS_VALUE) // rescale overflow
            continue;
        if (earliest == AV_NOPTS_VALUE || start < earliest)
            earliest = start;
        int64_t end;
        if (stream->duration <= 0 ||
            __builtin_add_overflow(stream->start_time, stream->duration, &end))
            continue;
        // Add in the original stream tick domain, then convert the end once.
        end = av_rescale_q(end, stream->time_base, AV_TIME_BASE_Q);
        if (end != AV_NOPTS_VALUE &&
            (latest_end == AV_NOPTS_VALUE || end > latest_end))
            latest_end = end;
    }
    struct mp_lavf_timing result = {
        .start_time = format->start_time != AV_NOPTS_VALUE
                    ? format->start_time : earliest,
        .duration = duration,
    };
    int64_t span;
    if (result.start_time != AV_NOPTS_VALUE && latest_end != AV_NOPTS_VALUE &&
        !__builtin_sub_overflow(latest_end, result.start_time, &span) && span > 0) {
        double known_duration = span / (double)AV_TIME_BASE;
        // The endpoint is known only to AV_TIME_BASE precision. Preserve an
        // existing precise track duration inside its enclosing microsecond;
        // rounding a boundary must not create a spurious sub-microsecond tail.
        if (span > ceil(result.duration * AV_TIME_BASE))
            result.duration = known_duration;
    }
    return result;
}
#endif
