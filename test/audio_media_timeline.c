#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/out/media_timeline.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static struct mp_media_copy copy_at(double media, double wall, int samples)
{
    return (struct mp_media_copy){
        .available = samples, .effective_rate = 48000, .output_rate = 48000,
        .media_start = media, .wall_start = wall,
    };
}

static void check_media(const struct mp_media_timeline *t, double wall, double media)
{
    double value = -999;
    CHECK(mp_media_timeline_media_at_wall(t, t->epoch, wall, &value));
    CHECK(fabs(value - media) < 1e-12);
}

static void check_wall(const struct mp_media_timeline *t, double media, double wall)
{
    double value = -999;
    CHECK(mp_media_timeline_wall_at_media(t, t->epoch, media, &value));
    CHECK(fabs(value - wall) < 1e-12);
}

static void clipping_and_speed(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 7);
    struct mp_media_admission a = mp_media_timeline_admit(
        &t, 7, copy_at(2, 10, 48000), 2.25);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 12000);
    CHECK(t.segments[0].media_end == 2.25);
    CHECK(t.segments[0].wall_end == 10.25);
    check_media(&t, 10.125, 2.125);
    check_wall(&t, 2.125, 10.125);

    struct mp_media_copy fast = copy_at(2.25, 11, 24000);
    fast.effective_rate = 24000;
    a = mp_media_timeline_admit(&t, 7, fast, 2.75);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 12000);
    CHECK(t.segments[1].media_end == 2.75);
    CHECK(t.segments[1].wall_end == 11.25);
    check_media(&t, 11.125, 2.5);
    check_wall(&t, 2.5, 11.125);

    mp_media_timeline_reset(&t, 8);
    a = mp_media_timeline_admit(&t, 8, copy_at(0, 0, 48000), nextafter(0.5, 0));
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 23999);
    CHECK(t.segments[0].media_end <= nextafter(0.5, 0));
    mp_media_timeline_reset(&t, 9);
    a = mp_media_timeline_admit(&t, 9, copy_at(0, 0, 12000), 100);
    CHECK(a.status == MP_MEDIA_ADMITTED && a.samples == 12000);
}

static void gaps_and_half_open_boundaries(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 1);
    double value = -999;
    CHECK(!mp_media_timeline_media_at_wall(&t, 1, 10, &value));
    CHECK(!mp_media_timeline_wall_at_media(&t, 1, 0, &value));
    CHECK(value == -999);
    CHECK(mp_media_timeline_admit(&t, 1, copy_at(0, 10, 24000), 0.5).samples == 24000);
    CHECK(!mp_media_timeline_media_at_wall(&t, 1, 9, &value));
    CHECK(!mp_media_timeline_wall_at_media(&t, 1, 0.5, &value));
    CHECK(value == -999);
    check_media(&t, 10, 0);
    check_media(&t, 10.5, 0.5);
    check_media(&t, 100, 0.5);
    CHECK(mp_media_timeline_admit(&t, 1, copy_at(0.5, 12, 24000), 1).samples == 24000);
    check_media(&t, 11.9, 0.5);
    check_media(&t, 12, 0.5);
    check_wall(&t, 0.5, 12);
    CHECK(!mp_media_timeline_wall_at_media(&t, 1, 1, &value));
    CHECK(mp_media_timeline_admit(&t, 1, copy_at(2, 13, 24000), 2.5).samples == 24000);
    CHECK(!mp_media_timeline_wall_at_media(&t, 1, 1.5, &value));
    check_media(&t, 12.75, 1);
    check_media(&t, 13, 2);
}

static void rejected_admission(struct mp_media_timeline *t, uint64_t epoch,
                               struct mp_media_copy copy, double ceiling,
                               enum mp_media_admission_status status)
{
    unsigned char before[sizeof(*t)];
    memcpy(before, t, sizeof(*t));
    struct mp_media_admission a = mp_media_timeline_admit(t, epoch, copy, ceiling);
    CHECK(a.status == status && a.samples == 0);
    CHECK(memcmp(before, t, sizeof(*t)) == 0);
}

static void rejection_is_transactional(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 3);
    CHECK(mp_media_timeline_admit(&t, 3, copy_at(0, 10, 24000), 0.5).samples == 24000);
    struct mp_media_copy good = copy_at(0.5, 11, 24000);
    rejected_admission(&t, 2, good, 1, MP_MEDIA_STALE);
    rejected_admission(&t, 3, good, 0.5, MP_MEDIA_NO_CREDIT);
    rejected_admission(&t, 3, good, 0.4, MP_MEDIA_NO_CREDIT);
    rejected_admission(&t, 3, good, 0.500001, MP_MEDIA_NO_CREDIT);
    rejected_admission(&t, 3, copy_at(0.25, 11, 24000), 1, MP_MEDIA_INVALID);
    rejected_admission(&t, 3, copy_at(0.5, 10.25, 24000), 1, MP_MEDIA_INVALID);
    rejected_admission(&t, 3, copy_at(0.5, 11, 0), 1, MP_MEDIA_INVALID);
    rejected_admission(&t, 3, copy_at(0.5, 11, -1), 1, MP_MEDIA_INVALID);
    double invalid[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        struct mp_media_copy c = good;
        c.media_start = invalid[i];
        rejected_admission(&t, 3, c, 1, MP_MEDIA_INVALID);
        c = good;
        c.wall_start = invalid[i];
        rejected_admission(&t, 3, c, 1, MP_MEDIA_INVALID);
        rejected_admission(&t, 3, good, invalid[i], MP_MEDIA_INVALID);
    }
    double bad_rates[] = {0, -1, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(bad_rates) / sizeof(bad_rates[0]); i++) {
        struct mp_media_copy c = good;
        c.effective_rate = bad_rates[i];
        rejected_admission(&t, 3, c, 1, MP_MEDIA_INVALID);
        c = good;
        c.output_rate = bad_rates[i];
        rejected_admission(&t, 3, c, 1, MP_MEDIA_INVALID);
    }
    struct mp_media_copy c = good;
    c.output_rate = DBL_MIN;
    rejected_admission(&t, 3, c, 1, MP_MEDIA_INVALID);
    c = good;
    c.wall_start = DBL_MAX;
    rejected_admission(&t, 3, c, 1, MP_MEDIA_INVALID);

    unsigned char before[sizeof(t)];
    memcpy(before, &t, sizeof(t));
    double value = -999;
    CHECK(!mp_media_timeline_media_at_wall(&t, 2, 10.25, &value));
    CHECK(!mp_media_timeline_wall_at_media(&t, 2, 0.25, &value));
    CHECK(!mp_media_timeline_media_at_wall(&t, 3, NAN, &value));
    CHECK(!mp_media_timeline_wall_at_media(&t, 3, INFINITY, &value));
    CHECK(!mp_media_timeline_release(&t, 2, 100, 100));
    CHECK(!mp_media_timeline_release(&t, 3, NAN, 100));
    CHECK(!mp_media_timeline_release(&t, 3, 100, INFINITY));
    CHECK(value == -999);
    CHECK(memcmp(before, &t, sizeof(t)) == 0);
}

static void independent_release_and_anchor(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 1);
    CHECK(mp_media_timeline_admit(&t, 1, copy_at(0, 10, 24000), 0.5).samples == 24000);
    CHECK(mp_media_timeline_admit(&t, 1, copy_at(0.5, 12, 24000), 1).samples == 24000);
    CHECK(mp_media_timeline_release(&t, 1, 11, 0.25));
    CHECK(t.count == 2);
    check_wall(&t, 0.25, 10.25);
    CHECK(mp_media_timeline_release(&t, 1, 10.25, 0.5));
    CHECK(t.count == 2);
    CHECK(mp_media_timeline_release(&t, 1, 10.5, 0.5));
    CHECK(t.count == 1 && t.has_retired);
    double value = -999;
    CHECK(!mp_media_timeline_media_at_wall(&t, 1, 10.25, &value));
    CHECK(!mp_media_timeline_wall_at_media(&t, 1, 0.25, &value));
    check_media(&t, 10.5, 0.5);
    check_media(&t, 11.5, 0.5);
    check_wall(&t, 0.5, 12);
    CHECK(mp_media_timeline_release(&t, 1, 12.5, 1));
    CHECK(t.count == 0);
    check_media(&t, 13, 1);
    rejected_admission(&t, 1, copy_at(0.75, 13, 24000), 2, MP_MEDIA_INVALID);
    rejected_admission(&t, 1, copy_at(1, 12.25, 24000), 2, MP_MEDIA_INVALID);
    CHECK(mp_media_timeline_admit(&t, 1, copy_at(1, 14, 24000), 1.5).samples == 24000);
    check_media(&t, 13.5, 1);
    check_wall(&t, 1, 14);
    mp_media_timeline_reset(&t, 2);
    CHECK(t.count == 0 && !t.has_retired);
    CHECK(!mp_media_timeline_media_at_wall(&t, 2, 100, &value));
    CHECK(!mp_media_timeline_wall_at_media(&t, 2, 1, &value));
    CHECK(mp_media_timeline_admit(&t, 2, copy_at(0, 0, 24000), 0.5).samples == 24000);
}

static void capacity_and_wraparound(void)
{
    struct mp_media_timeline t;
    mp_media_timeline_reset(&t, 5);
    for (int i = 0; i < 64; i++) {
        CHECK(mp_media_timeline_admit(&t, 5, copy_at(i, 100 + i, 48000), i + 1).samples == 48000);
    }
    CHECK(t.count == 64);
    rejected_admission(&t, 5, copy_at(64, 164, 48000), 65, MP_MEDIA_FULL);
    CHECK(mp_media_timeline_release(&t, 5, 132, 32));
    CHECK(t.count == 32 && t.head == 32);
    for (int i = 64; i < 96; i++) {
        CHECK(mp_media_timeline_admit(&t, 5, copy_at(i, 100 + i, 48000), i + 1).samples == 48000);
    }
    CHECK(t.count == 64);
    for (int i = 32; i < 96; i++) {
        check_media(&t, 100.5 + i, i + 0.5);
        check_wall(&t, i + 0.5, 100.5 + i);
    }
    CHECK(mp_media_timeline_release(&t, 5, 196, 96));
    CHECK(t.count == 0 && t.head == 32);
    check_media(&t, 200, 96);
    CHECK(mp_media_timeline_admit(&t, 5, copy_at(96, 201, 48000), 97).samples == 48000);
    check_wall(&t, 96.5, 201.5);
}

int main(void)
{
    clipping_and_speed();
    gaps_and_half_open_boundaries();
    rejection_is_transactional();
    independent_release_and_anchor();
    capacity_and_wraparound();
    puts("5 submitted-media timeline groups passed (CPU contract only)");
    return 0;
}
