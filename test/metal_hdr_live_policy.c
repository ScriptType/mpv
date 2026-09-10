#include <float.h>
#include <stdio.h>

#include "video/filter/metal_hdr_live_policy.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static const struct mp_hdr_live_settings neural = {
    .have_model = true, .width = 320, .height = 192, .strength = 1,
};

// Synthetic completed durations exercise production decisions only. They are
// not measurements and cannot qualify any actual model/device for Live.
static void feed(struct mp_hdr_live_policy *p, struct mp_hdr_live_settings s,
                 unsigned count, double seconds)
{
    for (unsigned n = 0; n < count; n++)
        mp_hdr_live_record(p, s, p->epoch, seconds);
}

static struct mp_hdr_live_policy ready(void)
{
    struct mp_hdr_live_policy p;
    mp_hdr_live_init(&p, true);
    CHECK(mp_hdr_live_observe_fps(&p, 25));
    feed(&p, neural, 63, .01);
    CHECK(p.qualified && p.warmed_samples == 60 && p.p95 == .01);
    return p;
}

static void cold_window_and_threshold(void)
{
    struct mp_hdr_live_policy p;
    mp_hdr_live_init(&p, false);
    CHECK(p.mode == MP_HDR_DIRECT);
    CHECK(!mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    mp_hdr_live_observe_fps(&p, 25);
    double limit = .8 / 25;
    feed(&p, neural, 3, 9); // excluded cold work must not enter the quantile
    CHECK(p.cold_samples == 3 && p.warmed_samples == 0 && p.p95 == 0);
    feed(&p, neural, 59, limit);
    CHECK(!p.qualified && p.warmed_samples == 59 && p.p95 == limit);
    CHECK(!mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    feed(&p, neural, 1, limit);
    CHECK(p.qualified && p.mode == MP_HDR_DIRECT); // inclusive 80%, no autoentry
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    CHECK(p.mode == MP_HDR_LIVE);
    feed(&p, neural, 4, nextafter(limit, INFINITY));
    CHECK(!p.qualified && p.mode == MP_HDR_ADAPTIVE);
}

static void rolling_quantile_and_modes(void)
{
    struct mp_hdr_live_policy p = ready();
    CHECK(p.mode == MP_HDR_ADAPTIVE);
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    feed(&p, neural, 3, .1);
    CHECK(p.qualified && p.p95 == .01 && p.mode == MP_HDR_LIVE);
    feed(&p, neural, 1, .1); // fourth slow sample crosses rank 57 of 60
    CHECK(!p.qualified && p.p95 == .1 && p.mode == MP_HDR_ADAPTIVE);
    CHECK(!mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    feed(&p, neural, 56, .01);
    CHECK(!p.qualified && p.p95 == .1); // all four slow entries remain
    feed(&p, neural, 1, .01);
    CHECK(p.qualified && p.p95 == .01 && p.warmed_samples == 121);
    CHECK(p.mode == MP_HDR_ADAPTIVE); // recovery needs an explicit request
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_DIRECT));
    CHECK(p.mode == MP_HDR_DIRECT && p.qualified);
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_ADAPTIVE));
    CHECK(p.mode == MP_HDR_ADAPTIVE && p.qualified);
    CHECK(!mp_hdr_live_request(&p, neural, (enum mp_hdr_live_mode)99));
    CHECK(p.mode == MP_HDR_ADAPTIVE && p.qualified);
}

static void reset_and_replacement(void)
{
    struct mp_hdr_live_policy p = ready();
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    uint64_t old_epoch = p.epoch;
    mp_hdr_live_invalidate(&p); // actual seek/bypass/geometry reset entry point
    CHECK(p.epoch != old_epoch && p.mode == MP_HDR_ADAPTIVE && !p.qualified);
    CHECK(p.warmed_samples == 0 && p.p95 == 0 && p.cold_samples == 0);
    mp_hdr_live_record(&p, neural, old_epoch, .001);
    CHECK(p.cold_samples == 0 && p.warmed_samples == 0);
    feed(&p, neural, 62, .01);
    CHECK(!p.qualified && p.warmed_samples == 59);
    feed(&p, neural, 1, .01);
    CHECK(p.qualified && p.mode == MP_HDR_ADAPTIVE);
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_DIRECT));
    mp_hdr_live_invalidate(&p);
    CHECK(p.mode == MP_HDR_DIRECT && !p.qualified);
    mp_hdr_live_init(&p, true); // actual model/settings filter replacement path
    CHECK(p.mode == MP_HDR_ADAPTIVE && p.source_fps == 0 && p.warmed_samples == 0);
    CHECK(!mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    mp_hdr_live_observe_fps(&p, 25);
    struct mp_hdr_live_settings changed = neural;
    changed.strength = .5; changed.width = 640; changed.height = 384;
    feed(&p, changed, 62, .01);
    CHECK(!p.qualified);
    feed(&p, changed, 1, .01);
    CHECK(p.qualified && p.mode == MP_HDR_ADAPTIVE);
}

static void rate_epochs(void)
{
    struct mp_hdr_live_policy p = ready();
    mp_hdr_live_observe_fps(&p, 30);
    feed(&p, neural, 63, .01);
    CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    uint64_t a = p.epoch;
    CHECK(!mp_hdr_live_observe_fps(&p, 30));
    CHECK(p.epoch == a && p.mode == MP_HDR_LIVE);
    CHECK(mp_hdr_live_observe_fps(&p, 60));
    uint64_t b = p.epoch;
    CHECK(p.mode == MP_HDR_ADAPTIVE && !p.qualified && p.warmed_samples == 0);
    CHECK(mp_hdr_live_observe_fps(&p, 30));
    CHECK(p.epoch != a && p.epoch != b);
    mp_hdr_live_record(&p, neural, a, .001); // A-B-A must not admit old A
    mp_hdr_live_record(&p, neural, b, NAN);
    CHECK(p.cold_samples == 0 && p.warmed_samples == 0);
    feed(&p, neural, 63, .01);
    CHECK(p.qualified && p.mode == MP_HDR_ADAPTIVE);
    uint64_t current = p.epoch;
    mp_hdr_live_record(&p, neural, a, NAN);
    CHECK(p.epoch == current && p.qualified && p.warmed_samples == 60);
}

static void valid_but_ineligible(void)
{
    struct mp_hdr_live_settings cases[6];
    for (unsigned n = 0; n < 6; n++) cases[n] = neural;
    cases[0].width = 319;
    cases[1].height = 191;
    cases[2].strength = 0;
    cases[3].have_model = false;
    cases[4].prepared = true;
    cases[5].bypass = true;
    for (unsigned n = 0; n < 6; n++) {
        struct mp_hdr_live_policy p;
        mp_hdr_live_init(&p, true);
        mp_hdr_live_observe_fps(&p, 25);
        feed(&p, cases[n], 63, .01);
        CHECK(!p.qualified && p.warmed_samples == 60 && p.p95 == .01);
        CHECK(!mp_hdr_live_request(&p, cases[n], MP_HDR_LIVE));
        CHECK(p.mode == MP_HDR_ADAPTIVE && p.warmed_samples == 60);
        // Recheck current eligibility when requesting, even if old evidence
        // was eligible. Production settings changes normally replace/reset.
        p = ready();
        CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
        CHECK(!mp_hdr_live_request(&p, cases[n], MP_HDR_LIVE));
        CHECK(!p.qualified && p.mode == MP_HDR_ADAPTIVE);
    }
}

static void malformed_data(void)
{
    const double bad_seconds[] = {0, -1, NAN, INFINITY, -INFINITY};
    for (unsigned n = 0; n < sizeof(bad_seconds) / sizeof(bad_seconds[0]); n++) {
        struct mp_hdr_live_policy p = ready();
        CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
        uint64_t old_epoch = p.epoch;
        mp_hdr_live_record(&p, neural, old_epoch, bad_seconds[n]);
        CHECK(p.epoch != old_epoch && !p.qualified && p.mode == MP_HDR_ADAPTIVE);
        CHECK(p.warmed_samples == 0 && p.p95 == 0 && p.cold_samples == 0);
        mp_hdr_live_record(&p, neural, old_epoch, .01);
        CHECK(p.cold_samples == 0); // other pending work invalidated too
        feed(&p, neural, 62, .01);
        CHECK(!p.qualified);
        feed(&p, neural, 1, .01);
        CHECK(p.qualified && p.mode == MP_HDR_ADAPTIVE);
    }
    const double bad_fps[] = {0, -30, NAN, INFINITY, -INFINITY, DBL_TRUE_MIN};
    for (unsigned n = 0; n < sizeof(bad_fps) / sizeof(bad_fps[0]); n++) {
        struct mp_hdr_live_policy p = ready();
        CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
        mp_hdr_live_observe_fps(&p, bad_fps[n]);
        CHECK(!p.qualified && p.mode == MP_HDR_ADAPTIVE && p.source_fps == 0);
        feed(&p, neural, 63, .01);
        CHECK(p.warmed_samples == 0 && p.p95 == 0);
        CHECK(!mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
    }
    const double bad_strength[] = {-1, NAN, INFINITY, -INFINITY};
    for (unsigned n = 0; n < sizeof(bad_strength) / sizeof(bad_strength[0]); n++) {
        struct mp_hdr_live_settings s = neural;
        s.strength = bad_strength[n];
        struct mp_hdr_live_policy p = ready();
        CHECK(mp_hdr_live_request(&p, neural, MP_HDR_LIVE));
        mp_hdr_live_record(&p, s, p.epoch, .01);
        CHECK(!p.qualified && p.mode == MP_HDR_ADAPTIVE && p.warmed_samples == 0);
        p = ready();
        CHECK(!mp_hdr_live_request(&p, s, MP_HDR_LIVE));
        CHECK(!p.qualified && p.warmed_samples == 0);
    }
}

int main(void)
{
    cold_window_and_threshold();
    rolling_quantile_and_modes();
    reset_and_replacement();
    rate_epochs();
    valid_but_ineligible();
    malformed_data();
    puts("6 Live policy groups passed (synthetic CPU durations, no device qualification)");
}
