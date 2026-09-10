/* Private Adaptive/Live audio-tail decisions. License: LGPL-2.1-or-later. */
#ifndef MPV_HDR_AUDIO_CLOCK_H
#define MPV_HDR_AUDIO_CLOCK_H

#include <stdbool.h>

struct mp_hdr_audio_clock_state {
    bool playing, draining_or_eof, eof;
    bool timed_enhancement_video, ao_playing, enhancement_hold;
};

static inline bool mp_hdr_audio_clock_active(struct mp_hdr_audio_clock_state s)
{
    // Logical EOF can precede AO completion under ordinary gapless playback.
    // Only enhancement-driven video extends clock use into that queued tail.
    return s.playing || (s.draining_or_eof && s.timed_enhancement_video &&
                         s.ao_playing);
}

static inline bool mp_hdr_audio_drain_before_pause(struct mp_hdr_audio_clock_state s)
{
    // Preserve normal gapless EOF pause semantics. An enhancement hold must
    // pause the remaining audio, rather than synchronously play it to EOF.
    return s.eof && !(s.timed_enhancement_video && s.ao_playing &&
                      s.enhancement_hold);
}

#endif
