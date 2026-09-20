#ifndef MP_ADAPTIVE_MEDIA_SCHEDULE_H
#define MP_ADAPTIVE_MEDIA_SCHEDULE_H

#include "audio/out/media_timeline.h"

enum mp_media_schedule_status {
    MP_MEDIA_SCHEDULE_PENDING,
    MP_MEDIA_SCHEDULE_STALE,
    MP_MEDIA_SCHEDULE_INVALID,
    MP_MEDIA_SCHEDULE_MAPPED,
};

struct mp_media_schedule {
    enum mp_media_schedule_status status;
    double mapped_wall, target_wall, lateness, audio_pts, av_difference;
};

static inline struct mp_media_schedule mp_media_schedule(
    const struct mp_media_timeline *timeline, uint64_t epoch,
    double video_pts, double audio_delay, double now)
{
    struct mp_media_schedule s = {0};
    if (epoch != timeline->epoch) {
        s.status = MP_MEDIA_SCHEDULE_STALE;
        return s;
    }
    if (!isfinite(video_pts) || !isfinite(audio_delay) || !isfinite(now)) {
        s.status = MP_MEDIA_SCHEDULE_INVALID;
        return s;
    }
    if (!mp_media_timeline_wall_at_media(timeline, epoch, video_pts - audio_delay,
                                         &s.mapped_wall))
        return s;
    s.target_wall = fmax(now, s.mapped_wall);
    s.lateness = s.target_wall - s.mapped_wall;
    if (!mp_media_timeline_media_at_wall(timeline, epoch, s.target_wall, &s.audio_pts)) {
        s.status = MP_MEDIA_SCHEDULE_INVALID;
        return s;
    }
    s.av_difference = s.audio_pts - video_pts + audio_delay;
    s.status = MP_MEDIA_SCHEDULE_MAPPED;
    return s;
}

#endif
