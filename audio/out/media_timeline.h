#ifndef MP_AUDIO_MEDIA_TIMELINE_H
#define MP_AUDIO_MEDIA_TIMELINE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define MP_MEDIA_TIMELINE_CAPACITY 64

struct mp_media_segment {
    int samples;
    double effective_rate, output_rate;
    double media_start, media_end;
    double wall_start, wall_end;
};

struct mp_media_timeline {
    uint64_t epoch;
    unsigned head, count;
    bool has_retired;
    double retired_media_end, retired_wall_end;
    struct mp_media_segment segments[MP_MEDIA_TIMELINE_CAPACITY];
};

struct mp_media_copy {
    int available;
    double effective_rate, output_rate;
    double media_start, wall_start;
};

enum mp_media_admission_status {
    MP_MEDIA_ADMITTED,
    MP_MEDIA_NO_CREDIT,
    MP_MEDIA_INVALID,
    MP_MEDIA_STALE,
    MP_MEDIA_FULL,
};

struct mp_media_admission {
    enum mp_media_admission_status status;
    int samples;
};

static inline void mp_media_timeline_reset(struct mp_media_timeline *t,
                                          uint64_t epoch)
{
    *t = (struct mp_media_timeline){.epoch = epoch};
}

static inline struct mp_media_admission mp_media_timeline_admit(
    struct mp_media_timeline *t, uint64_t epoch, struct mp_media_copy copy,
    double media_ceiling)
{
    if (epoch != t->epoch)
        return (struct mp_media_admission){.status = MP_MEDIA_STALE};
    if (copy.available <= 0 || !isfinite(copy.effective_rate) ||
        copy.effective_rate <= 0 || !isfinite(copy.output_rate) ||
        copy.output_rate <= 0 || !isfinite(copy.media_start) ||
        !isfinite(copy.wall_start) || !isfinite(media_ceiling))
        return (struct mp_media_admission){.status = MP_MEDIA_INVALID};

    double last_media = t->retired_media_end, last_wall = t->retired_wall_end;
    if (t->count) {
        const struct mp_media_segment *last = &t->segments[
            (t->head + t->count - 1) % MP_MEDIA_TIMELINE_CAPACITY];
        last_media = last->media_end;
        last_wall = last->wall_end;
    }
    if ((t->count || t->has_retired) &&
        (copy.media_start < last_media || copy.wall_start < last_wall))
        return (struct mp_media_admission){.status = MP_MEDIA_INVALID};

    long double credit = ((long double)media_ceiling - copy.media_start) *
                         copy.effective_rate;
    if (credit < 1)
        return (struct mp_media_admission){.status = MP_MEDIA_NO_CREDIT};
    int samples = credit >= copy.available ? copy.available : (int)floorl(credit);
    struct mp_media_segment s = {
        .samples = samples,
        .effective_rate = copy.effective_rate, .output_rate = copy.output_rate,
        .media_start = copy.media_start, .wall_start = copy.wall_start,
        .media_end = copy.media_start + samples / copy.effective_rate,
        .wall_end = copy.wall_start + samples / copy.output_rate,
    };
    if (!isfinite(s.media_end) || !isfinite(s.wall_end) ||
        s.media_end <= s.media_start || s.wall_end <= s.wall_start ||
        s.media_end > media_ceiling)
        return (struct mp_media_admission){.status = MP_MEDIA_INVALID};
    if (t->count == MP_MEDIA_TIMELINE_CAPACITY)
        return (struct mp_media_admission){.status = MP_MEDIA_FULL};

    t->segments[(t->head + t->count) % MP_MEDIA_TIMELINE_CAPACITY] = s;
    t->count++;
    return (struct mp_media_admission){.status = MP_MEDIA_ADMITTED, .samples = samples};
}

static inline bool mp_media_timeline_media_at_wall(
    const struct mp_media_timeline *t, uint64_t epoch, double wall, double *media)
{
    if (epoch != t->epoch || !isfinite(wall))
        return false;
    bool known = t->has_retired && wall >= t->retired_wall_end;
    double value = t->retired_media_end;
    for (unsigned i = 0; i < t->count; i++) {
        const struct mp_media_segment *s = &t->segments[
            (t->head + i) % MP_MEDIA_TIMELINE_CAPACITY];
        if (wall < s->wall_start)
            break;
        known = true;
        value = s->media_end;
        if (wall < s->wall_end) {
            double fraction = (wall - s->wall_start) / (s->wall_end - s->wall_start);
            value = s->media_start + fraction * (s->media_end - s->media_start);
            break;
        }
    }
    if (known)
        *media = value;
    return known;
}

static inline bool mp_media_timeline_wall_at_media(
    const struct mp_media_timeline *t, uint64_t epoch, double media, double *wall)
{
    if (epoch != t->epoch || !isfinite(media))
        return false;
    for (unsigned i = 0; i < t->count; i++) {
        const struct mp_media_segment *s = &t->segments[
            (t->head + i) % MP_MEDIA_TIMELINE_CAPACITY];
        if (media >= s->media_start && media < s->media_end) {
            double fraction = (media - s->media_start) / (s->media_end - s->media_start);
            *wall = s->wall_start + fraction * (s->wall_end - s->wall_start);
            return true;
        }
    }
    return false;
}

static inline bool mp_media_timeline_release(
    struct mp_media_timeline *t, uint64_t epoch, double played_wall,
    double released_media)
{
    if (epoch != t->epoch || !isfinite(played_wall) || !isfinite(released_media))
        return false;
    while (t->count) {
        const struct mp_media_segment *s = &t->segments[t->head];
        if (s->wall_end > played_wall || s->media_end > released_media)
            break;
        t->has_retired = true;
        t->retired_media_end = s->media_end;
        t->retired_wall_end = s->wall_end;
        t->head = (t->head + 1) % MP_MEDIA_TIMELINE_CAPACITY;
        t->count--;
    }
    return true;
}

#endif
