/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#import <QuartzCore/QuartzCore.h>

#include <math.h>

#include "video/out/gpu/context.h"
#include "osdep/mac/swift.h"

#include "common.h"
#include "context.h"
#include "utils.h"

struct priv {
    struct mpvk_ctx vk;
    MacCommon *vo_mac;
    // All metadata ownership/writes stay on the serialized VO thread. Keep the
    // layer alive until Vulkan has finished with it, before MacCommon teardown.
    CAMetalLayer *layer;
    CGColorSpaceRef linear_colorspace;
    CAEDRMetadata *linear_metadata;
    uint64_t color_revision;
    float linear_minimum;
    float linear_maximum;
    bool linear_hdr_active;
    bool linear_contract_failed;
};

static void mac_vk_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    ra_vk_ctx_uninit(ctx);
    mpvk_uninit(&p->vk);
    [p->linear_metadata release];
    [p->layer release];
    CGColorSpaceRelease(p->linear_colorspace);
    [p->vo_mac uninit:ctx->vo];
}

static void mac_vk_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    [p->vo_mac swapBuffer];
}

static void mac_vk_get_vsync(struct ra_ctx *ctx, struct vo_vsync_info *info)
{
    struct priv *p = ctx->priv;
    [p->vo_mac fillVsyncWithInfo:info];
}

static int mac_vk_color_depth(struct ra_ctx *ctx)
{
    return 0;
}

static void mac_vk_clear_linear_metadata(struct priv *p)
{
    // Layer access is supported on the rendering thread. Explicitly commit on
    // this thread (which has no Core Animation run loop), without a main-queue
    // round trip. Never hold this transaction lock while calling Vulkan.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [CATransaction lock];
    if (p->layer.EDRMetadata)
        p->layer.EDRMetadata = nil;
    [CATransaction unlock];
    [CATransaction commit];
    [p->linear_metadata release];
    p->linear_metadata = nil;
    p->linear_hdr_active = false;
}

static bool mac_vk_set_color(struct ra_ctx *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    bool linear_bt2020 = params &&
        params->color.transfer == PL_COLOR_TRC_LINEAR &&
        params->color.primaries == PL_COLOR_PRIM_BT_2020 &&
        params->repr.sys == PL_COLOR_SYSTEM_RGB &&
        params->repr.levels == PL_COLOR_LEVELS_FULL;

    if (!linear_bt2020) {
        // Remove our optical units before Vulkan resumes PQ/HLG ownership.
        // Also remove stale PQ metadata on an SDR transition: MoltenVK's SDR
        // branch, like its linear branch, does not clear that metadata.
        if (p->linear_hdr_active || !params || !pl_color_space_is_hdr(&params->color))
            mac_vk_clear_linear_metadata(p);
        p->linear_hdr_active = false;
        return false;
    }

    // Keep Vulkan's normal BT.2020-linear surface/format selection. Returning
    // external parameters preserves the source HDR range, which libplacebo's
    // transfer-only HDR metadata gate otherwise discards for a linear target.
    pl_color_space_infer(&params->color);
    float minimum = params->color.hdr.min_luma;
    float maximum = params->color.hdr.max_luma;
    if (!isfinite(minimum) || !isfinite(maximum) || minimum < 0 || maximum <= minimum)
        return false;
    pl_swapchain_colorspace_hint(p->vk.swapchain, &params->color);

    // Complete any pending colour/format recreation BEFORE writing metadata.
    // Zero dimensions preserve the current size. An unchanged swapchain only
    // takes libplacebo's mutex; recreation creates image wrappers but does not
    // acquire a drawable. Apple's edrMetadata contract requires assignment
    // before nextDrawable, which happens later in pl_swapchain_start_frame.
    int width = 0, height = 0;
    if (!pl_swapchain_resize(p->vk.swapchain, &width, &height) || width < 1 || height < 1)
        return false;

    uint64_t revision = p->vo_mac.displayColorRevision;
    bool configured = false;
    @autoreleasepool {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [CATransaction lock];
        CGColorSpaceRef colorspace = p->layer.colorspace;
        configured = p->layer.pixelFormat == MTLPixelFormatRGBA16Float &&
            colorspace && CFEqual(colorspace, p->linear_colorspace) &&
            p->layer.wantsExtendedDynamicRangeContent;
        if (configured && (!p->linear_hdr_active || revision != p->color_revision ||
            minimum != p->linear_minimum || maximum != p->linear_maximum ||
            p->layer.EDRMetadata != p->linear_metadata)) {
            // PL_HDR_NORM and normalized engine RGB both use 1.0 = 203 nits.
            // A fresh object also refreshes Core Animation on display changes.
            CAEDRMetadata *metadata = [[CAEDRMetadata
                HDR10MetadataWithMinLuminance:minimum maxLuminance:maximum
                opticalOutputScale:PL_COLOR_SDR_WHITE] retain];
            if (metadata) {
                p->layer.EDRMetadata = metadata;
                [p->linear_metadata release];
                p->linear_metadata = metadata;
                p->linear_minimum = minimum;
                p->linear_maximum = maximum;
                p->color_revision = revision;
            } else {
                configured = false;
            }
        }
        [CATransaction unlock];
        [CATransaction commit];
    }
    if (!configured)
        mac_vk_clear_linear_metadata(p);
    if (!configured && !p->linear_contract_failed)
        MP_WARN(ctx, "Linear HDR layer contract unavailable after swapchain recreation.\n");
    p->linear_contract_failed = !configured;
    p->linear_hdr_active = configured;
    return configured;
}

static bool mac_vk_check_visible(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    return [p->vo_mac isVisible];
}

static bool mac_vk_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    struct mpvk_ctx *vk = &p->vk;
    int msgl = ctx->opts.probing ? MSGL_V : MSGL_ERR;

    if (!NSApp) {
        MP_ERR(ctx, "Failed to initialize macvk context, no NSApplication initialized.\n");
        goto error;
    }

    if (!mpvk_init(vk, ctx, VK_EXT_METAL_SURFACE_EXTENSION_NAME))
        goto error;

    p->vo_mac = [[MacCommon alloc] init:ctx->vo];
    if (!p->vo_mac)
        goto error;

    p->layer = [p->vo_mac.layer retain];
    p->linear_colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
    if (!p->layer || !p->linear_colorspace)
        goto error;

    VkMetalSurfaceCreateInfoEXT mac_info = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pNext = NULL,
        .flags = 0,
        .pLayer = p->layer,
    };

    struct ra_ctx_params params = {
        .swap_buffers = mac_vk_swap_buffers,
        .get_vsync = mac_vk_get_vsync,
        .color_depth = mac_vk_color_depth,
        .check_visible = mac_vk_check_visible,
        .set_color = mac_vk_set_color,
    };

    VkInstance inst = vk->vkinst->instance;
    VkResult res = vkCreateMetalSurfaceEXT(inst, &mac_info, NULL, &vk->surface);
    if (res != VK_SUCCESS) {
        MP_MSG(ctx, msgl, "Failed creating metal surface\n");
        goto error;
    }

    if (!ra_vk_ctx_init(ctx, vk, params, VK_PRESENT_MODE_FIFO_KHR))
        goto error;

    return true;
error:
    [p->layer release];
    p->layer = nil;
    CGColorSpaceRelease(p->linear_colorspace);
    p->linear_colorspace = NULL;
    if (p->vo_mac)
        [p->vo_mac uninit:ctx->vo];
    return false;
}

static bool resize(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    CGSize size = p->vo_mac.surfaceSize;
    if (size.width < 1 || size.height < 1)
        return true; // A hidden/temporarily detached host retains its last swapchain.

    return ra_vk_ctx_resize(ctx, (int)size.width, (int)size.height);
}

static bool mac_vk_reconfig(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (![p->vo_mac config:ctx->vo])
        return false;

    [p->vo_mac updateWithAlpha:ctx->opts.want_alpha];

    return true;
}

static int mac_vk_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    struct priv *p = ctx->priv;
    int ret = [p->vo_mac control:ctx->vo events:events request:request data:arg];

    if (*events & VO_EVENT_RESIZE) {
        if (!resize(ctx))
            return VO_ERROR;
    }

    return ret;
}

static void mac_vk_update_render_opts(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    [p->vo_mac updateWithAlpha:ctx->opts.want_alpha];
}

const struct ra_ctx_fns ra_ctx_vulkan_mac = {
    .type               = "vulkan",
    .name               = "macvk",
    .description        = "mac/Vulkan (via Metal)",
    .reconfig           = mac_vk_reconfig,
    .control            = mac_vk_control,
    .update_render_opts = mac_vk_update_render_opts,
    .init               = mac_vk_init,
    .uninit             = mac_vk_uninit,
};
