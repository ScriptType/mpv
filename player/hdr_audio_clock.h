/* License: LGPL-2.1-or-later. */
#ifndef MPV_HDR_AUDIO_CLOCK_H
#define MPV_HDR_AUDIO_CLOCK_H

#include <stdbool.h>

struct mp_hdr_audio_clock_state {
    bool playing, draining_or_eof, eof;
    bool timed_enhancement_video, ao_playing, enhancement_hold;
};

static inline bool mp_hdr_audio_clock_active(struct mp_hdr_audio_clock_state s)
{
    // Gapless EOF can precede AO completion. Only enhancement-held video keeps
    // using that queued tail as its clock.
    return s.playing || (s.draining_or_eof && s.timed_enhancement_video &&
                         s.ao_playing);
}

static inline bool mp_hdr_audio_drain_before_pause(struct mp_hdr_audio_clock_state s)
{
    // An enhancement hold pauses the queued tail instead of draining it to EOF.
    return s.eof && !(s.timed_enhancement_video && s.ao_playing &&
                      s.enhancement_hold);
}

#endif
