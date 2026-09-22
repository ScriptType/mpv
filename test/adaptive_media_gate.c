#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/out/media_gate.h"
#include "player/adaptive_media_schedule.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void callback_offsets_and_silent_suffix(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 1);
    const int64_t start = INT64_C(98000000000), end = INT64_C(100000000000);
    double cursor = 0;
    struct mp_media_copy copy = {
        .available = 256, .effective_rate = 1024, .output_rate = 1024,
    };
    struct mp_media_admission a = mp_media_gate_admit(&t, 1, copy, .125, start, end, 2048, 0, &cursor);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 128);
    CHECK(t.segments[0].wall_start == 98 && t.segments[0].wall_end == 98.125);
    copy.media_start = .125;
    a = mp_media_gate_admit(&t, 1, copy, .375, start, end, 2048, 128, &cursor);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 256);
    CHECK(t.segments[1].wall_start == 98.125 && t.segments[1].wall_end == 98.375);
    double media = -1;
    CHECK(mp_media_timeline_media_at_wall(&t, 1, 99, &media) && media == .375);
    copy.media_start = .375;
    struct mp_media_timeline before = t;
    a = mp_media_gate_admit(&t, 1, copy, .375, start, end, 2048, 384, &cursor);
    CHECK(a.status == MP_MEDIA_NO_CREDIT && !a.samples);
    CHECK(!memcmp(&before, &t, sizeof(t)));
    a = mp_media_gate_admit(&t, 1, copy, 1, start, end, 2048, 2048, &cursor);
    CHECK(a.status == MP_MEDIA_INVALID && !a.samples);
    CHECK(!memcmp(&before, &t, sizeof(t)));
    a = mp_media_gate_admit(&t, 0, copy, 1, start, end, 2048, 384, &cursor);
    CHECK(a.status == MP_MEDIA_STALE && !a.samples);
    CHECK(!memcmp(&before, &t, sizeof(t)));
}

static void aframe_skip_rounding_is_one_ulp_only(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 2);
    const int64_t start = INT64_C(1989333334), end = INT64_C(2000000000);
    double cursor = 0;
    struct mp_media_copy copy = {
        .available = 512, .effective_rate = 48000, .output_rate = 48000,
        .media_start = .064, .wall_start = 0,
    };
    CHECK(mp_media_timeline_admit(&t, 2, copy, 1).samples == 512);
    copy.media_start += 512.0 / 48000;
    copy.wall_start = .5;
    CHECK(mp_media_timeline_admit(&t, 2, copy, 1).samples == 512);
    double skipped_pts = copy.media_start + 512.0 / 48000;
    double decoded_pts = 4096.0 / 48000;
    CHECK(nextafter(decoded_pts, INFINITY) == skipped_pts);
    struct mp_media_timeline before = t;
    copy.media_start = nextafter(decoded_pts, -INFINITY);
    struct mp_media_admission a = mp_media_gate_admit(&t, 2, copy, 1, start, end, 512, 0, &cursor);
    CHECK(a.status == MP_MEDIA_INVALID && !a.samples);
    CHECK(!memcmp(&before, &t, sizeof(t)));
    copy.media_start = decoded_pts;
    a = mp_media_gate_admit(&t, 2, copy, 1, start, end, 512, 0, &cursor);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 512);
    CHECK(t.segments[2].media_start == skipped_pts);

    // Terminal drain must not round-trip a calculated one-sample ceiling.
    copy.media_start = t.segments[2].media_end;
    copy.available = 1;
    a = mp_media_gate_admit(&t, 2, copy, DBL_MAX,
        INT64_C(2989333334), INT64_C(3000000000), 512, 0, &cursor);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 1);
}

static void integer_callback_bounds_and_new_callback_overlap(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 3);
    double cursor = 0;
    // ca_frames_to_ns truncates 512 / 48000 * 1e9 to this duration.
    const int64_t duration = 10666666;
    const int64_t end = INT64_C(100000000000);
    const int64_t start = end - duration;
    struct mp_media_copy copy = {
        .available = 256, .effective_rate = 48000, .output_rate = 48000,
    };
    CHECK(mp_media_gate_admit(&t, 3, copy, 1, start, end, 512, 0, &cursor).samples == 256);
    CHECK(t.segments[0].wall_start == start / 1e9);
    copy.media_start = 256.0 / 48000;
    double first_end = cursor;
    CHECK(mp_media_gate_admit(&t, 3, copy, 1, start, end, 512, 256, &cursor).samples == 256);
    CHECK(t.segments[1].wall_start == first_end);
    CHECK(t.segments[1].wall_end == 100 && cursor == 100);

    // Subtracting fractional sample duration invents an overlap of <1 ns.
    CHECK((end + duration) / 1e9 - 512.0 / 48000 < end / 1e9);
    copy.available = 512;
    int64_t next_start = end;
    for (int i = 0; i < 8; i++) {
        copy.media_start = t.segments[t.count - 1].media_end;
        CHECK(mp_media_gate_admit(&t, 3, copy, 1, next_start, next_start + duration,
                                  512, 0, &cursor).samples == 512);
        CHECK(t.segments[t.count - 1].wall_start == next_start / 1e9);
        CHECK(t.segments[t.count - 1].wall_end == (next_start + duration) / 1e9);
        next_start += duration;
    }

    // A true one-nanosecond overlap remains a transactional refusal.
    struct mp_media_timeline before = t;
    double before_cursor = cursor;
    copy.media_start = t.segments[t.count - 1].media_end;
    struct mp_media_admission a = mp_media_gate_admit(&t, 3, copy, 1,
        next_start - 1, next_start + duration - 1, 512, 0, &cursor);
    CHECK(a.status == MP_MEDIA_INVALID && !a.samples);
    CHECK(!memcmp(&before, &t, sizeof(t)) && cursor == before_cursor);
    a = mp_media_gate_admit(&t, 3, copy, 1, next_start, next_start, 512, 0, &cursor);
    CHECK(a.status == MP_MEDIA_INVALID && !a.samples);
    CHECK(!memcmp(&before, &t, sizeof(t)) && cursor == before_cursor);
}

static void mapped_deadlines_and_honest_lateness(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 7);
    struct mp_media_copy copy = {
        .available = 512, .effective_rate = 1024, .output_rate = 1024,
        .media_start = 0, .wall_start = 10,
    };
    CHECK(mp_media_timeline_admit(&t, 7, copy, .5).samples == 512);
    copy.media_start = .5;
    copy.wall_start = 12;
    CHECK(mp_media_timeline_admit(&t, 7, copy, 1).samples == 512);
    struct mp_media_schedule s = mp_media_schedule(&t, 7, .25, 0, 9);
    CHECK(s.status == MP_MEDIA_SCHEDULE_MAPPED);
    CHECK(s.mapped_wall == 10.25 && s.target_wall == 10.25);
    CHECK(s.lateness == 0 && s.audio_pts == .25 && s.av_difference == 0);
    s = mp_media_schedule(&t, 7, .25, 0, 10.375);
    CHECK(s.status == MP_MEDIA_SCHEDULE_MAPPED);
    CHECK(s.lateness == .125 && s.av_difference == .125);
    s = mp_media_schedule(&t, 7, .25, 0, 11);
    CHECK(s.status == MP_MEDIA_SCHEDULE_MAPPED);
    CHECK(s.audio_pts == .5 && s.av_difference == .25 && s.lateness == .75);
    s = mp_media_schedule(&t, 7, .75, .25, 11);
    CHECK(s.status == MP_MEDIA_SCHEDULE_MAPPED);
    CHECK(s.mapped_wall == 12 && s.audio_pts == .5 && s.av_difference == 0);
    s = mp_media_schedule(&t, 7, 1, 0, 11);
    CHECK(s.status == MP_MEDIA_SCHEDULE_PENDING);
    s = mp_media_schedule(&t, 6, .25, 0, 11);
    CHECK(s.status == MP_MEDIA_SCHEDULE_STALE);
    s = mp_media_schedule(&t, 7, .25, NAN, 11);
    CHECK(s.status == MP_MEDIA_SCHEDULE_INVALID);
}

int main(void)
{
    callback_offsets_and_silent_suffix();
    aframe_skip_rounding_is_one_ulp_only();
    integer_callback_bounds_and_new_callback_overlap();
    mapped_deadlines_and_honest_lateness();
    puts("4 Adaptive media gate groups passed (CPU boundaries only)");
    return 0;
}
