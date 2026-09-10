#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "demux/lavf_timing.h"

static AVStream *track(AVFormatContext *format, enum AVMediaType type,
                       int64_t start, int64_t duration, AVRational timebase)
{
    AVStream *stream = avformat_new_stream(format, NULL);
    if (!stream) abort();
    stream->codecpar->codec_type = type;
    stream->start_time = start;
    stream->duration = duration;
    stream->time_base = timebase;
    return stream;
}

static void check(const char *name, AVFormatContext *format, double existing,
                  int64_t expected_start, double expected_duration)
{
    struct mp_lavf_timing result = mp_lavf_resolve_timing(format, existing);
    if (result.start_time != expected_start ||
        fabs(result.duration - expected_duration) > 1e-9) {
        fprintf(stderr, "%s: start=%lld duration=%.12f\n", name,
                (long long)result.start_time, result.duration);
        exit(1);
    }
}

int main(void)
{
    AVFormatContext *format = avformat_alloc_context();
    if (!format) abort();
    format->start_time = AV_NOPTS_VALUE;
    check("missing timing", format, -1, AV_NOPTS_VALUE, -1);
    AVStream *video = track(format, AVMEDIA_TYPE_VIDEO, 240000, 2362360,
                            (AVRational){1, 24000});
    AVStream *audio = track(format, AVMEDIA_TYPE_AUDIO, 477888, 4726832,
                            (AVRational){1, 48000});
    check("Apple P5 audio lead", format, 98.475666667, 9956000, 98.475666667);
    video->start_time = 241001;
    check("Apple HDR10+ later video end", format, 98.475666667,
          9956000, 98.517375);
    check("preserve longer estimate", format, 120, 9956000, 120);
    format->start_time = 9000000;
    check("preserve aggregate origin", format, 98.475666667,
          9000000, 99.473375);
    format->start_time = AV_NOPTS_VALUE;

    AVStream *cover = track(format, AVMEDIA_TYPE_VIDEO, -100, 10000,
                            (AVRational){1, 1});
    cover->disposition = AV_DISPOSITION_ATTACHED_PIC;
    track(format, AVMEDIA_TYPE_SUBTITLE, -200, 20000, (AVRational){1, 1});
    track(format, AVMEDIA_TYPE_DATA, -300, 30000, (AVRational){1, 1});
    check("ignore cover subtitle data", format, 98.475666667, 9956000, 98.517375);

    video->start_time = 0; video->duration = 30; video->time_base = (AVRational){1, 1};
    audio->start_time = AV_NOPTS_VALUE;
    check("zero start unchanged", format, 30, 0, 30);
    video->duration = 1; video->time_base = (AVRational){1, 6};
    double precise = 1.0 / 6;
    check("fractional zero-origin duration", format, precise, 0, precise);
    if (mp_lavf_resolve_timing(format, precise).duration != precise)
        abort(); // preserve the exact existing Double, not only a tolerance
    video->duration = 166668; video->time_base = AV_TIME_BASE_Q;
    check("endpoint beyond enclosing microsecond", format, precise, 0, 0.166668);
    video->time_base = (AVRational){1, 1};
    video->start_time = -1; video->duration = 3;
    check("negative origin", format, 3, -1000000, 3);
    video->time_base.num = 0;
    check("invalid numerator", format, 7, AV_NOPTS_VALUE, 7);
    video->time_base = (AVRational){1, 0};
    check("invalid denominator", format, 7, AV_NOPTS_VALUE, 7);
    video->time_base = (AVRational){1, 1}; video->start_time = 5;
    video->duration = AV_NOPTS_VALUE;
    check("unknown duration", format, 7, 5000000, 7);
    video->duration = 0;
    check("zero duration", format, 7, 5000000, 7);
    video->duration = -1;
    check("negative duration", format, 7, 5000000, 7);
    video->start_time = INT64_MAX - 2; video->duration = 10;
    video->time_base = AV_TIME_BASE_Q;
    check("end addition overflow", format, 7, INT64_MAX - 2, 7);
    video->start_time = INT64_MAX; video->time_base = (AVRational){INT32_MAX, 1};
    check("rescale overflow", format, 7, AV_NOPTS_VALUE, 7);
    video->time_base = AV_TIME_BASE_Q; video->start_time = INT64_MIN + 1;
    video->duration = 1; format->start_time = INT64_MIN + 1;
    audio->time_base = AV_TIME_BASE_Q; audio->start_time = INT64_MAX - 2;
    audio->duration = 1;
    check("span subtraction overflow", format, 7, INT64_MIN + 1, 7);
    avformat_free_context(format);
    puts("18 header-timing checks passed");
}
