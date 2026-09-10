/* Private completed-work qualification for the Metal HDR filter.
 * License: LGPL-2.1-or-later
 */
#ifndef MPV_METAL_HDR_LIVE_POLICY_H
#define MPV_METAL_HDR_LIVE_POLICY_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum mp_hdr_live_mode {
    MP_HDR_DIRECT,
    MP_HDR_ADAPTIVE,
    MP_HDR_LIVE,
};

struct mp_hdr_live_settings {
    bool have_model, prepared, bypass;
    int width, height;
    double strength;
};

struct mp_hdr_live_policy {
    enum mp_hdr_live_mode mode;
    uint64_t epoch, warmed_samples;
    unsigned cold_samples, count, next;
    double samples[60], p95, source_fps;
    bool qualified;
};

static inline void mp_hdr_live_init(struct mp_hdr_live_policy *p, bool adaptive)
{
    *p = (struct mp_hdr_live_policy){
        .mode = adaptive ? MP_HDR_ADAPTIVE : MP_HDR_DIRECT,
        .epoch = 1,
    };
}

// Seek/bypass/geometry resets discard timing evidence. A replacement filter
// uses init instead, so model/settings/device evidence cannot cross sessions.
static inline void mp_hdr_live_invalidate(struct mp_hdr_live_policy *p)
{
    p->epoch++;
    p->cold_samples = p->count = p->next = 0;
    p->warmed_samples = 0;
    p->p95 = 0;
    p->qualified = false;
    if (p->mode == MP_HDR_LIVE)
        p->mode = MP_HDR_ADAPTIVE;
}

static inline bool mp_hdr_live_valid_fps(double fps)
{
    return isfinite(fps) && fps > 0 && isfinite(.8 / fps);
}

// This epoch affects qualification only, never engine temporal history. Each
// admission retains it, excluding old pending work even across FPS A-B-A.
static inline bool mp_hdr_live_observe_fps(struct mp_hdr_live_policy *p, double fps)
{
    if (!mp_hdr_live_valid_fps(fps))
        fps = 0;
    if (p->source_fps == fps)
        return false;
    mp_hdr_live_invalidate(p);
    p->source_fps = fps;
    return true;
}

static inline bool mp_hdr_live_valid_settings(struct mp_hdr_live_settings s)
{
    return isfinite(s.strength) && s.strength >= 0 && s.width > 0 && s.height > 0;
}

static inline bool mp_hdr_live_eligible(const struct mp_hdr_live_policy *p,
                                        struct mp_hdr_live_settings s)
{
    return mp_hdr_live_valid_settings(s) && mp_hdr_live_valid_fps(p->source_fps) &&
           s.have_model && !s.prepared && !s.bypass && s.strength > 0 &&
           s.width >= 320 && s.height >= 192;
}

static inline int mp_hdr_live_compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static inline void mp_hdr_live_record(struct mp_hdr_live_policy *p,
                                       struct mp_hdr_live_settings s,
                                       uint64_t epoch, double seconds)
{
    // Rejected stale completions consume neither cold nor warmed samples and
    // cannot invalidate newer evidence, even if their old timing is malformed.
    if (epoch != p->epoch)
        return;
    if (!isfinite(seconds) || seconds <= 0 ||
        !mp_hdr_live_valid_fps(p->source_fps) || !mp_hdr_live_valid_settings(s)) {
        mp_hdr_live_invalidate(p);
        return;
    }
    if (p->cold_samples < 3) {
        p->cold_samples++;
        return;
    }
    if (p->warmed_samples < UINT64_MAX)
        p->warmed_samples++;
    p->samples[p->next] = seconds;
    p->next = (p->next + 1) % 60;
    if (p->count < 60)
        p->count++;
    double sorted[60];
    memcpy(sorted, p->samples, p->count * sizeof(double));
    qsort(sorted, p->count, sizeof(double), mp_hdr_live_compare_double);
    p->p95 = sorted[(int)ceil(p->count * .95) - 1];
    p->qualified = p->count == 60 && mp_hdr_live_eligible(p, s) &&
                   p->p95 <= .8 / p->source_fps;
    if (p->mode == MP_HDR_LIVE && !p->qualified)
        p->mode = MP_HDR_ADAPTIVE;
}

// Qualification does not enter Live automatically; the existing user command
// must request it. Direct/Adaptive remain available without qualification.
static inline bool mp_hdr_live_request(struct mp_hdr_live_policy *p,
                                        struct mp_hdr_live_settings s,
                                        enum mp_hdr_live_mode mode)
{
    if (mode != MP_HDR_DIRECT && mode != MP_HDR_ADAPTIVE && mode != MP_HDR_LIVE)
        return false;
    if (mode == MP_HDR_LIVE && (!mp_hdr_live_valid_settings(s) ||
                              !mp_hdr_live_valid_fps(p->source_fps))) {
        mp_hdr_live_invalidate(p);
        return false;
    }
    if (mode == MP_HDR_LIVE && (!p->qualified || !mp_hdr_live_eligible(p, s))) {
        p->qualified = false;
        if (p->mode == MP_HDR_LIVE)
            p->mode = MP_HDR_ADAPTIVE;
        return false;
    }
    p->mode = mode;
    return true;
}

#endif
