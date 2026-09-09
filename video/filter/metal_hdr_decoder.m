/*
 * Independent source-core demux + FFmpeg VideoToolbox Prepared decoder.
 * This file is part of mpv, licensed under LGPL 2.1 or later.
 */
#import <CoreVideo/CoreVideo.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>

#include "common/av_common.h"
#include "common/common.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/filter.h"
#include "misc/thread_tools.h"
#include "stream/stream.h"
#include "video/mp_image.h"
#include "video/filter/metal_hdr_decoder.h"

struct decoder_owner {
    atomic_uint references;
    struct mpv_global *global;
    char *demuxer, *identifier;
    double timestamp_offset, reference_white, hlg_peak;
};

struct preparation_reader {
    struct decoder_owner *owner;
    struct mp_cancel *cancel;
    struct demuxer *demuxer;
    struct sh_stream *stream;
    struct demux_packet *packet;
    AVCodecContext *codec;
    AVFrame *frame;
    struct mp_image *borrowed;
    AVRational timebase;
    fe_time start, end;
    uint32_t mode;
    bool draining;
    uint64_t frame_id;
};

static void retain_owner(void *opaque)
{
    struct decoder_owner *owner = opaque;
    atomic_fetch_add_explicit(&owner->references, 1, memory_order_relaxed);
}

static void release_owner(void *opaque)
{
    struct decoder_owner *owner = opaque;
    if (atomic_fetch_sub_explicit(&owner->references, 1, memory_order_acq_rel) == 1) {
        free(owner->demuxer);
        free(owner->identifier);
        free(owner);
    }
}

static void cancel_reader(void *opaque)
{
    struct preparation_reader *reader = opaque;
    mp_cancel_trigger(reader->cancel);
}

static void close_reader(void *opaque)
{
    struct preparation_reader *reader = opaque;
    if (!reader) return;
    if (reader->cancel) mp_cancel_trigger(reader->cancel);
    talloc_free(reader->borrowed);
    talloc_free(reader->packet);
    av_frame_free(&reader->frame);
    avcodec_free_context(&reader->codec);
    if (reader->demuxer) demux_cancel_and_free(reader->demuxer);
    talloc_free(reader->cancel);
    release_owner(reader->owner);
    free(reader);
}

static enum AVPixelFormat videotoolbox_format(AVCodecContext *codec,
                                             const enum AVPixelFormat *formats)
{
    for (int n = 0; formats[n] != AV_PIX_FMT_NONE; n++) {
        if (formats[n] == AV_PIX_FMT_VIDEOTOOLBOX)
            return formats[n];
    }
    return AV_PIX_FMT_NONE; // never silently switch the Prepared pixel contract
}

static void *open_reader(void *opaque, const char *path, uint32_t ordinal,
                         fe_time start, fe_time end, uint32_t mode,
                         char *error, size_t capacity)
{
    struct decoder_owner *owner = opaque;
    if (mode > FE_PREPARATION_PIXELS ||
        (mode == FE_PREPARATION_PIXELS && (start.timescale <= 0 || end.timescale <= 0))) {
        snprintf(error, capacity, "Invalid Prepared decoder range or mode");
        return NULL;
    }
    struct preparation_reader *reader = calloc(1, sizeof(*reader));
    if (!reader) return NULL;
    retain_owner(owner);
    reader->owner = owner;
    reader->start = start; reader->end = end; reader->mode = mode;
    reader->cancel = mp_cancel_new(NULL);
    // A separate synchronous demuxer preserves native Matroska BlockDuration,
    // packet side data and missing-duration semantics. Reopening with libavformat
    // would synthesize rounded durations that differ from the actual core.
    struct demuxer_params params = {
        .force_format = owner->demuxer,
        .stream_flags = STREAM_READ_FILE_FLAGS_DEFAULT,
        .disable_timeline = true,
        .allow_playlist_create = false,
    };
    reader->demuxer = demux_open_url(path, &params, reader->cancel, owner->global);
    if (!reader->demuxer || strcmp(reader->demuxer->desc->name, owner->demuxer)) {
        snprintf(error, capacity, "Prepared cannot reopen the actual playback demuxer");
        goto fail;
    }
    demux_set_ts_offset(reader->demuxer, owner->timestamp_offset);
    uint32_t index = 0;
    for (int n = 0; n < demux_get_num_stream(reader->demuxer); n++) {
        struct sh_stream *stream = demux_get_stream(reader->demuxer, n);
        if (stream->type == STREAM_VIDEO && index++ == ordinal)
            reader->stream = stream;
    }
    if (!reader->stream || reader->stream->attached_picture || reader->stream->codec->avi_dts) {
        snprintf(error, capacity, "Prepared requires a video stream with reliable presentation timestamps");
        goto fail;
    }
    demuxer_select_track(reader->demuxer, reader->stream, MP_NOPTS_VALUE, true);
    const struct mp_codec_params *source = reader->stream->codec;
    AVCodecParameters *parameters = mp_codec_params_to_av(source);
    const AVCodec *codec = parameters ? avcodec_find_decoder(parameters->codec_id) : NULL;
    avcodec_parameters_free(&parameters);
    if (!codec) {
        snprintf(error, capacity, "Prepared source has no FFmpeg video decoder");
        goto fail;
    }
    reader->timebase = mp_get_codec_timebase(source);
    reader->codec = avcodec_alloc_context3(codec);
    reader->frame = av_frame_alloc();
    if (!reader->codec || !reader->frame) goto fail;
    if (mp_set_avctx_codec_headers(reader->codec, source) < 0) {
        snprintf(error, capacity, "Prepared cannot configure source codec headers");
        goto fail;
    }
    reader->codec->pkt_timebase = reader->timebase;
    reader->codec->get_format = videotoolbox_format;
    reader->codec->thread_count = 1;
    reader->codec->extra_hw_frames = 3;
    int status = av_hwdevice_ctx_create(&reader->codec->hw_device_ctx,
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0);
    if (status >= 0) status = avcodec_open2(reader->codec, codec, NULL);
    if (status < 0) {
        snprintf(error, capacity, "Prepared VideoToolbox initialization failed: %s", av_err2str(status));
        goto fail;
    }
    if (mode == FE_PREPARATION_PIXELS &&
        !demux_seek(reader->demuxer, (double)start.value / start.timescale, SEEK_HR)) {
        snprintf(error, capacity, "Prepared source cannot seek before exact preroll");
        goto fail;
    }
    return reader;
fail:
    close_reader(reader);
    return NULL;
}

static void merge_container_params(struct mp_image *image, const struct mp_codec_params *source)
{
    struct mp_image_params *params = &image->params;
    // Match default decoder-wrapper interpretation. Transforms remain unbaked
    // metadata; the cache stores the coded frame's linear pixels.
    if (source->par_w > 0 && source->par_h > 0) {
        params->p_w = source->par_w; params->p_h = source->par_h;
    }
    if (params->p_w <= 0 || params->p_h <= 0) params->p_w = params->p_h = 1;
    if (!params->rotate) params->rotate = source->rotate;
    if (!mp_rect_equals(&source->crop, &(struct mp_rect){0})) {
        struct mp_rect crop = source->crop;
        crop.x0 += params->crop.x0; crop.x1 += params->crop.x0;
        crop.y0 += params->crop.y0; crop.y1 += params->crop.y0;
        if (mp_image_crop_valid(&(struct mp_image_params){
            .w = mp_rect_w(params->crop), .h = mp_rect_h(params->crop), .crop = crop}))
            params->crop = crop;
    }
    pl_color_space_merge(&params->color, &source->color);
    pl_color_repr_merge(&params->repr, &source->repr);
    if (params->chroma_location == PL_CHROMA_UNKNOWN)
        params->chroma_location = source->chroma_location;
    mp_image_params_guess_csp(params);
}

static fe_status next_frame(void *opaque, fe_frame *result, char *error, size_t capacity)
{
    struct preparation_reader *reader = opaque;
    talloc_free(reader->borrowed); reader->borrowed = NULL;
    while (!mp_cancel_test(reader->cancel)) {
        int status = avcodec_receive_frame(reader->codec, reader->frame);
        if (status == 0) {
            if (reader->frame->pts == AV_NOPTS_VALUE) {
                snprintf(error, capacity, "Prepared decoder output lacks exact presentation timestamp");
                return FE_FAILED;
            }
            struct mp_image *image = mp_image_from_av_frame(reader->frame);
            if (!image) return FE_FAILED;
            image->source_pts = reader->frame->pts;
            image->source_duration = reader->frame->duration;
            image->source_timebase_num = reader->timebase.num;
            image->source_timebase_den = reader->timebase.den;
            image->pts = mp_pts_from_av(reader->frame->pts, &reader->timebase);
            image->pkt_duration = mp_pts_from_av(reader->frame->duration, &reader->timebase);
            image->nominal_fps = reader->stream->codec->fps;
            av_frame_unref(reader->frame);
            merge_container_params(image, reader->stream->codec);
            bool valid = mp_hdr_frame_descriptor(image, result,
                reader->owner->reference_white, reader->owner->hlg_peak, error, capacity);
            if (!valid) { talloc_free(image); return FE_FAILED; }
            if (reader->mode == FE_PREPARATION_PIXELS) {
                int before = av_compare_ts(result->pts.value, (AVRational){1, result->pts.timescale},
                    reader->start.value, (AVRational){1, reader->start.timescale});
                int after = av_compare_ts(result->pts.value, (AVRational){1, result->pts.timescale},
                    reader->end.value, (AVRational){1, reader->end.timescale});
                if (before < 0) { talloc_free(image); continue; }
                if (after >= 0) { talloc_free(image); return FE_EMPTY; }
            }
            result->frame_id = reader->frame_id++;
            if (reader->mode == FE_PREPARATION_INVENTORY) {
                result->pixel_buffer = NULL;
                result->plane_count = 0;
            }
            reader->borrowed = image;
            return FE_ACCEPTED;
        }
        if (status == AVERROR_EOF) return FE_EMPTY;
        if (status != AVERROR(EAGAIN)) {
            snprintf(error, capacity, "Prepared VideoToolbox decode failed: %s", av_err2str(status));
            return FE_FAILED;
        }
        if (!reader->packet && !reader->draining) {
            int read_status = demux_read_packet_async(reader->stream, &reader->packet);
            if (mp_cancel_test(reader->cancel)) return FE_CANCELLED;
            if (read_status == 0) {
                snprintf(error, capacity, "Independent Prepared demuxer unexpectedly blocked");
                return FE_FAILED;
            }
            reader->draining = read_status < 0;
        }
        // mp_set_av_packet borrows the demux packet's data/side-data. libavcodec
        // retains accepted data internally. Do not unref this borrowed wrapper.
        AVPacket packet = {0};
        mp_set_av_packet(&packet, reader->packet, &reader->timebase);
        status = avcodec_send_packet(reader->codec, reader->packet ? &packet : NULL);
        if (status == AVERROR(EAGAIN)) continue;
        talloc_free(reader->packet); reader->packet = NULL;
        if (status == AVERROR_EOF) return FE_EMPTY;
        if (status < 0) {
            snprintf(error, capacity, "Prepared source packet rejected: %s", av_err2str(status));
            return FE_FAILED;
        }
    }
    return FE_CANCELLED;
}

fe_preparation_decoder_provider mp_hdr_preparation_decoder(struct mp_filter *filter,
                                      double reference_white, double hlg_peak)
{
    struct mp_stream_info *info = mp_filter_find_stream_info(filter);
    if (!info || !info->source_demuxer ||
        (strcmp(info->source_demuxer, "mkv") && strcmp(info->source_demuxer, "lavf")))
        return (fe_preparation_decoder_provider){0};
    struct decoder_owner *owner = calloc(1, sizeof(*owner));
    if (!owner) return (fe_preparation_decoder_provider){0};
    atomic_init(&owner->references, 1);
    owner->global = filter->global;
    owner->demuxer = strdup(info->source_demuxer);
    owner->timestamp_offset = info->source_timestamp_offset;
    owner->reference_white = reference_white; owner->hlg_peak = hlg_peak;
    if (!owner->demuxer) {
        release_owner(owner);
        return (fe_preparation_decoder_provider){0};
    }
    if (asprintf(&owner->identifier,
        "mpv-independent-demux-vt-v1;%s;libavcodec=%u;demux=%s;offset=%.17g;white=%.17g;hlg=%.17g;exact-decoder-pts-duration-native-fallback",
        mpv_version, avcodec_version(), owner->demuxer, owner->timestamp_offset,
        reference_white, hlg_peak) < 0) {
        release_owner(owner);
        return (fe_preparation_decoder_provider){0};
    }
    return (fe_preparation_decoder_provider){
        .struct_size = sizeof(fe_preparation_decoder_provider), .abi_version = FE_ABI_VERSION,
        .identifier = owner->identifier, .user = owner,
        .retain_user = retain_owner, .release_user = release_owner,
        .open = open_reader, .next = next_frame, .cancel = cancel_reader, .close = close_reader,
    };
}
