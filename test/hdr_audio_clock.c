#include <stdio.h>
#include <stdlib.h>

#include "player/hdr_audio_clock.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static struct mp_hdr_audio_clock_state enhancement_tail(bool eof)
{
    return (struct mp_hdr_audio_clock_state){
        .draining_or_eof = true, .eof = eof,
        .timed_enhancement_video = true, .ao_playing = true,
    };
}

static void ordinary_gapless_is_unchanged(void)
{
    struct mp_hdr_audio_clock_state s = {.playing = true};
    CHECK(mp_hdr_audio_clock_active(s));
    CHECK(!mp_hdr_audio_drain_before_pause(s));
    s = (struct mp_hdr_audio_clock_state){
        .draining_or_eof = true, .eof = true, .ao_playing = true,
    };
    CHECK(!mp_hdr_audio_clock_active(s));
    CHECK(mp_hdr_audio_drain_before_pause(s));
    // A pause flag alone must not bypass ordinary gapless draining.
    s.enhancement_hold = true;
    CHECK(mp_hdr_audio_drain_before_pause(s));
}

static void queued_draining_and_eof_audio_remain_clock_active(void)
{
    for (int eof = 0; eof <= 1; eof++) {
        struct mp_hdr_audio_clock_state s = enhancement_tail(eof);
        CHECK(mp_hdr_audio_clock_active(s));
        CHECK(mp_hdr_audio_drain_before_pause(s) == (bool)eof);
        // AO-playing includes a logically paused queue; retain that clock.
        s.enhancement_hold = true;
        CHECK(mp_hdr_audio_clock_active(s));
        CHECK(!mp_hdr_audio_drain_before_pause(s));
    }
}

static void only_the_actual_enhancement_hold_skips_draining(void)
{
    struct mp_hdr_audio_clock_state s = enhancement_tail(true);
    CHECK(mp_hdr_audio_drain_before_pause(s)); // user/cache pause
    s.enhancement_hold = true;
    CHECK(!mp_hdr_audio_drain_before_pause(s));
    s.enhancement_hold = false;
    CHECK(mp_hdr_audio_drain_before_pause(s)); // ordinary semantics restored
}

static void stopped_ao_does_not_extend_the_clock(void)
{
    struct mp_hdr_audio_clock_state s = enhancement_tail(true);
    s.enhancement_hold = true;
    s.ao_playing = false;
    CHECK(!mp_hdr_audio_clock_active(s));
    CHECK(mp_hdr_audio_drain_before_pause(s));
    // ao_set_paused itself will not drain an AO that is no longer playing.
}

static void late_enhancement_activation_handles_already_logical_eof(void)
{
    struct mp_hdr_audio_clock_state s = enhancement_tail(true);
    s.timed_enhancement_video = false;
    s.enhancement_hold = true;
    CHECK(!mp_hdr_audio_clock_active(s));
    CHECK(mp_hdr_audio_drain_before_pause(s));
    s.timed_enhancement_video = true;
    CHECK(mp_hdr_audio_clock_active(s));
    CHECK(!mp_hdr_audio_drain_before_pause(s));
    // No new PLAYING -> DRAINING transition was required.
}

static void inactive_video_or_audio_cannot_borrow_a_previous_clock(void)
{
    struct mp_hdr_audio_clock_state s = enhancement_tail(true);
    s.enhancement_hold = true;
    s.timed_enhancement_video = false; // bypass/Direct/no-AO/untimed/video EOF
    CHECK(!mp_hdr_audio_clock_active(s));
    CHECK(mp_hdr_audio_drain_before_pause(s));
    s = (struct mp_hdr_audio_clock_state){
        .timed_enhancement_video = true, .ao_playing = true,
    }; // audio still syncing/ready, not current playing or draining data
    CHECK(!mp_hdr_audio_clock_active(s));
    CHECK(!mp_hdr_audio_drain_before_pause(s));
}

int main(void)
{
    ordinary_gapless_is_unchanged();
    queued_draining_and_eof_audio_remain_clock_active();
    only_the_actual_enhancement_hold_skips_draining();
    stopped_ao_does_not_extend_the_clock();
    late_enhancement_activation_handles_already_logical_eof();
    inactive_video_or_audio_cannot_borrow_a_previous_clock();
    puts("6 enhancement audio-clock groups passed (CPU policy, no physical audio timing)");
}
