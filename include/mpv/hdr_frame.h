/* Optional ScriptType macOS HDR extension; ISC license, as client.h. */
#ifndef MPV_HDR_FRAME_H
#define MPV_HDR_FRAME_H
#include "client.h"
#ifdef __cplusplus
extern "C" {
#endif

#define MPV_HDR_EXPORT_ABI_VERSION 1
typedef struct mpv_hdr_export mpv_hdr_export;
typedef struct mpv_hdr_frame mpv_hdr_frame;
typedef struct { int64_t value; int32_t timescale; } mpv_hdr_time;
typedef enum {
    MPV_HDR_FRAME_READY = 0, MPV_HDR_UNCHANGED = 1, MPV_HDR_NO_FRAME = 2,
    MPV_HDR_UNSUPPORTED = 3, MPV_HDR_FULL = 4, MPV_HDR_CLOSED = 5,
    MPV_HDR_INVALID = -1,
} mpv_hdr_status;
typedef enum {
    MPV_HDR_REASON_NONE = 0, MPV_HDR_REASON_NO_VIDEO = 1,
    MPV_HDR_REASON_FORMAT = 2, MPV_HDR_REASON_STALE = 3,
    MPV_HDR_REASON_DOLBY_VISION = 4, MPV_HDR_REASON_SUBTITLES = 5,
    MPV_HDR_REASON_GEOMETRY = 6, MPV_HDR_REASON_CLOCK = 7,
    MPV_HDR_REASON_TIMING = 8, MPV_HDR_REASON_PLATFORM = 9,
} mpv_hdr_reason;

typedef struct {
    uint32_t struct_size, abi_version;
    // Stream epoch changes on source/filter ownership changes. Generation is
    // the engine seek/reset token. Revision also changes on same-PTS compare.
    uint64_t stream_epoch, generation, revision;
    uint32_t supported, reason, content_kind, pixel_format;
    uint32_t width, height, outstanding_leases, maximum_leases;
    uint32_t producer_pool_capacity, producer_pending_frames;
    uint32_t user_paused, buffering, core_paused, clock_valid;
    mpv_hdr_time source_pts, source_duration;
    // Source timing above is independent of the normalized player timeline.
    double player_pts_seconds, source_to_player_seconds;
    // Native mach_absolute_time ticks. Convert with
    // CMClockMakeHostTimeFromSystemUnits; these are NOT mp_time_ns values.
    uint64_t host_ticks, clock_sample_span_ticks;
    double media_seconds, rate;
    // 1=playing_audio_pts sampled against host_ticks, 2=paused selected frame.
    // A progressing video-only timeline is initially unsupported.
    uint32_t clock_source, reserved;
} mpv_hdr_snapshot;

typedef struct {
    uint32_t struct_size, abi_version;
    mpv_hdr_snapshot selected;
    // Immutable CVPixelBufferRef, GPU-complete RGBA16F, linear BT.2020,
    // full-range RGB with 1.0=203 reference nits, alpha independent.
    // Borrowed for the lease lifetime. No physical AVKit brightness guarantee.
    void *pixel_buffer;
    const char *source_path; // exact opened-core path, UTF-8; not a content hash
    int32_t video_track_id;
    uint32_t reserved;
} mpv_hdr_frame_descriptor;

// Optional symbols: resolve at runtime. One active exporter per core, at most
// three outstanding leases across its exporters; max_leases must be1...3.
// Call open/poll/close on the native client worker, never the AppKit/VO thread.
// Poll briefly synchronizes with the core; it never waits for inference/GPU.
MPV_EXPORT mpv_hdr_status mpv_hdr_export_open(mpv_handle *client,
    uint32_t max_leases, mpv_hdr_export **result);
// Caller initializes snapshot struct_size/abi_version. Every valid poll fills
// current clock/state, including unchanged/unsupported/full results. A lease is
// returned only for a supported selected revision newer than after_revision.
// The selection boundary is a successful VO draw/flip, not physical scanout.
MPV_EXPORT mpv_hdr_status mpv_hdr_export_poll(mpv_hdr_export *exporter,
    uint64_t after_revision, mpv_hdr_snapshot *snapshot, mpv_hdr_frame **frame);
MPV_EXPORT const mpv_hdr_frame_descriptor *mpv_hdr_frame_get(const mpv_hdr_frame *frame);
// Thread-safe validity check immediately before enqueue. False after exporter
// close or engine generation invalidation. A lease keeps its immutable buffer
// alive across reset, filter replacement, exporter close and client destruction.
MPV_EXPORT int mpv_hdr_frame_is_current(const mpv_hdr_frame *frame);
MPV_EXPORT void mpv_hdr_frame_release(mpv_hdr_frame *frame);
// Close before client destruction. Existing frame leases remain releasable;
// retain libmpv's dylib until every lease has been released.
MPV_EXPORT void mpv_hdr_export_close(mpv_hdr_export *exporter);

// AVFoundation can retain CVPixelBuffers beyond enqueue and lease release.
// Consumers must bound that retention (flush completion/backpressure), preserve
// generation checks and never recycle/mutate surfaces while the renderer owns
// them. The producer's six-buffer pool can stall until those references drain.
// Subtitles, raw Dolby Vision, nontrivial crop/rotation/PAR and nonfloat source
// previews are explicitly unsupported by this initial pre-composition export.

#ifdef __cplusplus
}
#endif
#endif
