/* Included by client.c: optional native selected-frame client extension. */
#include <libavutil/buffer.h>
#include <libavutil/mathematics.h>
#include "config.h"
#include "filters/f_output_chain.h"
#include "demux/demux.h"
#include "video/mp_image.h"
#include "video/out/vo.h"
#include "audio/out/ao.h"
#if HAVE_DARWIN
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#endif

struct hdr_budget {
    atomic_uint refs, outstanding;
};

struct hdr_lifetime {
    atomic_uint refs;
    atomic_bool closed;
    atomic_uint_fast64_t epoch;
    struct hdr_budget *budget;
};

struct mpv_hdr_export {
    mpv_handle *ctx; // detached before client destruction, accessed by worker
    struct hdr_lifetime *lifetime;
    uint32_t maximum;
    struct vo *last_vo; // comparison only; never dereferenced after replacement
    AVBufferRef *last_owner, *last_selection;
    char *source;
    int track_id;
    uint64_t revision, observed_selection, observed_generation;
    uint32_t observed_reason;
    bool seeking;
};

struct mpv_hdr_frame {
    mpv_hdr_frame_descriptor descriptor;
    struct hdr_lifetime *lifetime;
    AVBufferRef *generation, *selection;
    uint64_t selection_revision;
};

static void hdr_budget_unref(struct hdr_budget *b)
{
    if (b && atomic_fetch_sub(&b->refs, 1) == 1) free(b);
}

static void hdr_lifetime_unref(struct hdr_lifetime *l)
{
    if (l && atomic_fetch_sub(&l->refs, 1) == 1) {
        hdr_budget_unref(l->budget);
        free(l);
    }
}

static void hdr_export_detach(mpv_handle *ctx)
{
    // Like client destruction itself, serialized with this client's API calls.
    // Internal clients may be destroyed during initialization with core locked.
    // They cannot own an exporter; do not acquire that lock recursively.
    if (!ctx->hdr_export) return;
    lock_core(ctx);
    struct mpv_hdr_export *e = ctx->clients->hdr_export;
    if (e && e->ctx == ctx) {
        if (ctx->mpctx->video_out)
            vo_hdr_export_enable(ctx->mpctx->video_out, false);
        ctx->clients->hdr_export = NULL;
        ctx->hdr_export = NULL;
        e->ctx = NULL;
        atomic_store(&e->lifetime->closed, true);
    }
    unlock_core(ctx);
}

mpv_hdr_status mpv_hdr_export_open(mpv_handle *ctx, uint32_t maximum,
                                  mpv_hdr_export **result)
{
    if (!ctx || !result || maximum < 1 || maximum > 3) return MPV_HDR_INVALID;
    *result = NULL;
#if !HAVE_DARWIN || !HAVE_FRAME_ENGINE
    return MPV_HDR_UNSUPPORTED;
#else
    if (!ctx->mpctx->initialized) return MPV_HDR_INVALID;
    lock_core(ctx);
    if (ctx->clients->hdr_export) { unlock_core(ctx); return MPV_HDR_FULL; }
    if (!ctx->clients->hdr_budget) {
        ctx->clients->hdr_budget = calloc(1, sizeof(struct hdr_budget));
        if (!ctx->clients->hdr_budget) { unlock_core(ctx); return MPV_HDR_INVALID; }
        atomic_init(&ctx->clients->hdr_budget->refs, 1);
        atomic_init(&ctx->clients->hdr_budget->outstanding, 0);
    }
    struct mpv_hdr_export *e = calloc(1, sizeof(*e));
    struct hdr_lifetime *l = calloc(1, sizeof(*l));
    if (!e || !l) { free(e); free(l); unlock_core(ctx); return MPV_HDR_INVALID; }
    atomic_init(&l->refs, 1);
    atomic_init(&l->closed, false);
    atomic_init(&l->epoch, 1);
    l->budget = ctx->clients->hdr_budget;
    atomic_fetch_add(&l->budget->refs, 1);
    *e = (struct mpv_hdr_export){ .ctx = ctx, .maximum = maximum, .lifetime = l,
        .observed_reason = MPV_HDR_REASON_NO_VIDEO };
    ctx->clients->hdr_export = e;
    ctx->hdr_export = e;
    *result = e;
    unlock_core(ctx);
    return MPV_HDR_FRAME_READY;
#endif
}

const mpv_hdr_frame_descriptor *mpv_hdr_frame_get(const mpv_hdr_frame *frame)
{
    return frame ? &frame->descriptor : NULL;
}

int mpv_hdr_frame_is_current(const mpv_hdr_frame *f)
{
    if (!f || atomic_load(&f->lifetime->closed) ||
        atomic_load(&f->lifetime->epoch) != f->descriptor.selected.stream_epoch)
        return 0;
    return f->generation && f->selection &&
        atomic_load((_Atomic uint64_t *)f->generation->data) == f->descriptor.selected.generation &&
        atomic_load((_Atomic uint64_t *)f->selection->data) == f->selection_revision;
}

void mpv_hdr_frame_release(mpv_hdr_frame *f)
{
    if (!f) return;
#if HAVE_DARWIN
    if (f->descriptor.pixel_buffer) CVPixelBufferRelease(f->descriptor.pixel_buffer);
#endif
    av_buffer_unref(&f->generation);
    av_buffer_unref(&f->selection);
    free((void *)f->descriptor.source_path);
    atomic_fetch_sub(&f->lifetime->budget->outstanding, 1);
    hdr_lifetime_unref(f->lifetime);
    free(f);
}

void mpv_hdr_export_close(mpv_hdr_export *e)
{
    if (!e) return;
    if (e->ctx) hdr_export_detach(e->ctx);
    atomic_store(&e->lifetime->closed, true);
    av_buffer_unref(&e->last_owner);
    av_buffer_unref(&e->last_selection);
    free(e->source);
    hdr_lifetime_unref(e->lifetime);
    free(e);
}

#if HAVE_DARWIN && HAVE_FRAME_ENGINE
static bool hdr_same_owner(AVBufferRef *a, AVBufferRef *b)
{
    return (!a && !b) || (a && b && a->data == b->data);
}

static bool hdr_source_time(struct mp_image *image, mpv_hdr_time *time)
{
    return image->source_pts != AV_NOPTS_VALUE && image->source_timebase_num > 0 &&
        image->source_timebase_den > 0 &&
        !__builtin_mul_overflow(image->source_pts, (int64_t)image->source_timebase_num, &time->value) &&
        (time->timescale = image->source_timebase_den) > 0;
}

static bool hdr_geometry_supported(struct mp_image *i)
{
    return i->params.rotate == 0 &&
        (i->params.p_w <= 0 || i->params.p_h <= 0 || i->params.p_w == i->params.p_h) &&
        i->params.crop.x0 == 0 && i->params.crop.y0 == 0 &&
        i->params.crop.x1 == i->w && i->params.crop.y1 == i->h;
}
#endif

mpv_hdr_status mpv_hdr_export_poll(mpv_hdr_export *e, uint64_t after_revision,
                                  mpv_hdr_snapshot *s, mpv_hdr_frame **result)
{
    if (!e || !s || !result || s->struct_size != sizeof(*s) ||
        s->abi_version != MPV_HDR_EXPORT_ABI_VERSION) return MPV_HDR_INVALID;
    *result = NULL;
    *s = (mpv_hdr_snapshot){ .struct_size = sizeof(*s), .abi_version = MPV_HDR_EXPORT_ABI_VERSION,
        .maximum_leases = e->maximum,
        .outstanding_leases = atomic_load(&e->lifetime->budget->outstanding),
        .producer_pool_capacity = 6, .reason = MPV_HDR_REASON_NO_VIDEO };
    if (!e->ctx || atomic_load(&e->lifetime->closed)) return MPV_HDR_CLOSED;
#if !HAVE_DARWIN || !HAVE_FRAME_ENGINE
    s->reason = MPV_HDR_REASON_PLATFORM;
    return MPV_HDR_UNSUPPORTED;
#else
    mpv_handle *ctx = e->ctx;
    lock_core(ctx);
    struct MPContext *mpctx = ctx->mpctx;
    struct vo *vo = mpctx->video_out;
    struct mp_async_video_state empty = {0};
    struct mp_async_video_state *state = mpctx->vo_chain ?
        &mpctx->vo_chain->filter->async_video : &empty;
    struct track *track = mpctx->vo_chain ? mpctx->vo_chain->track : NULL;
    const char *source = track && track->demuxer ? track->demuxer->filename : "";
    int track_id = track ? track->user_tid : -1;
    bool seeking = mpctx->seek.type != MPSEEK_NONE || mpctx->hrseek_active;
    s->user_paused = mpctx->opts->pause;
    s->buffering = mpctx->paused_for_enhancement || mpctx->paused_for_cache || seeking;
    s->core_paused = mpctx->paused;
    s->producer_pending_frames = state->pending;
    s->generation = state->generation;

    uint64_t host_before = mach_absolute_time();
    double audio = playing_audio_pts(mpctx);
    struct mp_image *image = NULL;
    AVBufferRef *selection = NULL;
    uint64_t selected_revision = 0;
    if (vo) {
        vo_hdr_export_enable(vo, true);
        image = vo_hdr_export_get(vo, &selected_revision, &selection);
    }
    uint64_t host_after = mach_absolute_time();
    s->host_ticks = host_before + (host_after - host_before) / 2;
    s->clock_sample_span_ticks = host_after - host_before;

    bool owner_changed = !hdr_same_owner(e->last_owner, image ? image->async_generation : NULL);
    bool source_changed = !e->source || strcmp(e->source, source) || e->track_id != track_id;
    bool selection_changed = !hdr_same_owner(e->last_selection, selection) ||
                             selected_revision != e->observed_selection;
    bool generation_changed = e->observed_generation != state->generation;
    if (owner_changed || source_changed || generation_changed || seeking != e->seeking || vo != e->last_vo) {
        atomic_fetch_add(&e->lifetime->epoch, 1);
        e->revision++;
    } else if (selection_changed) {
        e->revision++;
    }
    if (source_changed) {
        free(e->source);
        e->source = strdup(source);
        e->track_id = track_id;
    }
    av_buffer_unref(&e->last_owner);
    e->last_owner = image && image->async_generation ? av_buffer_ref(image->async_generation) : NULL;
    av_buffer_unref(&e->last_selection);
    e->last_selection = selection ? av_buffer_ref(selection) : NULL;
    e->last_vo = vo;
    e->seeking = seeking;
    e->observed_selection = selected_revision;
    e->observed_generation = state->generation;
    s->stream_epoch = atomic_load(&e->lifetime->epoch);
    s->revision = e->revision;

    if (image) {
        s->width = image->w; s->height = image->h;
        s->content_kind = image->async_content_kind;
        hdr_source_time(image, &s->source_pts);
        s->source_duration = (mpv_hdr_time){image->async_duration_value, image->async_duration_timescale};
        s->player_pts_seconds = image->pts;
        s->source_to_player_seconds = s->source_pts.timescale > 0 ?
            image->pts - (double)s->source_pts.value / s->source_pts.timescale : NAN;
        if (image->imgfmt == IMGFMT_VIDEOTOOLBOX && image->planes[3])
            s->pixel_format = CVPixelBufferGetPixelFormatType((CVPixelBufferRef)image->planes[3]);
    }

    // The clock uses the same audio-delay interpretation as mpv video sync.
    if (mpctx->ao && !ao_untimed(mpctx->ao) && audio != MP_NOPTS_VALUE && isfinite(audio) &&
        isfinite(mpctx->audio_speed) && mpctx->audio_speed > 0 &&
        (ao_is_playing(mpctx->ao) || s->core_paused || s->buffering)) {
        s->media_seconds = audio + mpctx->opts->audio_delay;
        s->rate = s->core_paused || s->buffering ? 0 : mpctx->audio_speed;
        s->clock_source = 1;
        s->clock_valid = isfinite(s->media_seconds);
    } else if (image && (s->core_paused || s->user_paused) && isfinite(image->pts) &&
               image->pts != MP_NOPTS_VALUE) {
        s->media_seconds = image->pts;
        s->rate = 0;
        s->clock_source = 2;
        s->clock_valid = 1;
    }
    s->reason = !image ? MPV_HDR_REASON_NO_VIDEO : MPV_HDR_REASON_NONE;
    if (image) {
        // Both references are required: allocation of the selection token can
        // fail independently of the image reference. av_buffer_ref requires a
        // non-NULL source, and a lease without this token cannot track redraws.
        if (!selection || seeking || !mp_image_is_current(image) || (image->async_generation &&
            image->async_frame_generation != state->generation))
            s->reason = MPV_HDR_REASON_STALE;
        else if (state->source_dovi || image->dovi || image->params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION)
            s->reason = MPV_HDR_REASON_DOLBY_VISION;
        else if (!image->async_generation || s->pixel_format != kCVPixelFormatType_64RGBAHalf ||
                 image->params.color.primaries != PL_COLOR_PRIM_BT_2020 ||
                 image->params.color.transfer != PL_COLOR_TRC_LINEAR ||
                 image->params.repr.sys != PL_COLOR_SYSTEM_RGB)
            s->reason = MPV_HDR_REASON_FORMAT;
        else if (!hdr_geometry_supported(image))
            s->reason = MPV_HDR_REASON_GEOMETRY;
        else if (s->source_pts.timescale <= 0 || s->source_duration.timescale <= 0 ||
                 s->source_duration.value <= 0 || !isfinite(s->source_to_player_seconds) ||
                 !isfinite(image->pts) || image->pts == MP_NOPTS_VALUE)
            s->reason = MPV_HDR_REASON_TIMING;
        for (int n = 0; n < 2; n++) {
            if (mpctx->current_track[n][STREAM_SUB] && mpctx->opts->subs_shared->sub_visibility[n])
                s->reason = MPV_HDR_REASON_SUBTITLES;
        }
        if (s->reason == MPV_HDR_REASON_NONE && !s->clock_valid)
            s->reason = MPV_HDR_REASON_CLOCK;
    }
    s->supported = s->reason == MPV_HDR_REASON_NONE;
    // Capability changes (e.g. a subtitle toggled while paused) invalidate
    // already-exported leases even when the underlying frame did not change.
    if (s->reason != e->observed_reason) {
        e->observed_reason = s->reason;
        atomic_fetch_add(&e->lifetime->epoch, 1);
        e->revision++;
        s->stream_epoch = atomic_load(&e->lifetime->epoch);
        s->revision = e->revision;
    }
    mpv_hdr_status status = !image ? MPV_HDR_NO_FRAME : !s->supported ? MPV_HDR_UNSUPPORTED :
        s->revision <= after_revision ? MPV_HDR_UNCHANGED : MPV_HDR_FRAME_READY;
    if (status == MPV_HDR_FRAME_READY) {
        struct hdr_budget *b = e->lifetime->budget;
        unsigned count = atomic_load(&b->outstanding);
        if (count >= e->maximum) {
            status = MPV_HDR_FULL;
        } else {
            struct mpv_hdr_frame *f = calloc(1, sizeof(*f));
            if (!f) {
                status = MPV_HDR_INVALID;
            } else {
                atomic_fetch_add(&b->outstanding, 1);
                atomic_fetch_add(&e->lifetime->refs, 1);
                f->lifetime = e->lifetime;
                f->generation = av_buffer_ref(image->async_generation);
                f->selection = av_buffer_ref(selection);
                f->selection_revision = selected_revision;
                f->descriptor = (mpv_hdr_frame_descriptor){ .struct_size = sizeof(f->descriptor),
                    .abi_version = MPV_HDR_EXPORT_ABI_VERSION, .selected = *s,
                    .pixel_buffer = CVPixelBufferRetain((CVPixelBufferRef)image->planes[3]),
                    .source_path = strdup(source), .video_track_id = track_id };
                if (!f->generation || !f->selection || !f->descriptor.source_path || !mpv_hdr_frame_is_current(f)) {
                    mpv_hdr_frame_release(f);
                    status = MPV_HDR_UNSUPPORTED;
                    s->supported = 0; s->reason = MPV_HDR_REASON_STALE;
                } else {
                    *result = f;
                }
            }
        }
    }
    s->outstanding_leases = atomic_load(&e->lifetime->budget->outstanding);
    if (*result) (*result)->descriptor.selected.outstanding_leases = s->outstanding_leases;
    talloc_free(image);
    av_buffer_unref(&selection);
    unlock_core(ctx);
    return status;
#endif
}
