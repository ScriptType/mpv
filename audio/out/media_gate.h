#ifndef MP_AUDIO_MEDIA_GATE_H
#define MP_AUDIO_MEDIA_GATE_H

#include "media_timeline.h"

// Copy offsets belong to the full callback, including its unconsumed silence.
static inline struct mp_media_admission mp_media_gate_admit(
    struct mp_media_timeline *timeline, uint64_t epoch, struct mp_media_copy copy,
    double ceiling, int64_t callback_start_ns, int64_t callback_end_ns,
    int callback_samples, int offset, double *wall_cursor)
{
    if (callback_start_ns >= callback_end_ns || callback_samples <= 0 || offset < 0 ||
        offset >= callback_samples || copy.available > callback_samples - offset)
        return (struct mp_media_admission){.status = MP_MEDIA_INVALID};
    // The cursor belongs to this callback and advances only after reservation.
    // A new callback always derives its own slot start and retains overlap checks.
    double callback_end = callback_end_ns / 1e9;
    copy.wall_start = offset ? *wall_cursor : callback_start_ns / 1e9;
    if (!isfinite(copy.wall_start) || callback_end <= copy.wall_start)
        return (struct mp_media_admission){.status = MP_MEDIA_INVALID};
    bool has_previous = timeline->count || timeline->has_retired;
    double previous = timeline->retired_media_end;
    if (timeline->count) {
        previous = timeline->segments[(timeline->head + timeline->count - 1) %
            MP_MEDIA_TIMELINE_CAPACITY].media_end;
    }
    // Repeated aframe sample skips and the next decoded PTS can differ by one
    // representable double. This does not permit a sample-sized discontinuity.
    if (has_previous && copy.media_start < previous &&
        nextafter(copy.media_start, INFINITY) == previous)
        copy.media_start = previous;
    struct mp_media_admission admitted = mp_media_timeline_admit(timeline, epoch, copy, ceiling);
    if (admitted.status == MP_MEDIA_ADMITTED) {
        struct mp_media_segment *last = &timeline->segments[
            (timeline->head + timeline->count - 1) % MP_MEDIA_TIMELINE_CAPACITY];
        if (admitted.samples == callback_samples - offset)
            last->wall_end = callback_end;
        *wall_cursor = last->wall_end;
    }
    return admitted;
}

#endif
