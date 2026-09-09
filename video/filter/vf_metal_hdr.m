/*
 * Asynchronous VideoToolbox -> shared HDR engine -> native float import.
 * This file is part of mpv, licensed under LGPL 2.1 or later.
 */
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>

#include <libavutil/buffer.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <frame_engine.h>

#include "common/common.h"
#include "common/msg.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "video/mp_image.h"

#define HDR_SLOTS 3
#define HDR_POOL_BUFFERS 6

struct hdr_options {
    char *model;
    char *measurements;
    char *measurement_config, *engine_report;
    int processing_width, processing_height;
    double strength, colour_strength, reference_white, hlg_peak, maximum_luminance_ratio;
    bool bypass;
};

struct hdr_result {
    CVPixelBufferRef buffer;
    fe_frame frame;
    double normalized_host, normalization_gpu_seconds;
    float peak_nits;
};

struct hdr_pending {
    uint64_t frame_id;
    struct mp_image *image;
    double submitted_host;
};

struct hdr_gpu_job {
    fe_output *lease;
    fe_frame frame;
    CVPixelBufferRef buffer;
    CVMetalTextureRef input_view, output_view;
    id<MTLCommandBuffer> command;
    id<MTLBuffer> peak;
};

struct priv {
    struct hdr_options *opts;
    fe_session *session;
    mp_thread thread;
    mp_mutex lock;
    mp_cond wakeup;
    bool thread_started, stop, failed;
    char error[512];
    struct hdr_result results[HDR_SLOTS];
    int result_count;
    int active_work; // protected by lock; idle workers sleep without polling
    struct hdr_pending pending[HDR_SLOTS];
    int pending_count;
    struct mp_frame held_input;
    bool eof;
    uint64_t next_frame, emitted, resets;
    AVBufferRef *generation;
    struct mp_image_params input_params;
    bool have_input_params;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> normalize;
    CVMetalTextureCacheRef texture_cache;
    CVPixelBufferPoolRef pool;
    int pool_width, pool_height;
    FILE *measurements;
    bool measurements_configured;
};

static void release_result(struct hdr_result *result)
{
    if (result->buffer)
        CVPixelBufferRelease(result->buffer);
    *result = (struct hdr_result){0};
}

static void release_gpu_job(struct hdr_gpu_job *job)
{
    if (job->input_view) CFRelease(job->input_view);
    if (job->output_view) CFRelease(job->output_view);
    if (job->buffer) CVPixelBufferRelease(job->buffer);
    [job->command release];
    [job->peak release];
    if (job->lease) fe_output_release(job->lease);
    *job = (struct hdr_gpu_job){0};
}

static void fail_worker(struct mp_filter *f, const char *message)
{
    struct priv *p = f->priv;
    mp_mutex_lock(&p->lock);
    p->failed = true;
    snprintf(p->error, sizeof(p->error), "%s", message);
    mp_mutex_unlock(&p->lock);
    mp_filter_wakeup(f);
}

// A single pass converts absolute nits into libplacebo's 1.0=203-nit linear
// domain without clipping, and computes output peak luminance per threadgroup.
static NSString *normalization_shader = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void normalize_nits(texture2d<half,access::read> src [[texture(0)]],"
    "texture2d<half,access::write> dst [[texture(1)]],"
    "device atomic_uint *peak [[buffer(0)]], uint2 gid [[thread_position_in_grid]],"
    "uint tid [[thread_index_in_threadgroup]]) {"
    "threadgroup float lum[256]; float y=0.0;"
    "if(gid.x<src.get_width() && gid.y<src.get_height()){"
    "float4 rgba=float4(src.read(gid));"
    "y=max(0.0,dot(rgba.rgb,float3(0.2627,0.6780,0.0593)));"
    "dst.write(half4(float4(rgba.rgb/203.0,rgba.a)),gid); }"
    "lum[tid]=y; threadgroup_barrier(mem_flags::mem_threadgroup);"
    "for(uint n=128;n>0;n/=2){if(tid<n)lum[tid]=max(lum[tid],lum[tid+n]);"
    "threadgroup_barrier(mem_flags::mem_threadgroup);}"
    "if(tid==0)atomic_fetch_max_explicit(peak,as_type<uint>(lum[0]),memory_order_relaxed);"
    "}";

// 0 means pool backpressure, 1 enqueued, -1 failure. All allocations are bounded
// by six downstream pixel buffers plus the engine's three admitted frame slots.
static int start_normalization(struct priv *p, struct hdr_gpu_job *job)
{
    int width = job->frame.geometry.width, height = job->frame.geometry.height;
    if (!p->pool || width != p->pool_width || height != p->pool_height) {
        if (p->pool) CFRelease(p->pool);
        p->pool = NULL;
        NSDictionary *attributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_64RGBAHalf),
            (id)kCVPixelBufferWidthKey: @(width), (id)kCVPixelBufferHeightKey: @(height),
            (id)kCVPixelBufferMetalCompatibilityKey: @YES,
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        if (CVPixelBufferPoolCreate(NULL, NULL, (CFDictionaryRef)attributes, &p->pool) != kCVReturnSuccess)
            return -1;
        p->pool_width = width; p->pool_height = height;
    }
    NSDictionary *limits = @{ (id)kCVPixelBufferPoolAllocationThresholdKey: @(HDR_POOL_BUFFERS) };
    CVReturn result = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(NULL, p->pool,
                                (CFDictionaryRef)limits, &job->buffer);
    if (result == kCVReturnWouldExceedAllocationThreshold)
        return 0;
    if (result != kCVReturnSuccess)
        return -1;
    CVPixelBufferRef input = job->frame.pixel_buffer;
    if (CVMetalTextureCacheCreateTextureFromImage(NULL, p->texture_cache, input, NULL,
            MTLPixelFormatRGBA16Float, width, height, 0, &job->input_view) != kCVReturnSuccess ||
        CVMetalTextureCacheCreateTextureFromImage(NULL, p->texture_cache, job->buffer, NULL,
            MTLPixelFormatRGBA16Float, width, height, 0, &job->output_view) != kCVReturnSuccess)
        return -1;
    job->peak = [p->device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
    if (!job->peak)
        return -1;
    *(uint32_t *)job->peak.contents = 0;
    job->command = [[p->queue commandBuffer] retain];
    id<MTLComputeCommandEncoder> encoder = [job->command computeCommandEncoder];
    if (!encoder)
        return -1;
    [encoder setComputePipelineState:p->normalize];
    [encoder setTexture:CVMetalTextureGetTexture(job->input_view) atIndex:0];
    [encoder setTexture:CVMetalTextureGetTexture(job->output_view) atIndex:1];
    [encoder setBuffer:job->peak offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake((width + 15) / 16, (height + 15) / 16, 1)
             threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [encoder endEncoding];
    [job->command commit];
    return 1;
}

static MP_THREAD_VOID poll_worker(void *argument)
{
    struct mp_filter *f = argument;
    struct priv *p = f->priv;
    struct hdr_gpu_job job = {0};
    mp_thread_set_name("mpv-hdr-output");
    while (true) {
        @autoreleasepool {
            mp_mutex_lock(&p->lock);
            bool stop = p->stop;
            bool room = p->result_count < HDR_SLOTS;
            bool idle = !job.lease && p->active_work <= p->result_count;
            if (!stop && idle) {
                mp_cond_wait(&p->wakeup, &p->lock);
                mp_mutex_unlock(&p->lock);
                continue;
            }
            mp_mutex_unlock(&p->lock);
            if (stop) {
                if (job.command && job.command.status < MTLCommandBufferStatusCompleted) {
                    // The libmpv core shuts down off AppKit's main thread. Drain
                    // this last GPU consumer before releasing its CV/Metal owners.
                    mp_sleep_ns(MP_TIME_MS_TO_NS(2));
                    continue;
                }
                release_gpu_job(&job);
                MP_THREAD_RETURN();
            }

            if (!job.lease && room) {
                fe_output *output = NULL;
                if (fe_session_poll(p->session, &output) == FE_ACCEPTED) {
                    job.lease = output;
                    job.frame = *fe_output_frame(output);
                }
            }
            if (job.lease && !job.command) {
                if (job.frame.generation != fe_session_generation(p->session)) {
                    release_gpu_job(&job);
                } else if (start_normalization(p, &job) < 0) {
                    release_gpu_job(&job);
                    fail_worker(f, "Failed to allocate or encode the bounded HDR normalization pass");
                    MP_THREAD_RETURN();
                }
            }
            if (job.command && job.command.status >= MTLCommandBufferStatusCompleted) {
                if (job.command.status == MTLCommandBufferStatusError) {
                    release_gpu_job(&job);
                    fail_worker(f, "Metal HDR normalization command failed");
                    MP_THREAD_RETURN();
                }
                if (job.frame.generation == fe_session_generation(p->session)) {
                    struct hdr_result result = {
                        .buffer = job.buffer, .frame = job.frame,
                        .normalized_host = CACurrentMediaTime(),
                        .normalization_gpu_seconds = job.command.GPUEndTime - job.command.GPUStartTime,
                    };
                    memcpy(&result.peak_nits, job.peak.contents, sizeof(float));
                    fe_session_record_transfers(p->session, job.frame.generation, job.frame.frame_id, 1, 1, 0);
                    job.buffer = NULL;
                    mp_mutex_lock(&p->lock);
                    p->results[p->result_count++] = result;
                    mp_mutex_unlock(&p->lock);
                    mp_filter_wakeup(f);
                }
                release_gpu_job(&job);
            }
            fe_statistics statistics = {0};
            fe_session_statistics(p->session, &statistics);
            if (statistics.failures && !job.command) {
                char error[512];
                fe_session_error(p->session, error, sizeof(error));
                release_gpu_job(&job);
                fail_worker(f, error);
                MP_THREAD_RETURN();
            }
            mp_mutex_lock(&p->lock);
            if (!p->stop)
                mp_cond_timedwait(&p->wakeup, &p->lock, MP_TIME_MS_TO_NS(2));
            mp_mutex_unlock(&p->lock);
        }
    }
}

static void reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    uint64_t generation = fe_session_reset(p->session);
    atomic_store((_Atomic uint64_t *)p->generation->data, generation);
    mp_frame_unref(&p->held_input);
    for (int n = 0; n < p->pending_count; n++)
        talloc_free(p->pending[n].image);
    p->pending_count = 0;
    p->eof = false;
    p->have_input_params = false;
    mp_mutex_lock(&p->lock);
    for (int n = 0; n < p->result_count; n++)
        release_result(&p->results[n]);
    p->result_count = 0;
    p->active_work = 0;
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);
    p->resets++;
    MP_VERBOSE(f, "HDR generation reset to %llu\n", (unsigned long long)generation);
}

static bool source_colour(struct priv *p, struct mp_image *image, fe_colour *colour)
{
    const struct mp_image_params *params = &image->params;
    *colour = (fe_colour){ .reference_white_nits = p->opts->reference_white,
                           .hlg_peak_nits = p->opts->hlg_peak };
    switch (params->color.primaries) {
    case PL_COLOR_PRIM_BT_2020: colour->primaries = FE_BT2020; break;
    case PL_COLOR_PRIM_BT_709: colour->primaries = FE_BT709_PRIMARIES; break;
    case PL_COLOR_PRIM_DISPLAY_P3: colour->primaries = FE_DISPLAY_P3; break;
    default: return false;
    }
    switch (params->color.transfer) {
    case PL_COLOR_TRC_PQ: colour->transfer = FE_PQ; break;
    case PL_COLOR_TRC_HLG: colour->transfer = FE_HLG; break;
    case PL_COLOR_TRC_SRGB: colour->transfer = FE_SRGB; break;
    case PL_COLOR_TRC_BT_1886: colour->transfer = FE_BT709; break;
    case PL_COLOR_TRC_LINEAR: colour->transfer = FE_LINEAR; break;
    default: return false;
    }
    switch (params->repr.sys) {
    case PL_COLOR_SYSTEM_BT_2020_NC: colour->matrix = FE_YUV2020; break;
    case PL_COLOR_SYSTEM_BT_709: colour->matrix = FE_YUV709; break;
    case PL_COLOR_SYSTEM_BT_601: colour->matrix = FE_YUV601; break;
    case PL_COLOR_SYSTEM_RGB: colour->matrix = FE_RGB; break;
    default: return false; // Dolby Vision reshaping is not generic PQ conversion.
    }
    colour->range = params->repr.levels == PL_COLOR_LEVELS_FULL ? FE_FULL_RANGE : FE_VIDEO_RANGE;
    switch (params->chroma_location) {
    case PL_CHROMA_CENTER: colour->chroma_location = 0; break;
    case PL_CHROMA_LEFT: colour->chroma_location = 1; break;
    case PL_CHROMA_TOP_LEFT: colour->chroma_location = 2; break;
    case PL_CHROMA_TOP_CENTER: colour->chroma_location = 3; break;
    case PL_CHROMA_BOTTOM_LEFT: colour->chroma_location = 4; break;
    case PL_CHROMA_BOTTOM_CENTER: colour->chroma_location = 5; break;
    default: return false;
    }
    const struct pl_hdr_metadata *hdr = &params->color.hdr;
    const struct pl_raw_primaries *prim = &hdr->prim;
    double xy[] = {prim->red.x, prim->red.y, prim->green.x, prim->green.y,
                   prim->blue.x, prim->blue.y, prim->white.x, prim->white.y};
    memcpy(colour->mastering_xy, xy, sizeof(xy));
    colour->mastering_min_nits = hdr->min_luma;
    colour->mastering_max_nits = hdr->max_luma;
    colour->max_cll = hdr->max_cll; colour->max_fall = hdr->max_fall;
    return true;
}

static bool descriptor(struct mp_filter *f, struct mp_image *image, fe_frame *frame)
{
    struct priv *p = f->priv;
    CVPixelBufferRef buffer = (CVPixelBufferRef)image->planes[3];
    if (image->imgfmt != IMGFMT_VIDEOTOOLBOX || !buffer || image->source_pts == AV_NOPTS_VALUE ||
        image->source_timebase_num <= 0 || image->source_timebase_den <= 0)
        return false;
    AVRational timebase = {image->source_timebase_num, image->source_timebase_den};
    double exact_pts = image->source_pts * av_q2d(timebase);
    // Filters that changed timeline semantics must provide a new rational identity.
    if (fabs(exact_pts - image->pts) > 1e-9)
        return false;
    int64_t pts;
    if (__builtin_mul_overflow(image->source_pts, (int64_t)timebase.num, &pts))
        return false;
    int64_t duration;
    if (image->source_duration > 0) {
        if (__builtin_mul_overflow(image->source_duration, (int64_t)timebase.num, &duration))
            return false;
    } else {
        // mp_compute_frame_duration/decoder FPS may establish duration after decode.
        double seconds = image->pkt_duration > 0 ? image->pkt_duration :
                         image->nominal_fps > 0 ? 1 / image->nominal_fps : 0;
        duration = llround(seconds * timebase.den);
        if (duration <= 0 || fabs((double)duration / timebase.den - seconds) > 1e-9)
            return false;
    }
    *frame = (fe_frame){ .struct_size = sizeof(*frame), .abi_version = FE_ABI_VERSION,
        .source_id = 1, .stream_id = 1, .frame_id = p->next_frame,
        .generation = fe_session_generation(p->session), .pts = {pts, timebase.den},
        .duration = {duration, timebase.den}, .pixel_buffer = buffer,
        .pixel_format = CVPixelBufferGetPixelFormatType(buffer),
        .plane_count = (uint32_t)CVPixelBufferGetPlaneCount(buffer),
        .geometry = { .width = image->w, .height = image->h,
            .crop_x = image->params.crop.x0, .crop_y = image->params.crop.y0,
            .crop_width = mp_rect_w(image->params.crop), .crop_height = mp_rect_h(image->params.crop),
            .rotation_degrees = image->params.rotate,
            .pixel_aspect_num = image->params.p_w > 0 ? image->params.p_w : 1,
            .pixel_aspect_den = image->params.p_h > 0 ? image->params.p_h : 1 },
    };
    if (!source_colour(p, image, &frame->colour) || frame->plane_count > 3)
        return false;
    for (int n = 0; n < frame->plane_count; n++) {
        frame->planes[n] = (fe_plane){ .width = CVPixelBufferGetWidthOfPlane(buffer, n),
            .height = CVPixelBufferGetHeightOfPlane(buffer, n),
            .bytes_per_row = CVPixelBufferGetBytesPerRowOfPlane(buffer, n) };
    }
    return true;
}

static void release_pixel_buffer(void *buffer)
{
    CVPixelBufferRelease(buffer);
}

static bool configure_measurements(struct mp_filter *f, struct mp_image *image)
{
    struct priv *p = f->priv;
    if (p->measurements_configured)
        return true;
    NSData *data;
    if (p->opts->measurement_config && p->opts->measurement_config[0]) {
        data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:p->opts->measurement_config]];
    } else {
        int display[2] = {0};
        struct mp_stream_info *info = mp_filter_find_stream_info(f);
        if (info && info->get_display_res)
            info->get_display_res(info, display);
        NSDictionary *settings = @{ @"strength": @(p->opts->strength), @"colourStrength": @(p->opts->colour_strength),
            @"referenceWhiteNits": @(p->opts->reference_white), @"maximumLuminanceRatio": @(p->opts->maximum_luminance_ratio) };
        NSData *settings_data = [NSJSONSerialization dataWithJSONObject:settings options:0 error:NULL];
        NSString *settings_json = [[[NSString alloc] initWithData:settings_data encoding:NSUTF8StringEncoding] autorelease];
        NSDictionary *configuration = @{ @"adapter": @"mpv-metal-hdr", @"source": @"caller-unreported",
            @"sourceWidth": @(image->w), @"sourceHeight": @(image->h),
            @"processingWidth": @(p->opts->processing_width), @"processingHeight": @(p->opts->processing_height),
            @"displayWidth": @(display[0]), @"displayHeight": @(display[1]), @"sourceFPS": @(image->nominal_fps),
            @"modelVersion": p->opts->model && p->opts->model[0] ? [NSString stringWithUTF8String:p->opts->model] : @"original",
            @"implementationRevision": @"mpv-metal-hdr-working-tree", @"settingsJSON": settings_json,
            @"warmupFrames": @3, @"displayConfiguration": @"gpu-next/macvk; physical screen dimensions; drawable and display settings caller-unreported",
            @"powerConfiguration": @"caller-unreported" };
        data = [NSJSONSerialization dataWithJSONObject:configuration options:0 error:NULL];
    }
    if (!data || data.length > 65536)
        return false;
    NSString *json = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    if (fe_session_measurements_configure(p->session, json.UTF8String) != FE_ACCEPTED)
        return false;
    p->measurements_configured = true;
    return true;
}

static void process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;
    mp_mutex_lock(&p->lock);
    if (p->failed) {
        MP_ERR(f, "HDR engine failed: %s\n", p->error);
        mp_mutex_unlock(&p->lock);
        mp_filter_internal_mark_failed(f);
        return;
    }
    struct hdr_result output = {0};
    if (p->result_count) {
        output = p->results[0];
        memmove(p->results, p->results + 1, --p->result_count * sizeof(p->results[0]));
    }
    mp_mutex_unlock(&p->lock);
    if (output.buffer) {
        int match = -1;
        for (int n = 0; n < p->pending_count; n++)
            if (p->pending[n].frame_id == output.frame.frame_id) match = n;
        if (match >= 0 && output.frame.generation == fe_session_generation(p->session)) {
            struct hdr_pending pending = p->pending[match];
            memmove(p->pending + match, p->pending + match + 1,
                    (--p->pending_count - match) * sizeof(p->pending[0]));
            struct mp_image template = {0};
            mp_image_sethwfmt(&template, IMGFMT_VIDEOTOOLBOX, IMGFMT_RGBAF16);
            mp_image_set_size(&template, output.frame.geometry.width, output.frame.geometry.height);
            template.planes[3] = (uint8_t *)output.buffer;
            struct mp_image *image = mp_image_new_custom_ref(&template, output.buffer, release_pixel_buffer);
            output.buffer = NULL;
            mp_image_copy_attributes(image, pending.image);
            // ICC, dynamic metadata and film grain describe the unprocessed source.
            // Applying them again would reinterpret or modify the reconstructed RGB.
            av_buffer_unref(&image->icc_profile);
            av_buffer_unref(&image->dovi);
            av_buffer_unref(&image->film_grain);
            for (int n = 0; n < image->num_ff_side_data; n++)
                av_buffer_unref(&image->ff_side_data[n].buf);
            image->num_ff_side_data = 0;
            mp_image_unrefp(&image->enhancement_layer);
            image->params.no_dovi = true;
            image->params.no_enhancement_layer = true;
            image->params.color = (struct pl_color_space){ .primaries = PL_COLOR_PRIM_BT_2020,
                .transfer = PL_COLOR_TRC_LINEAR,
                .hdr = {.min_luma = PL_COLOR_HDR_BLACK, .max_luma = MPMAX(203, output.peak_nits),
                        .max_cll = output.peak_nits} };
            image->params.repr = (struct pl_color_repr){ .sys = PL_COLOR_SYSTEM_RGB,
                .levels = PL_COLOR_LEVELS_FULL, .alpha = PL_ALPHA_INDEPENDENT };
            image->params.primaries_orig = PL_COLOR_PRIM_BT_2020;
            image->params.transfer_orig = PL_COLOR_TRC_LINEAR;
            image->params.sys_orig = PL_COLOR_SYSTEM_RGB;
            image->params.light = MP_CSP_LIGHT_DISPLAY;
            image->params.chroma_location = PL_CHROMA_CENTER;
            av_buffer_unref(&image->async_generation);
            image->async_generation = av_buffer_ref(p->generation);
            image->async_frame_generation = output.frame.generation;
            talloc_free(pending.image);
            p->emitted++;
            mp_mutex_lock(&p->lock);
            p->active_work--;
            mp_cond_signal(&p->wakeup);
            mp_mutex_unlock(&p->lock);
            if (p->measurements) {
                fprintf(p->measurements, "{\"event\":\"filter-output\",\"frame\":%llu,\"generation\":%llu,"
                    "\"pts_value\":%lld,\"pts_timescale\":%d,\"submitted_host\":%.9f,\"completed_host\":%.9f,"
                    "\"normalization_gpu_seconds\":%.9f,\"peak_nits\":%.6f,\"gpu_copies\":1,\"cpu_readback_bytes\":4}\n",
                    (unsigned long long)output.frame.frame_id, (unsigned long long)output.frame.generation,
                    (long long)output.frame.pts.value, output.frame.pts.timescale,
                    pending.submitted_host, output.normalized_host, output.normalization_gpu_seconds, output.peak_nits);
                fflush(p->measurements);
            }
            mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, image));
            return;
        }
        release_result(&output);
        mp_filter_internal_mark_progress(f);
    }
    if (p->eof) {
        if (!p->pending_count) {
            mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
            p->eof = false;
        }
        return;
    }
    if (p->pending_count >= HDR_SLOTS)
        return;
    if (!p->held_input.type)
        p->held_input = mp_pin_out_read(f->ppins[0]);
    if (!p->held_input.type)
        return;
    if (p->held_input.type == MP_FRAME_EOF) {
        mp_frame_unref(&p->held_input);
        p->eof = true;
        mp_filter_internal_mark_progress(f);
        return;
    }
    if (p->held_input.type != MP_FRAME_VIDEO) {
        mp_pin_in_write(f->ppins[1], p->held_input);
        p->held_input = MP_NO_FRAME;
        return;
    }
    struct mp_image *image = p->held_input.data;
    if (p->opts->bypass) {
        mp_pin_in_write(f->ppins[1], p->held_input);
        p->held_input = MP_NO_FRAME;
        return;
    }
    if (p->have_input_params && !mp_image_params_static_equal(&p->input_params, &image->params)) {
        struct mp_frame held = p->held_input;
        p->held_input = MP_NO_FRAME;
        reset(f);
        p->held_input = held;
    }
    p->input_params = image->params;
    p->have_input_params = true;
    fe_frame frame;
    if (!descriptor(f, image, &frame) || !configure_measurements(f, image)) {
        MP_ERR(f, "metal-hdr requires VideoToolbox NV12/P010 input, supported colour metadata and unmodified rational decoder PTS\n");
        mp_filter_internal_mark_failed(f);
        return;
    }
    fe_status status = fe_session_submit(p->session, &frame);
    if (status == FE_FULL)
        return;
    if (status != FE_ACCEPTED) {
        MP_ERR(f, "HDR submission rejected with status %d\n", status);
        mp_filter_internal_mark_failed(f);
        return;
    }
    p->pending[p->pending_count++] = (struct hdr_pending){ .frame_id = p->next_frame++,
        .image = image, .submitted_host = CACurrentMediaTime() };
    p->held_input = MP_NO_FRAME;
    mp_mutex_lock(&p->lock);
    p->active_work++;
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);
    mp_filter_internal_mark_progress(f);
}

static bool command(struct mp_filter *f, struct mp_filter_command *command)
{
    struct priv *p = f->priv;
    if (command->type != MP_FILTER_COMMAND_TEXT || strcmp(command->cmd, "bypass"))
        return false;
    bool bypass;
    if (!strcmp(command->arg, "yes")) bypass = true;
    else if (!strcmp(command->arg, "no")) bypass = false;
    else return false;
    if (bypass != p->opts->bypass) {
        reset(f);
        p->opts->bypass = bypass;
        mp_filter_wakeup(f);
    }
    return true;
}

static void destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->generation)
        atomic_fetch_add((_Atomic uint64_t *)p->generation->data, 1);
    if (p->session)
        fe_session_close(p->session);
    mp_mutex_lock(&p->lock);
    p->stop = true;
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);
    if (p->thread_started)
        mp_thread_join(p->thread);
    if (p->session) {
        // Process/dylib teardown must not race MLX's global Metal destruction.
        // Admission and queued generations are already cancelled; only genuinely
        // running work remains. This occurs on mpv's core thread, not AppKit.
        while (!fe_session_is_idle(p->session))
            mp_sleep_ns(MP_TIME_MS_TO_NS(2));
        fe_statistics statistics = {0};
        fe_session_statistics(p->session, &statistics);
        MP_INFO(f, "HDR engine: submitted=%llu completed=%llu emitted=%llu resets=%llu peak_slots=%u peak_bytes=%llu\n",
                (unsigned long long)statistics.submitted, (unsigned long long)statistics.completed,
                (unsigned long long)p->emitted, (unsigned long long)p->resets,
                statistics.peak_slots, (unsigned long long)statistics.peak_retained_bytes);
        if (p->opts->engine_report && p->opts->engine_report[0]) {
            size_t capacity = fe_session_measurements_json(p->session, NULL, 0);
            if (capacity && capacity <= 16 * 1024 * 1024) {
                char *json = malloc(capacity);
                size_t required = fe_session_measurements_json(p->session, json, capacity);
                FILE *report = required <= capacity ? fopen(p->opts->engine_report, "w") : NULL;
                if (report) { fputs(json, report); fputc('\n', report); fclose(report); }
                free(json);
            }
        }
        fe_session_destroy(p->session);
    }
    mp_frame_unref(&p->held_input);
    for (int n = 0; n < p->pending_count; n++) talloc_free(p->pending[n].image);
    for (int n = 0; n < p->result_count; n++) release_result(&p->results[n]);
    av_buffer_unref(&p->generation);
    if (p->measurements) fclose(p->measurements);
    if (p->texture_cache) CFRelease(p->texture_cache);
    if (p->pool) CFRelease(p->pool);
    [p->normalize release]; [p->queue release]; [p->device release];
    mp_cond_destroy(&p->wakeup); mp_mutex_destroy(&p->lock);
}

static const struct mp_filter_info filter_info = {
    .name = "metal-hdr", .priv_size = sizeof(struct priv), .process = process,
    .reset = reset, .destroy = destroy, .command = command,
};

static struct mp_filter *create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &filter_info);
    if (!f) { talloc_free(options); return NULL; }
    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    mp_mutex_init(&p->lock); mp_cond_init(&p->wakeup);
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    fe_config config = { .struct_size = sizeof(config), .abi_version = FE_ABI_VERSION,
        .max_in_flight = HDR_SLOTS, .memory_limit_bytes = UINT64_C(512) * 1024 * 1024,
        .processing_width = p->opts->processing_width, .processing_height = p->opts->processing_height,
        .reference_white_nits = p->opts->reference_white, .effect_strength = p->opts->strength,
        .colour_strength = p->opts->colour_strength, .maximum_luminance_ratio = p->opts->maximum_luminance_ratio,
        .model_path = p->opts->model && p->opts->model[0] ? p->opts->model : NULL,
        .model_version = "mpv-metal-hdr-v1" };
    p->session = fe_session_create(&config, p->error, sizeof(p->error));
    if (!p->session) { MP_ERR(f, "%s\n", p->error); goto error; }
    p->generation = av_buffer_allocz(sizeof(_Atomic uint64_t));
    if (!p->generation) goto error;
    atomic_init((_Atomic uint64_t *)p->generation->data, fe_session_generation(p->session));
    p->device = MTLCreateSystemDefaultDevice();
    p->queue = [p->device newCommandQueue];
    NSError *error = nil;
    id<MTLLibrary> library = [p->device newLibraryWithSource:normalization_shader options:nil error:&error];
    if (!library) {
        MP_ERR(f, "HDR normalization shader compilation failed: %s\n", error.localizedDescription.UTF8String);
        goto error;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"normalize_nits"];
    if (!function) { [library release]; goto error; }
    p->normalize = [p->device newComputePipelineStateWithFunction:function error:&error];
    [function release]; [library release];
    if (!p->normalize || CVMetalTextureCacheCreate(NULL, NULL, p->device, NULL, &p->texture_cache) != kCVReturnSuccess) {
        MP_ERR(f, "HDR normalization setup failed: %s\n", error.localizedDescription.UTF8String);
        goto error;
    }
    if (p->opts->measurements && p->opts->measurements[0]) {
        p->measurements = fopen(p->opts->measurements, "w");
        if (!p->measurements) { MP_ERR(f, "Cannot open HDR measurements file\n"); goto error; }
    }
    if (mp_thread_create(&p->thread, poll_worker, f)) goto error;
    p->thread_started = true;
    MP_INFO(f, "Persistent HDR engine active (%s); three slots; RGBA16F/BT.2020 output, one GPU unit-normalization pass\n",
            config.model_path ? "neural model" : "HDR original");
    return f;
error:
    talloc_free(f);
    return NULL;
}

#define OPT_BASE_STRUCT struct hdr_options
static const m_option_t options[] = {
    {"model", OPT_STRING(model)}, {"measurements", OPT_STRING(measurements)},
    {"measurement-config", OPT_STRING(measurement_config)}, {"engine-report", OPT_STRING(engine_report)},
    {"processing-width", OPT_INT(processing_width), M_RANGE(16, 8192)},
    {"processing-height", OPT_INT(processing_height), M_RANGE(16, 8192)},
    {"strength", OPT_DOUBLE(strength), M_RANGE(0, 1)},
    {"colour-strength", OPT_DOUBLE(colour_strength), M_RANGE(0, 1)},
    {"reference-white", OPT_DOUBLE(reference_white), M_RANGE(1, 1000)},
    {"hlg-peak", OPT_DOUBLE(hlg_peak), M_RANGE(400, 2000)},
    {"maximum-luminance-ratio", OPT_DOUBLE(maximum_luminance_ratio), M_RANGE(1, 16)},
    {"bypass", OPT_BOOL(bypass)}, {0},
};

const struct mp_user_filter_entry vf_metal_hdr = {
    .desc = { .name = "metal-hdr", .description = "asynchronous shared Metal HDR engine",
        .priv_size = sizeof(struct hdr_options), .options = options,
        .priv_defaults = &(const struct hdr_options){ .processing_width = 320, .processing_height = 192,
            .strength = 1, .colour_strength = 1, .reference_white = 203, .hlg_peak = 1000,
            .maximum_luminance_ratio = 2 } },
    .create = create,
};
