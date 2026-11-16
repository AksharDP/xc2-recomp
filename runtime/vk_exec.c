// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_draw.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop (END / BEGIN / JUMP / CALL, which ops are pure state, the state defaults, render targets kept until they
// change, clears skipped while rendering is disabled) follows MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:10588-10700, 7409-7460, 5449-5457
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou). Pass begin and end (GENERAL-layout attachments, LOAD/STORE, render area from the smallest attachment, a
// full barrier before each pass), the scissor clip and attachment clears follow LAS Recomp
// runtime/nvn/vulkan/exec.c:855-1149, 3553-3778 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
// The replay state this thread records with, while it is inside vk_exec_record (vk_exec_inline_begin): X on the replay
// thread. Per thread, so a thread that records a part of the replay into a command buffer of its own reaches its own.
static _Thread_local VkExec *x_cur;

// ---- XC2_TRACE=gfx frame counters

VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_cur ? x_cur->fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu copies %llu reused %llu "
           "(snapshot draws %llu, images new %llu evicted %llu, draws skipped %llu, targets read live %llu); targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, (unsigned long long)f->fb_snaps, (unsigned long long)f->fb_snap_reuse,
           (unsigned long long)f->fb_snap_draws, (unsigned long long)f->snap_new, (unsigned long long)f->snap_evicted,
           (unsigned long long)f->snap_skipped, (unsigned long long)f->snap_live, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_STENCIL_MASK, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

// NVN defaults of the replay state (nvn_mtl.m).
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
}

// ---- render targets

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

// A view format other than the texture's is used when both map directly and have the same texel size (the image is
// created MUTABLE_FORMAT for colour formats). blocks: t is a BC texture with block_views, and the view an uncompressed
// format whose texel is one of its blocks.
static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

// ---- 3D BC render targets
// A 3D BC texture has no block views (vk_texture.c create_tex, XC2_BLOCK_VIEWS_3D): on RDNA2 cards such an image made
// BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE took writes past the end of its memory (TotK Recomp issue #5).
// Draws into it through an uncompressed view of its blocks (TotK's agl BC4 encoder fills 32x32x32 and 64x64x64 volumes
// and their levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one layer
// per depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and each
// pass end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed
// format).
typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64   // one per level drawn: 6 volumes of up to 7 levels at the logo
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;   // the replay adds and reads; destroy_tex (any thread) removes

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can
// be made (logged). Replay thread.
static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    // In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};   // texels of the compressed source
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

// vk_exec_draw.c vk_exec_image_gone: the stand-in of a BC image that goes, freed once the GPU is done with it.
void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

// At a pass end: the planes the pass drew into a stand-in go to its texture.
static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};   // texels of the uncompressed source
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

// A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the
// image's stand-in (bc_standin). 1 when o is filled (target or left out), 0 for any other target.
static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;   // not img->image: view_of makes this command buffer's view of the stand-in
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = vk_texture_get(t);
    if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return;   // a 3D BC image: its stand-in
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;   // slices drawn through 2D views (2D_ARRAY_COMPATIBLE)
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here: left out",
                  t->d.format, t->d.target, t->d.width, t->d.height);
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    // A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {   // a BC texture takes draws only through an uncompressed view of its blocks
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {   // an uncompressed view of a BC image has one layer (no maintenance6 here)
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;   // the view's extent: 4x4 blocks
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

/* The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384, as
   MacNativeGames and nxnative size it. XCX draws a fragment shader that only writes a storage buffer into a 1x1
   viewport; a pass sized by the viewport's width alone puts an offset viewport outside the scissor, and the cave rock
   loses its ambient light. */
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

// Resolves ex->rt to this command buffer's images and derives the pass parameters and the pipeline's target formats.
static void resolve_rt(VkExec *ex) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

// The attachment view of a target. A registered texture's view lives as long as its image (vk_exec_target_view); any
// other image's view is made for this command buffer and freed at its fence.
static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {   // completion thread, once the submission that used them is done
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

// ---- target history
// The presented frame (gfx_counters.frames + 1; 0: never) each target subresource was last rendered into, and the one
// before. prepare_draw lets a draw into a target the previous frame did not render (a one-shot draw: a volume or LUT
// generated once) wait for its shaders and pipeline instead of skipping it. Per subresource, not per image: a volume
// may be generated a few slices per frame. Direct-mapped: a slot another subresource took reads as not rendered, so a
// collision makes a draw wait rather than skip.
typedef struct { VkImage image; uint32_t level, layer; uint64_t last, prev; } TargetFrames;
static TargetFrames tfr[1024];

static TargetFrames *tfr_slot(const VkExecTarget *t) {
    uint64_t h = ((uint64_t)(uintptr_t)t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->base_layer << 40) *
                 0x9E3779B97F4A7C15ull;
    return &tfr[h >> 54];
}

static int tfr_match(const TargetFrames *e, const VkExecTarget *t) {
    return e->image == t->image && e->level == t->level && e->layer == t->base_layer;
}

static void tfr_note(const VkExecTarget *t, uint64_t stamp) {
    TargetFrames *e = tfr_slot(t);
    if (!tfr_match(e, t)) *e = (TargetFrames){t->image, t->level, t->base_layer, 0, 0};
    if (e->last != stamp) e->prev = e->last, e->last = stamp;
}

static int tfr_recent(const VkExecTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_slot(t);
    return tfr_match(e, t) && (e->last == stamp ? e->prev : e->last) == stamp - 1;
}

int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    int n = 0, recent = 0;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        n++;
        if (tfr_recent(t, stamp)) recent++;
        else if (!any) return 0;
    }
    return any ? recent > 0 : n > 0;
}

// One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
// (prepare_draw's pipeline wait log).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        const TargetFrames *e = tfr_slot(t);
        int m = tfr_match(e, t);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu", n ? "; " : "", i < 8 ? 'c' : 'd',
                         i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)(m ? e->last : 0), (unsigned long long)(m ? e->prev : 0));
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    vk_exec_pass_end(ex);
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0); }

// The learned targets of the next pass (VkTexImage.fb_learned, attached in the feedback layout): their images and aspects
// go to want[], and bit i of the result is set for attachment i (bit 8 depth).
static uint32_t pass_learned(VkExec *ex, VkImage want[9], VkImageAspectFlags wasp[9], int *nwant) {
    resolve_rt(ex);
    uint32_t fbl = 0;
    *nwant = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, *nwant, t->image)) continue;
        want[*nwant] = t->image, wasp[(*nwant)++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    return fbl;
}

static void pass_open(VkExec *ex, uint32_t fbl);

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = pass_learned(ex, want, wasp, &nwant);
    barrier_to(ex, want, wasp, nwant);   // passes, clears, copies and dispatches see each other's results
    pass_open(ex, fbl);
}

// Begins rendering with the attachments of the open pass; fbl as pass_learned gives it. No barrier: the caller has made
// the earlier work visible.
static void pass_open(VkExec *ex, uint32_t fbl) {
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
}

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    ex->recorded++;
    ex->in_pass = 0;
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)   // stand-ins of 3D BC targets go to their textures
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex), tfr_note(&ex->color[i], stamp);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex), tfr_note(&ex->depth, stamp);
    if (ex->fsr3_armed) vk_fsr3_resolve(ex);
}

// A layout transition of one colour image, with the stages and accesses of its two sides.
static VkImageMemoryBarrier2 snap_layout(VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                                         VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = src_stage,
                                   .srcAccessMask = src_access, .dstStageMask = dst_stage, .dstAccessMask = dst_access,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                        VK_REMAINING_ARRAY_LAYERS}};
}

void vk_exec_snap_copy(VkExec *ex, const VkExecSnap *s, int n, int narrow) {
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = pass_learned(ex, want, wasp, &nwant);
    int same = nwant == ex->nfbl;
    for (int i = 0; same && i < nwant; i++) same = has_image(ex->fbl_img, ex->nfbl, want[i]);
    if (!narrow || !same) {   // the full barrier: copies between it and a pass begin, as before
        vk_exec_barrier(ex);   // ends the pass; what it drew is visible to the copies
        for (int i = 0; i < n; i++) {
            VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {s[i].w, s[i].h, 1}};
            vkCmdCopyImage(ex->cb, s[i].src, VK_IMAGE_LAYOUT_GENERAL, s[i].dst, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            ex->recorded++;
        }
        vk_exec_pass_begin(ex);
        return;
    }
    vk_exec_pass_end(ex);
    // Before the copies: the writes the copies read (attachments of the pass just ended, transfers and dispatches before
    // it, through ALL_COMMANDS), and the reads of the copies' images by the snapshot draws before (the copies overwrite
    // them). A learned source leaves the feedback layout for the copy, which cannot read from it.
    VkImageMemoryBarrier2 ib[8];
    uint32_t nib = 0;
    for (int i = 0; i < n; i++)
        if (has_image(ex->fbl_img, ex->nfbl, s[i].src))
            ib[nib++] = snap_layout(s[i].src, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkMemoryBarrier2 pre = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &pre,
                           .imageMemoryBarrierCount = nib, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    for (int i = 0; i < n; i++) {
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {s[i].w, s[i].h, 1}};
        vkCmdCopyImage(ex->cb, s[i].src, VK_IMAGE_LAYOUT_GENERAL, s[i].dst, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        ex->recorded++;
    }
    // After the copies, before the resumed pass: the copies and every earlier write are visible to the graphics stages
    // (the snapshot draw's samples, and the pass's LOAD of its attachments), and the copies' reads come before later
    // writes to the copied targets. The learned sources return to the feedback layout.
    VkImageMemoryBarrier2 ia[8];
    uint32_t nia = 0;
    for (int i = 0; i < n; i++)
        if (has_image(ex->fbl_img, ex->nfbl, s[i].src))
            ia[nia++] = snap_layout(s[i].src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkMemoryBarrier2 post[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
         .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
         .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
    di = (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 2, .pMemoryBarriers = post,
                            .imageMemoryBarrierCount = nia, .pImageMemoryBarriers = ia};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    vk_exec_frame.fb_layouts += nib + nia;
    pass_open(ex, fbl);   // the learned set is the one fbl_img holds, so nothing else changes state
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = x_cur;
    *reopen = 0;
    if (!ex || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {   // after a vk_exec_inline_begin that returned a command buffer
    x_cur->recorded++;
    if (reopen) vk_exec_pass_begin(x_cur);
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(x_cur ? x_cur->fence : X.fence, free_inline_buf, t);
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t x0 = ex->scissor.x, y0 = ex->scissor.y;
    int64_t x1 = x0 + ex->scissor.w, y1 = y0 + ex->scissor.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too. A stand-in of a 3D BC image (bc_target)
    // clears in a pass as well: only the pass end copies a stand-in back to its BC image (bc_copy_back), so a clear
    // of the stand-in directly would never reach the texture.
    if (ex->in_pass || t->is3d || t->blocks || (t->img && t->img->image != t->image) || t->width != ex->pass_w ||
        t->height != ex->pass_h)
        return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
    if ((a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && a->colorAttachment < 8) {
        // Its copy is stale, and so is the copy of the same image at another colour slot (a clear reads no copy).
        VkImage img = ex->color[a->colorAttachment].image;
        for (uint32_t j = 0; j < 8; j++)
            if ((ex->snap_keep >> j & 1) && (j == a->colorAttachment || (img && ex->snap_src[j] == img))) ex->snap_keep &= ~(1u << j);
    }
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        // Every skip is logged (not once): the second and later targets matter when a stale buffer feeds a decal (H3)
        rt_log("vk: colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        // Every skip is logged (not once): which targets skip matters when a stale alpha or mask buffer is what a
        // decal composites against (issue 22 hypothesis H3)
        rt_log("vk: colour clear of target %d (nvn fmt %d) with channel mask %#x is not replayed: skipped",
               i, ex->color[i].nvn_format, mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    if (ex->render_disabled) return;
    resolve_rt(ex);
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else rt_log("vk: stencil clear with mask %#x is not replayed: skipped", sm);   // every skip, not once (H3)
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once XC2_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value. nxnative commits every 64 draws between passes; MacNativeGames after 30, then every 40.
// A part also spans at least XC2_SPLIT_US (1000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early.
static int split_n = -1, split_draws, part_start;
static uint64_t split_ns, part_t0;
static uint64_t n_parts, n_subs;

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("XC2_SPLIT_DRAWS");
        // With shadow-storage captures the replay has a thread of its own, and each part is a submit ioctl on its
        // time: on the Steam Deck (RADV) a quarter of the replay thread was in ioctl, and one part per submission gave
        // 57.5 fps against 55.5 over a moving camera in town, with no worse frames. Set XC2_SPLIT_DRAWS to split anyway.
        if (!cfg_is_set("XC2_SPLIT_DRAWS") && vk_mem_shadow_captures()) split_n = 0;
        if (split_n < 0) split_n = 0;
        int us = cfg_int("XC2_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        rt_log("vk: replay command buffer split every %d draws between passes, parts of at least %d us%s", split_n,
               us > 0 ? us : 0, split_n ? "" : " (off)");
    }
    if (!split_n || ex->in_pass || split_draws < split_n) return;
    if (split_ns && kern_now_ns() - part_t0 < split_ns) return;   // split_draws stays: the next draw asks again
    split_draws = 0;
    if (ex->recorded == part_start) return;
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
}

// One op other than the control flow (BEGIN, END, JUMP, CALL), which walk and flatten follow.
static void exec_op(VkExec *ex, uint32_t op, const void *a) {
    switch (op) {
        case OP_SET_RT: set_rt(ex, a); break;
        case OP_CLEAR_COLOR: clear_color(ex, a); break;
        case OP_CLEAR_DS: clear_ds(ex, a); break;
        // an unchanged viewport or scissor leaves the command buffer's dynamic state as it is (a new command buffer
        // starts with every bit dirty): games set them again before most draws
        case OP_VIEWPORTS: {
            const p_viewports *v = a;
            int changed = !ex->has_vp;
            for (int i = 0; i < v->n && v->first + i < 16; i++)
                if (v->first + i >= 0 && memcmp(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]))
                    memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]), changed = 1;
            ex->has_vp = 1;
            if (changed) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
            break;
        }
        case OP_SCISSOR: {
            const p_scissor *s = a;
            if (!ex->has_scissor || memcmp(&ex->scissor, s, sizeof *s)) ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
            ex->scissor = *s;
            ex->has_scissor = 1;
            break;
        }
        case OP_DEPTH_RANGE: {
            const p_depth_range *r = a;
            ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
            ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
            break;
        }
        case OP_STENCIL: {
            const p_stencil *s = a;
            for (int f = 0; f < 2; f++) {
                if (!(s->face & (1 << f))) continue;
                if (s->which == 0) ex->st_vmask[f] = (uint32_t)s->value_mask;
                else if (s->which == 1) ex->st_wmask[f] = (uint32_t)s->mask;
                else ex->st_ref[f] = (uint32_t)s->ref;
            }
            ex->dirty |= VK_EXEC_DIRTY_STENCIL;
            break;
        }
        case OP_POLY_OFFSET:
            ex->poly_offset = *(const p_poly_offset *)a;
            ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
            break;
        case OP_BLEND_COLOR:
            memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
            ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
            break;
        case OP_ALPHA_REF: ex->alpha_ref = ((const p_alpha_ref *)a)->ref; break;
        case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
        case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
        case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
        case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
        case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
        case OP_BARRIER: vk_exec_barrier(ex); break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
            if (op == OP_DRAW || op == OP_DRAW_IDX || op == OP_DRAW_IDX_IND) vk_exec_frame.draws_seen++;
            if (vk_dump_armed && op == OP_DRAW) vk_dump_note("replay draw prim %d count %d", ((const p_draw *)a)->prim, ((const p_draw *)a)->count);
            if (vk_dump_armed && op == OP_DRAW_IDX) vk_dump_note("replay draw prim %d count %d", ((const p_draw_idx *)a)->prim, ((const p_draw_idx *)a)->count);
            if (op == OP_DISPATCH || op == OP_DISPATCH_IND) vk_exec_frame.dispatches++;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
        case OP_XFB_BIND: case OP_XFB_ACTIVE:
            vk_exec_op_draw(ex, op, a);
            break;
        case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
            if (op != OP_CLEAR_TEX) vk_exec_frame.copies++;
            if (vk_dump_armed) vk_dump_copy(ex, op, a);
            // fall through
        case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        case OP_CLEAR_BUF:
            vk_exec_op_xfer(ex, op, a);
            break;
        default: rt_fatal("vk: op %s has no replay handler", nvn_op_name(op));
    }
}

static int is_draw_op(uint32_t op) {   // a draw or dispatch: counts toward XC2_SPLIT_DRAWS
    return op == OP_DRAW || op == OP_DRAW_IDX || op == OP_DRAW_IDX_IND || op == OP_DISPATCH || op == OP_DISPATCH_IND;
}

static void check_op(const op_hdr *h) {
    if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
        rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
}

static void walk(VkExec *ex, const op_hdr *h, int depth) {
    for (;;) {
        check_op(h);
        const void *a = nvn_op_payload(h);
        vk_prog_between_ops(&ex->prog);   // a program released meanwhile: its copy from here on
        switch (h->op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        default:
            if (is_draw_op(h->op)) split_draws++;
            exec_op(ex, h->op, a);
        }
        if (split_draws >= split_n) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

static void views_release(VkExec *ex) {   // the transient views go with the submission's fence
    if (!ex->nviews) return;
    ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
    if (!l) rt_fatal("vk: out of memory");
    l->n = ex->nviews;
    for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
    vk_completion_add(ex->fence, free_views, l);
    ex->nviews = 0;
}

// ---- parallel parts (XC2_REPLAY_WORKERS=<n>, an experiment, default 0)
// The ops of a submission are flattened into one list and walked once for their state alone (pre_op): the bound state,
// render targets and fixed-function state each op leaves, without recording. Before a render target change (a pass
// boundary) the list is cut into parts of about equal draw counts, each starting with a copy of that state. n worker
// threads record the first parts into command buffers of their own while the replay thread records the last part into
// the submission's command buffer; the workers' parts go in order ahead of it in the submission's one submit (vk_sync_parts), and the last
// part's state carries on in X as the whole submission's would. Each worker part ends its pass and with a full barrier,
// so every image is in GENERAL when the next part begins, as at the start of a submission.
// What one part leaves for later ones reaches them in op order, not in the order the threads happen to record: each part
// starts with the inline uniform updates of the ops before it (vk_exec_upd_seed); the uniform snapshot's drops apply
// by op position (vk_usnap.c vk_usnap_par); a part makes a new texture over storage another texture holds only after
// the earlier parts are recorded (vk_texture.c entry_of, vk_exec_order_wait); samples-passed segments go to the
// completion thread in part order (vk_exec_oq_flush). Marking a texture GPU-written is the same in any order (the
// comment at vk_texture.c order_before_new). A replayed capture (--replay) gives the same pictures with parts as without
// them when the serial replay does not split its submissions (XC2_SPLIT_DRAWS=0); the parts never split one. Submissions
// with conditional rendering stay serial, and parts are not cut inside transform feedback.
typedef struct { const op_hdr **op; int n, cap; } OpList;

static void flatten(const op_hdr *h, int depth, OpList *l) {
    for (;;) {
        check_op(h);
        const void *a = nvn_op_payload(h);
        switch (h->op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            flatten(nvn_op_first(((const p_jump *)a)->target), depth + 1, l);
            break;
        default:
            if (l->n == l->cap) {
                l->cap = l->cap ? l->cap * 2 : 4096;
                if (!(l->op = realloc(l->op, sizeof *l->op * (size_t)l->cap))) rt_fatal("vk: out of memory");
            }
            l->op[l->n++] = h;
        }
        h = nvn_op_next(h);
    }
}

// The state op applies to ex, nothing recorded.
static void pre_op(VkExec *ex, uint32_t op, const void *a) {
    switch (op) {
    case OP_SET_RT:
        if (memcmp(&ex->rt, a, sizeof ex->rt)) ex->rt = *(const p_set_rt *)a, ex->rt_resolved = 0;
        break;
    case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR:
    case OP_ALPHA_REF: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART: case OP_COPY_STRIDE:
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
        exec_op(ex, op, a);   // state only
        break;
    case OP_RENDER_ENABLE: ex->render_disabled = !((const p_flag *)a)->value; break;
    case OP_XFB_ACTIVE:   // parts are not cut while it is on, so the bytes stored since (xfb_written) stay in one part
        ex->xfb_active = ((const p_flag *)a)->value != 0;
        if (ex->xfb_active) ex->xfb_written = 0;
        break;
    default: vk_exec_bind_op(ex, op, a);
    }
}

static int serial_only(uint32_t op) { return op == OP_RENDER_COND; }

// Samples-passed counting as the ops leave it (vk_exec_xfer.c reset_counter, oq_report), for the pre-walk: each part
// starts counting as the ops before it leave it.
typedef struct { int counting, reported, keep; } OqSim;

static void oq_sim(OqSim *s, uint32_t op, const void *a) {
    if (op == OP_RESET_COUNTER && ((const p_reset_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED)
        s->counting = 1, s->reported = 0;
    if (op == OP_REPORT_COUNTER && ((const p_report_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
        if (s->reported) s->keep = 1;
        s->reported = 1;
        if (!s->keep) s->counting = 0;
    }
}

#define PAR_MAX 9   // workers (XC2_REPLAY_WORKERS, at most 8) plus the replay thread
typedef struct { VkExec ex; int i0, i1; } Part;   // ops [i0, i1) of the list, recorded with ex
static struct {
    int n, prio;
    int rec;   // XC2_REPLAY_WORKER_REC (default 1): each worker has a recorder thread of its own
    int cutc;  // XC2_REPLAY_CUT_COUNTING (default 1): parts may be cut inside samples-passed counting regions
    PlatMutex lock;
    PlatCond cv, done_cv;
    Part parts[PAR_MAX];
    int nparts, next, done;   // the workers' parts of the submission being recorded: handed out, finished
    OpList ops;
    uint64_t subs, nparts_all, tried, maxrun, alldraws, block[2];
    uint64_t ns[4];   // pre-walk, the replay thread's part, its wait for the workers, the workers' parts (summed)
    // The inline uniform updates of the op list in op order, and how many come before each part (vk_exec_upd_seed).
    const void **ups;
    int nups, cups, ups_at[PAR_MAX];
    int pdone[PAR_MAX];   // the part is recorded (vk_exec_order_wait)
    uint64_t owaits;      // order waits since the last log
} W = {.n = -1, .lock = PLAT_MUTEX_INIT, .cv = PLAT_COND_INIT, .done_cv = PLAT_COND_INIT};

static _Thread_local int par_idx = -1;   // the part this thread records (vk_exec_part)

int vk_exec_part(void) { return par_idx; }

int vk_exec_order_wait(void) {
    if (par_idx <= 0) return 0;
    int waited = 0;
    plat_mutex_lock(&W.lock);
    for (int q = 0; q < par_idx; q++)
        while (!W.pdone[q]) waited = 1, plat_cond_wait(&W.done_cv, &W.lock);
    if (waited) W.owaits++;
    plat_mutex_unlock(&W.lock);
    return waited;
}

// The ops [i0, i1) on this thread, as part index of the submission: its inline updates first, each op's position for the
// uniform snapshot's drops.
static void part_ops(VkExec *ex, int index, int i0, int i1) {
    par_idx = index;
    vk_exec_upd_seed(ex, W.ups, W.ups_at[index]);
    for (int i = i0; i < i1; i++) {
        const op_hdr *h = W.ops.op[i];
        vk_usnap_pos((uint32_t)i);
        vk_prog_between_ops(&ex->prog);
        exec_op(ex, h->op, nvn_op_payload(h));
    }
    par_idx = -1;
}

static void part_record(Part *p) {
    VkExec *ex = &p->ex;
    ex->cb = vk_sync_worker_begin(ex->fence);
    // The worker's Vulkan calls go to a recorder thread of its own, as the replay thread's go to its recorder: the
    // driver's work is about half of a part's time otherwise.
    if (W.rec) rec_own(), rec_begin(ex->cb);
    x_cur = ex;
    vk_exec_draw_new_cb();
    int index = (int)(p - W.parts);
    part_ops(ex, index, p->i0, p->i1);
    par_idx = index;   // the pass end marks the part's targets
    vk_exec_pass_end(ex);
    par_idx = -1;
    vk_exec_barrier(ex);
    views_release(ex);
    free(ex->views);   // the part's query segments stay for vk_exec_oq_flush
    ex->views = NULL, ex->cap_views = 0;
    x_cur = NULL;
    if (!W.rec || !rec_end_cb(ex->cb, 0, 0)) vkEndCommandBuffer(ex->cb);
}

static void worker_main(void *arg) {
    (void)arg;
    plat_thread_priority(W.prio);
    plat_mutex_lock(&W.lock);
    for (;;) {
        while (W.next >= W.nparts) plat_cond_wait(&W.cv, &W.lock);
        Part *p = &W.parts[W.next++];
        plat_mutex_unlock(&W.lock);
        uint64_t t0 = kern_now_ns();
        part_record(p);
        uint64_t t1 = kern_now_ns();
        plat_mutex_lock(&W.lock);
        W.ns[3] += t1 - t0;
        W.pdone[p - W.parts] = 1;
        W.done++;
        plat_cond_broadcast(&W.done_cv);
    }
}

static int par_workers(void) {
    if (W.n < 0) {
        W.n = cfg_int("XC2_REPLAY_WORKERS");
        if (W.n < 0) W.n = 0;
        if (W.n > PAR_MAX - 1) W.n = PAR_MAX - 1;
        W.prio = plat_thread_priority_get();
        W.rec = rt_knob("REPLAY_WORKER_REC", "1");
        W.cutc = rt_knob("REPLAY_CUT_COUNTING", "1");
        for (int i = 0; i < W.n; i++)
            if (!plat_thread_new(worker_main, NULL, "vk replay worker")) W.n = i;
        if (W.n) rt_log("vk: replay parts recorded by %d worker threads and the replay thread (XC2_REPLAY_WORKERS)", W.n);
    }
    return W.n;
}

// The submission's ops into parts; 0 when it stays serial (walk).
#define PAR_MIN_DRAWS 96   // a part has at least this many draws and dispatches
static int record_parallel(VkExec *ex, const uint64_t *handles, int n) {
    int nw = par_workers();
    if (!nw || vk_dump_armed || vk_capture_on) return 0;
    if (++W.tried % 1000 == 0 && W.subs) {
        double s = (double)W.subs;
        uint64_t to[4];
        vk_texture_order_stats(to);
        rt_log("vk: parallel replay: %llu of the last 1000 submissions in parts, %.2f parts each; per such submission "
               "%.3f ms pre-walk, %.3f ms the replay thread's part, %.3f ms waiting for the workers, %.3f ms in the "
               "workers' parts (sum); longest run of draws between render target changes %.0f%% of the draws; cuts blocked by counting %llu, by transform feedback %llu; waits for earlier parts %llu; new textures: waited %llu, unordered %llu, with a later part's content %llu; write-back order inverted %llu",
               (unsigned long long)W.subs, (double)W.nparts_all / s, W.ns[0] / s / 1e6, W.ns[1] / s / 1e6,
               W.ns[2] / s / 1e6, W.ns[3] / s / 1e6, W.alldraws ? 100.0 * (double)W.maxrun / (double)W.alldraws : 0.0,
               (unsigned long long)W.block[0], (unsigned long long)W.block[1], (unsigned long long)W.owaits, (unsigned long long)to[0], (unsigned long long)to[1], (unsigned long long)to[2], (unsigned long long)to[3]);
        W.subs = W.nparts_all = W.maxrun = W.alldraws = W.block[0] = W.block[1] = W.owaits = 0, memset(W.ns, 0, sizeof W.ns);
    }
    uint64_t t0 = kern_now_ns();
    W.ops.n = 0;
    for (int i = 0; i < n; i++) flatten(nvn_op_first(handles[i]), 0, &W.ops);
    int draws = 0;
    for (int i = 0; i < W.ops.n; i++) {
        const op_hdr *h = W.ops.op[i];
        if (serial_only(h->op)) {
            static volatile int32_t told[OP_COUNT_];
            if (plat_inc32(&told[h->op]) == 1)
                rt_log("vk: a submission with %s stays serial (parallel replay)", nvn_op_name(h->op));
            return 0;
        }
        draws += is_draw_op(h->op);
    }
    int want = nw + 1;
    if (draws / want < PAR_MIN_DRAWS) want = draws / PAR_MIN_DRAWS;
    if (want < 2) return 0;
    if (ex->xfb_active) return 0;   // carried in: it stays on X
    OqSim oq = {ex->oq_counting, ex->oq_reported, vk_exec_oq_keep()};
    int reported[PAR_MAX] = {0}, counting[PAR_MAX] = {0};
    int per = draws / want, k = 1, since = 0, run = 0, maxrun = 0;   // run: draws since the last place a cut could go
    for (int i = 0; i < W.ops.n; i++) {   // measurement only: the longest run of draws no cut can divide
        const op_hdr *h = W.ops.op[i];
        if (h->op == OP_SET_RT) run = 0;   // approximate: ignores counting and transform feedback
        run += is_draw_op(h->op);
        if (run > maxrun) maxrun = run;
    }
    W.maxrun += (uint64_t)maxrun, W.alldraws += (uint64_t)draws;
    W.nups = 0;
    W.parts[0].ex = *ex, W.parts[0].i0 = 0;   // part 0 starts with the state as it is
    VkExec *cur = malloc(sizeof *cur);         // the state walked on, copied into each part where it begins
    if (!cur) rt_fatal("vk: out of memory");
    *cur = *ex;
    for (int i = 0; i < W.ops.n; i++) {
        const op_hdr *h = W.ops.op[i];
        const void *a = nvn_op_payload(h);
        if (h->op == OP_SET_RT && k < want && since >= per && memcmp(&cur->rt, a, sizeof cur->rt)) {
            if (oq.counting && !W.cutc) W.block[0]++;
            if (cur->xfb_active) W.block[1]++;
        }
        // Cuts may fall inside a samples-passed counting region: each part opens queries of its own and
        // vk_exec_oq_flush hands their segments to the completion thread in part order.
        if (h->op == OP_SET_RT && k < want && since >= per && (W.cutc || !oq.counting) && !cur->xfb_active &&
            memcmp(&cur->rt, a, sizeof cur->rt)) {
            W.parts[k - 1].i1 = i;
            W.parts[k].ex = *cur, W.parts[k].i0 = i;
            reported[k] = oq.reported, counting[k] = oq.counting;
            W.ups_at[k] = W.nups;
            k++, since = 0;
        }
        vk_prog_between_ops(&cur->prog);
        pre_op(cur, h->op, a);
        oq_sim(&oq, h->op, a);
        if (h->op == OP_UPDATE_UB) {
            if (W.nups == W.cups) {
                W.cups = W.cups ? W.cups * 2 : 1024;
                if (!(W.ups = realloc(W.ups, sizeof *W.ups * (size_t)W.cups))) rt_fatal("vk: out of memory");
            }
            W.ups[W.nups++] = a;
        }
        since += is_draw_op(h->op);
    }
    free(cur);
    W.parts[k - 1].i1 = W.ops.n;
    if (k < 2) return 0;
    W.ups_at[0] = 0;
    // The uniform snapshot's drops that the copies, fills and inline updates make go in by op position before any part
    // records (vk_usnap.c); those of shader writes are found as the parts record.
    vk_usnap_par(1);
    for (int i = 0; i < W.ops.n; i++) {
        const op_hdr *h = W.ops.op[i];
        const void *a = nvn_op_payload(h);
        if (h->op == OP_UPDATE_UB) vk_usnap_predrop(((const p_update_ub *)a)->dst, ((const p_update_ub *)a)->size, (uint32_t)i);
        else if (h->op == OP_COPY_B2B) vk_usnap_predrop(((const p_copy_b2b *)a)->dst, ((const p_copy_b2b *)a)->size, (uint32_t)i);
        else if (h->op == OP_CLEAR_BUF) vk_usnap_predrop(((const p_clear_buf *)a)->addr, ((const p_clear_buf *)a)->size, (uint32_t)i);
    }
    memset(W.pdone, 0, sizeof W.pdone);
    uint32_t ps = ex->pass_serial;
    for (int p = 0; p < k; p++) {   // as vk_exec_record starts a command buffer; pass serials apart
        VkExec *e = &W.parts[p].ex;
        e->recorded = 0, e->in_pass = 0, e->dirty = VK_EXEC_DIRTY_ALL, e->rt_resolved = 0, e->barrier_at = -1;
        e->views = NULL, e->nviews = e->cap_views = 0;
        e->nfbl = 0, e->fbl_mask = 0, e->fb_dirty = 0, e->fb_fs = NULL, e->nsnap = 0, e->snap_keep = 0, e->snap_now = 0;
        e->pass_serial = ps + (uint32_t)p * 0x10000u;
        e->oq_open = -1, e->oq_lost = 0, e->oq_seg = NULL, e->oq_nseg = e->oq_capseg = 0;
        e->oq_defer = NULL, e->oq_ndefer = e->oq_capdefer = 0;
        if (p) e->oq_reported = reported[p], e->oq_counting = counting[p];
        vk_exec_oq_defer(e);
    }
    // Part 0 goes on with the slots the submission carried in; their segment closes in part order (vk_exec_oq_flush).
    W.parts[0].ex.oq_seg = ex->oq_seg, W.parts[0].ex.oq_nseg = ex->oq_nseg, W.parts[0].ex.oq_capseg = ex->oq_capseg;
    W.parts[0].ex.oq_lost = ex->oq_lost;
    // The last part on X: its command buffer, fence, view list and deferral array stay; it starts a fresh segment array.
    VkExec last = W.parts[k - 1].ex;
    last.cb = ex->cb, last.fence = ex->fence, last.views = ex->views, last.cap_views = ex->cap_views, last.nviews = 0;
    last.oq_defer = ex->oq_defer, last.oq_capdefer = ex->oq_capdefer;
    int i0 = W.parts[k - 1].i0, i1 = W.parts[k - 1].i1;
    uint64_t t1 = kern_now_ns();
    plat_mutex_lock(&W.lock);
    W.nparts = k - 1, W.next = 0, W.done = 0;
    plat_cond_broadcast(&W.cv);
    plat_mutex_unlock(&W.lock);
    *ex = last;
    part_ops(ex, k - 1, i0, i1);
    uint64_t t2 = kern_now_ns();
    plat_mutex_lock(&W.lock);
    while (W.done < k - 1) plat_cond_wait(&W.done_cv, &W.lock);
    W.nparts = W.next = 0;
    W.ns[0] += t1 - t0, W.ns[1] += t2 - t1, W.ns[2] += kern_now_ns() - t2;
    plat_mutex_unlock(&W.lock);
    vk_usnap_par(0);
    VkExec *pp[PAR_MAX];
    for (int p = 0; p < k - 1; p++) pp[p] = &W.parts[p].ex;
    pp[k - 1] = ex;
    vk_exec_oq_flush(pp, k);
    for (int p = 0; p < k - 1; p++) {
        VkExec *e = &W.parts[p].ex;
        free(e->oq_seg), free(e->oq_defer);
        e->oq_seg = NULL, e->oq_defer = NULL;
    }
    VkCommandBuffer cbs[PAR_MAX];
    for (int p = 0; p < k - 1; p++) cbs[p] = W.parts[p].ex.cb;
    vk_sync_parts(cbs, k - 1);
    W.subs++, W.nparts_all += (uint64_t)k;
    if ((rt_trace & RT_TRACE_PERF) && W.subs % 600 == 0)
        rt_log("perf: parallel replay: %.2f parts per parallel submission", (double)W.nparts_all / (double)W.subs);
    return 1;
}

// The submit-time captures of the next submission this thread records (vk_backend.c, the replay thread or the
// submitting one).
static _Thread_local void *next_mem, *next_ubo;
static _Thread_local int next_set;

void vk_exec_captures(void *mem, void *ubo) {
    next_mem = mem, next_ubo = ubo, next_set = 1;
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    VkExec *ex = &X;
    void *mcap = next_mem, *ucap = next_ubo;
    int given = next_set;
    next_mem = next_ubo = NULL, next_set = 0;
    if (!n && given) vk_mem_capture_drop(mcap), vk_usnap_capture_drop(ucap);
    if (n && !given) mcap = vk_mem_capture(), ucap = vk_usnap_capture(handles, n);   // the probes' own submissions
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    ex->cb = cb;
    ex->fence = fence;
    ex->prog = vk_prog_resolve(ex->prog);   // the program carried over, as this submission sees it
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    vk_exec_frame.submits++;
    split_draws = part_start = 0;
    part_t0 = kern_now_ns();
    x_cur = ex;
    if (n) vk_mem_mirror_begin(mcap), vk_usnap_begin(ucap, fence);
    for (int i = 0; i < n; i++)
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
    if (!(n && record_parallel(ex, handles, n)))
        for (int i = 0; i < n; i++) {
            if (vk_dump_armed) vk_dump_note("replay handle %d/%d %#llx", i, n, (unsigned long long)handles[i]);
            walk(ex, nvn_op_first(handles[i]), 0);
        }
    vk_exec_pass_end(ex);
    x_cur = NULL;
    if (ex->recorded) vk_exec_barrier(ex);   // later submissions see this one's writes
    if (n) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    views_release(ex);
    ex->cb = VK_NULL_HANDLE;
    if (n) n_subs++;
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f", (double)(n_subs + n_parts) / (double)n_subs);
        n_subs = n_parts = 0;
    }
    return ex->recorded - part_start;   // what the last part holds
}
