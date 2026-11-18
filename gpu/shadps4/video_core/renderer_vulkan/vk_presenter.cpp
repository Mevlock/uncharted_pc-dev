// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/game_frame.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_hdr.h"
#include "bbport_overlay.h"
#include "bbport_thumbnail.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // bbport: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

// bbport: set by the loader when the game's view was widened to the window (runtime_ultrawide.c):
// gameplay frames then fill the window; menus, loading screens and movies stay 16:9.
extern "C" int runtime_ultrawide_fill;
// bbport: the game renders at BB_RENDER_RES (runtime_ultrawide.c) with its 16:9 layout stretched over
// it: frames that do not fill the window are shown at 16:9, not at the buffer's shape.
extern "C" int runtime_render_resolution;
// bbport: src/probe.c movie_start_called: the intro logo movie was not asked for (BB_SKIP_INTRO=1).
extern "C" volatile int runtime_logo_skipped;
// src/runtime_menumouse.c: a game menu showed (the title, after the boot).
extern "C" volatile int runtime_title_shown;
extern std::atomic<bool> g_last_frame_drew_scene; // vk_rasterizer.cpp TakeSceneFrame

namespace {
/// bbport: skipped intro logos (BB_SKIP_INTRO=1). The games load their title level while the logo movie
/// plays, and the movie is drawn over their own frames until it ends. Without it those frames show: Uncharted 2
/// clears its scene to the title's fog grey (0.63) and runs its post process over it for about a second before
/// the temple is drawn: a grey 16:9 screen (sometimes: when the grey clear comes before the first scene draws).
/// So from the skip until the first frame that draws the 3D scene, or a menu shows (at most 15 s), the frames are
/// shown black, as the movie's last picture would be.
bool CoverSkippedLogos() {
    static int state = 0; // 0 no skip yet, 1 covering, 2 done
    static std::chrono::steady_clock::time_point since;
    static u32 covered = 0;
    if (state == 2) {
        return false;
    }
    if (state == 0) {
        if (!runtime_logo_skipped) {
            return false;
        }
        state = 1;
        since = std::chrono::steady_clock::now();
    }
    const bool scene = g_last_frame_drew_scene.load(std::memory_order_relaxed);
    if (scene || runtime_title_shown || std::chrono::steady_clock::now() - since > std::chrono::seconds(15)) {
        state = 2;
        std::printf("Intro: %u frames before the title's picture shown black (skipped logos; ended by %s)\n", covered,
                    scene ? "the title's 3D scene" : runtime_title_shown ? "a menu" : "the 15 s limit");
        return false;
    }
    ++covered;
    return true;
}
} // namespace

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height, bool fill = false) {
    if (fill) {
        return vk::Rect2D{{0, 0}, {static_cast<u32>(swapchain_width), static_cast<u32>(swapchain_height)}};
    }
    float frame_aspect = runtime_render_resolution ? 16.0f / 9.0f
                                                   : static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height, bool fill = false) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height, fill);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    // bbport: frames are composed in RGBA8 (what frame generation reads and writes) and
    // blitted into the swapchain's format.
    // HDR10 output (BbHdr, the swapchain chose an HDR10 format at start): the frames hold the
    // HDR10 signal in A2B10G10R10 (an 8-bit frame would band), the post process pass encodes it
    // and the overlay is blended on at paper white. Nothing of it is made with HDR off.
    own_hdr = BbHdr::Output() && swapchain.GetHDR();
    if (own_hdr) {
        FrameGenerator::ColorFormat = vk::Format::eA2B10G10R10UnormPack32;
        pp_settings.hdr = HostPasses::PostProcessingPass::HdrEncode;
        pp_settings.paper_nits = BbHdr::PaperWhite(); // then each frame (PrepareFrame)
        pp_settings.peak_nits = BbHdr::PeakNits();
        pp_pass.Create(device, FrameGenerator::ColorFormat, swapchain.GetHdrFormat());
    } else {
        pp_pass.Create(device, FrameGenerator::ColorFormat);
    }
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

    frame_generator = std::make_unique<FrameGenerator>(instance, draw_scheduler);
    rasterizer->GetUpscaler().SetFrameGenerator(frame_generator.get());

    // bbport even presents: paced frames held to their targets on the release queue
    // (vk_present_release.h; the swapchain presents there). Without it (BB_EVEN_RELEASE=0, A/B
    // only, or a device without such a queue) the presenter thread holds them, as before.
    if (swapchain.UsesReleaseQueue() ||
        (PresentRelease::Mode() >= 1 && PresentRelease::Supported(instance))) {
        release = std::make_unique<PresentRelease>(instance);
        LowLatency::SetGpuHolds(release->Ready() ? (swapchain.UsesReleaseQueue() ? 2 : 1) : 0);
    }
}

Presenter::~Presenter() {
    rasterizer->GetUpscaler().SetFrameGenerator(nullptr);

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    release.reset(); // waits for the release queue
    if (overlay_image) {
        device.destroyImageView(overlay_view);
        vmaDestroyImage(instance.GetAllocator(), overlay_image, overlay_allocation);
    }
    for (auto& frame : present_frames) {
        DestroyGenerated(&frame);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        if (frame.other.image) {
            device.destroyImageView(frame.other.image_view);
            vmaDestroyImage(instance.GetAllocator(), frame.other.image, frame.other.allocation);
        }
        device.destroyFence(frame.present_done);
    }
    if (hudless_frame.image) {
        vmaDestroyImage(instance.GetAllocator(), hudless_frame.image, hudless_frame.allocation);
        device.destroyImageView(hudless_frame.image_view);
    }
    if (thumbnail.image) {
        vmaDestroyImage(instance.GetAllocator(), thumbnail.image, thumbnail.image_allocation);
    }
    if (thumbnail.buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), thumbnail.buffer, thumbnail.buffer_allocation);
    }
}

void Presenter::DestroyGenerated(Frame* frame) {
    const vk::Device device = instance.GetDevice();
    for (auto& generated : frame->generated) {
        if (generated.view) {
            device.destroyImageView(generated.view);
        }
        if (generated.image) {
            vmaDestroyImage(instance.GetAllocator(), generated.image, generated.allocation);
        }
        generated = {};
    }
    frame->generated_count = 0;
}

void Presenter::EnsureGenerated(Frame* frame) {
    if (frame->generated[0].image) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < frame->generated.size(); ++i) {
        auto& generated = frame->generated[i];
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = FrameGenerator::ColorFormat,
            .extent = {frame->width, frame->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        });
        const VmaAllocationCreateInfo alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &generated.allocation, nullptr) != VK_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "Frame generation: no memory for the generated frames");
            DestroyGenerated(frame);
            return;
        }
        generated.image = vk::Image{image};
        SetObjectName(device, generated.image, "Generated frame #{}.{}", frame->id, i);
        generated.view = Check<"create generated frame view">(device.createImageView({
            .image = generated.image,
            .viewType = vk::ImageViewType::e2D,
            .format = FrameGenerator::ColorFormat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
}

void Presenter::RenderOverlayHdr(vk::CommandBuffer cmdbuf, vk::Extent2D extent) {
    const vk::Device device = instance.GetDevice();
    const vk::Format format = swapchain.GetSurfaceFormat().format; // the overlay's own (SDR)
    if (!overlay_image || overlay_extent != extent) {
        if (overlay_image) {
            // The previous image may still be read by a frame in flight. vkDeviceWaitIdle needs the
            // queues externally synchronized: under the submit lock (not held here).
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            Check(device.waitIdle());
            device.destroyImageView(overlay_view);
            vmaDestroyImage(instance.GetAllocator(), overlay_image, overlay_allocation);
            overlay_image = vk::Image{};
        }
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        });
        const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &overlay_allocation, nullptr) != VK_SUCCESS) {
            return;
        }
        overlay_image = vk::Image{image};
        SetObjectName(device, overlay_image, "HDR overlay image");
        overlay_view = Check<"create HDR overlay view">(device.createImageView({
            .image = overlay_image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        overlay_extent = extent;
    }
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = overlay_image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(overlay_image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
    BbOverlay::Render(cmdbuf, overlay_view, extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);
    pp_pass.RenderOverlay(cmdbuf, overlay_view, swapchain.ImageView(), extent, pp_settings);
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

bool Presenter::KeepFrameSizes() const {
    // bbport: both frame sizes stay allocated (about 75 MiB a frame more at 3440x1440 with the
    // FSR pass) unless video memory is tight (a card or budget under 5 GB, as the staging pool's
    // and the arena's own rules).
    static const bool keep = [this] {
        const char* env = std::getenv("BB_KEEP_FRAME_SIZES");
        if (env) {
            return env[0] != '0';
        }
        return instance.GetTotalMemoryBudget() >= 5_GB;
    }();
    return keep;
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    DestroyGenerated(frame); // made again at the new size when needed
    // bbport: the presenter switches between two frame sizes (16:9 menus and scenes, gameplay
    // filling an ultrawide window). Making the frames (and the FSR pass images) again at each
    // switch put ~316 MiB of new video memory in one frame at U3's gameplay start, and the
    // kernel then blocked a queue submission for 18-24 ms ~0.5 s later (the horse ride's worst
    // hitch, 25-32 ms, in 10 of 11 runs before; none in 6 runs with this). The
    // other size's image is kept and swapped in; it is made ahead (PrewarmFrame).
    const bool hdr = swapchain.GetHDR();
    if (KeepFrameSizes() && frame->image && frame != &hudless_frame) {
        if (frame->other.image && frame->other.width == width && frame->other.height == height &&
            frame->other.is_hdr == hdr) {
            std::swap(frame->image, frame->other.image);
            std::swap(frame->image_view, frame->other.image_view);
            std::swap(frame->allocation, frame->other.allocation);
            std::swap(frame->width, frame->other.width);
            std::swap(frame->height, frame->other.height);
            std::swap(frame->is_hdr, frame->other.is_hdr);
            return;
        }
        if (frame->other.image) {
            device.destroyImageView(frame->other.image_view);
            vmaDestroyImage(instance.GetAllocator(), frame->other.image, frame->other.allocation);
        }
        frame->other = {frame->width, frame->height, frame->is_hdr, frame->allocation, frame->image,
                        frame->image_view};
        frame->image = vk::Image{};
        frame->image_view = vk::ImageView{};
        frame->allocation = nullptr;
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = FrameGenerator::ColorFormat;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

void Presenter::PrewarmFrame(Frame* frame, u32 width, u32 height) {
    // The other size's image, made now (before it is needed) through RecreateFrame: the current
    // image goes aside, the new one is made, then they swap back.
    if (!KeepFrameSizes() || frame->width == 0 || width == 0 || height == 0 ||
        (frame->width == width && frame->height == height) ||
        (frame->other.image && frame->other.width == width && frame->other.height == height &&
         frame->other.is_hdr == swapchain.GetHDR())) {
        return;
    }
    const u32 keep_width = frame->width, keep_height = frame->height;
    RecreateFrame(frame, width, height);           // current aside, the other size made
    RecreateFrame(frame, keep_width, keep_height); // swapped back
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        char game_frame[96];
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during waiting for a frame ({})",
                   Libraries::VideoOut::GameFrameText(game_frame, sizeof(game_frame), true));
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// bbport: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

// bbport diagnosis (U2 train flicker, 2026-11-02): BB_FLASH_LOG=<file>: per presented frame, the mean brightness of a
// 16x9 grid of cells (every 8th row read back), appended as one record: u32 frame, then 144 u32 cell means read as
// 8-bit channels and 144 as 10-bit channels (the frame's format is either). A one-frame white flash is a spike there.
static void FlashSample(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                        vk::Image image, vk::ImageLayout layout, u32 w, u32 h) {
    static std::FILE* out = [] {
        const char* path = std::getenv("BB_FLASH_LOG");
        return path && *path ? std::fopen(path, "wb") : nullptr;
    }();
    static u32 frame = 0;
    if (!out || w < 16 || h < 72) {
        return;
    }
    constexpr u32 Step = 8;
    const u32 rows = h / Step;
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * rows * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    std::vector<vk::BufferImageCopy> regions(rows);
    for (u32 r = 0; r < rows; ++r) {
        regions[r] = vk::BufferImageCopy{
            .bufferOffset = VkDeviceSize(r) * w * 4,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = {0, s32(r * Step), 0},
            .imageExtent = {w, 1, 1}};
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer, regions);
    // BB_FLASH_TIMES=<file>: "frame microseconds" per sampled frame (steady clock when presented), to line flashes up
    // with other logs (BB_LATE_WRITE_CHECK).
    static std::FILE* times = [] {
        const char* path = std::getenv("BB_FLASH_TIMES");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (times) {
        std::fprintf(times, "%u %lld\n", frame,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count()));
    }
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      w, rows, index = frame++] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        const auto* px = static_cast<const u32*>(info.pMappedData);
        // BB_FLASH_GRID=64: a 64x36 grid of one-byte means (8-bit channels, 0-255), record = u32 frame
        // + 2304 bytes; otherwise 16x9 as below.
        static const bool fine = [] {
            const char* env = std::getenv("BB_FLASH_GRID");
            return env && std::atoi(env) == 64;
        }();
        if (fine) {
            constexpr u32 GW = 64, GH = 36;
            std::vector<u64> sum(GW * GH);
            std::vector<u32> cnt(GW * GH);
            for (u32 r = 0; r < rows; ++r) {
                const u32 cy = std::min<u32>(GH - 1, r * GH / rows);
                for (u32 x = 0; x < w; ++x) {
                    const u32 v = px[size_t(r) * w + x];
                    const u32 cell = cy * GW + std::min<u32>(GW - 1, x * GW / w);
                    sum[cell] += (v & 0xff) + ((v >> 8) & 0xff) + ((v >> 16) & 0xff);
                    ++cnt[cell];
                }
            }
            std::vector<u8> bytes(GW * GH);
            for (u32 c = 0; c < GW * GH; ++c) {
                bytes[c] = cnt[c] ? u8(sum[c] / (3 * cnt[c])) : 0;
            }
            std::fwrite(&index, sizeof(u32), 1, out);
            std::fwrite(bytes.data(), 1, bytes.size(), out);
            std::fflush(out);
            vmaDestroyBuffer(allocator, buffer, allocation);
            return;
        }
        std::array<u64, 144> sum8{}, sum10{};
        std::array<u32, 144> count{};
        for (u32 r = 0; r < rows; ++r) {
            const u32 cy = std::min<u32>(8, r * 9 / rows);
            for (u32 x = 0; x < w; ++x) {
                const u32 v = px[size_t(r) * w + x];
                const u32 cell = cy * 16 + std::min<u32>(15, x * 16 / w);
                sum8[cell] += (v & 0xff) + ((v >> 8) & 0xff) + ((v >> 16) & 0xff);
                sum10[cell] += (v & 0x3ff) + ((v >> 10) & 0x3ff) + ((v >> 20) & 0x3ff);
                ++count[cell];
            }
        }
        std::array<u32, 1 + 288> record{};
        record[0] = index;
        for (u32 c = 0; c < 144; ++c) {
            record[1 + c] = count[c] ? u32(sum8[c] / count[c]) : 0;
            record[145 + c] = count[c] ? u32(sum10[c] / count[c]) : 0;
        }
        std::fwrite(record.data(), sizeof(u32), record.size(), out);
        std::fflush(out);
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // bbport: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    // BB_FRAME_DUMP_BURST=N (diagnosis): one trigger dumps N consecutive frames.
    static const int frame_dump_burst = [] {
        const char* env = std::getenv("BB_FRAME_DUMP_BURST");
        return env ? std::max(1, std::atoi(env)) : 1;
    }();
    static int frame_dump_left = 0;
    // The file is looked for every 30th frame: a remove() each frame cost stage B ~1.4% in tests.
    static u32 frame_dump_poll = 0;
    if (frame_dump_trigger && frame_dump_poll++ % 30 == 0 && std::remove(frame_dump_trigger) == 0) {
        frame_dump_left = frame_dump_burst;
    }
    const bool frame_dump = frame_dump_left > 0;
    if (frame_dump) {
        --frame_dump_left;
    }
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    // bbport: a widened gameplay frame fills the window (runtime_ultrawide.c).
    const bool scene = rasterizer->TakeSceneFrame();
    if (runtime_ultrawide_fill) {
        static bool noted = false;
        if (scene && !noted) {
            noted = true;
            std::printf("Ultrawide: gameplay frames fill the window (menus and movies stay 16:9)\n");
        }
        // BB_ULTRAWIDE_STATS=1 (diagnosis): the share of filled frames.
        static const bool stats = [] {
            const char* env = std::getenv("BB_ULTRAWIDE_STATS");
            return env && env[0] == '1';
        }();
        static u32 frames = 0, filled = 0;
        filled += scene;
        if (stats && ++frames == 600) {
            std::printf("Ultrawide: %u of the last 600 frames filled the window\n", filled);
            frames = filled = 0;
        }
    }
    Frame* frame = GetRenderFrame(runtime_ultrawide_fill && scene);

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    // bbport (scene HDR): the upscaler's HDR composite (RGBA16F, linear, read as it is).
    const bool float_display = upscaled && display.format == vk::Format::eR16G16B16A16Sfloat;
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = float_display ? display.format : view_info.format,
            // The HDR composite's alpha is the HUD's share of the pixel (the vivid HUD): read as
            // it is; forced to one it made the whole picture count as HUD.
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity,
                           float_display ? vk::ComponentSwizzle::eIdentity
                                         : vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        // The FSR pass reads the picture in compute shaders (EASU when upscaling, RCAS), the post
        // process pass in a fragment shader: the barrier covers both (from BilalReloaded's port
        // 0.8.0: AMD menu flicker and black blocks).
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader |
                            vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = runtime_render_resolution
                         ? 16.0f / 9.0f
                         : static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // FSR's HDR sampling is for a guest PQ picture; the HDR10 encode reads the game's own one.
    const bool encode_hdr = pp_settings.hdr == HostPasses::PostProcessingPass::HdrEncode;
    // HDR10 output: the menu's HDR settings, live. The scene HDR composite keeps gamma 2.2 and its
    // own colours (its domain: hdr_scene.comp; the tone pass replacement has its own saturation).
    if (encode_hdr) {
        const BbHdr::Tuning tuning = BbHdr::Current();
        pp_settings.paper_nits = tuning.paper_white;
        pp_settings.peak_nits = tuning.peak_nits;
        pp_settings.sdr_gamma = float_display ? 2.2f : tuning.sdr_gamma;
        pp_settings.sdr_saturation = float_display ? 1.0f : tuning.sdr_saturation;
        // Gamut expansion: the last step of both (the overlay keeps accurate colours).
        pp_settings.gamut_expansion = tuning.gamut_expansion;
        // Vivid HUD: only the scene HDR composite carries the HUD's share in its alpha.
        pp_settings.hud_expansion = float_display ? tuning.hud_vivid : 0.0f;
    }
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr && !encode_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    static const u32 downscale_filter = [] {
        const char* env = std::getenv("BB_DOWNSCALE_FILTER");
        return env && env[0] == '0' ? 0u : 1u;
    }();
    pp_settings.downscale = downscale_filter;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (CoverSkippedLogos()) {
        // The frame is General after the pass (as the swapchain blit and the dumps read it).
        const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                                         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        cmdbuf.clearColorImage(frame->image, vk::ImageLayout::eGeneral,
                               vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                               vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
        const vk::MemoryBarrier2 cleared{.srcStageMask = vk::PipelineStageFlagBits2::eClear,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                          vk::AccessFlagBits2::eMemoryWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &cleared});
    }

    // bbport: frame generation: the frames between the previous frame and this one.
    frame->generated_count = 0;
    // Above about 165 FPS (6 ms) the game is faster than displays show generated frames:
    // generating would only take GPU time from it.
    // With a margin (paused above ~175 FPS, back below ~158): one threshold flipped generation on
    // and off around 165 FPS, which halved and doubled the frames shown (DLSS players near it
    // reported stutter, 2026-11-05).
    static bool fast_game = false;
    static u32 fast_switches = 0;
    {
        const double ms = game_frame_ms.load(std::memory_order_relaxed);
        const bool was = fast_game;
        if (!fast_game && ms < 5.7) {
            fast_game = true;
        } else if (fast_game && ms > 6.3) {
            fast_game = false;
        }
        if (was != fast_game && frame_generator->Wanted()) {
            ++fast_switches;
            if (fast_switches <= 20 || (fast_switches % 100) == 0) {
                std::printf("Frame generation: %s (game %.1f FPS, switch %u)\n",
                            fast_game ? "paused, the game runs above 175 FPS" : "back on",
                            1000.0 / ms, fast_switches);
            }
        }
    }
    // bbport: an upscaled frame (the upscaler's output-size image) is generated from too: its
    // picture before the UI is captured at its own size (TemporalUpscaler::RunScaledVelocity).
    // bbport: with the port's HDR10 output the frames are A2B10G10R10 (FrameGenerator::ColorFormat).
    if (frame_generator->Wanted() && (!frame->is_hdr || own_hdr) && !fast_game) {
        EnsureGenerated(frame);
        // The picture before the UI, composed like the frame (equal outside the UI).
        vk::Image hudless{};
        vk::ImageView hudless_view{};
        vk::Extent2D hudless_size{};
        // Scene HDR: the HDR scene before the UI (the upscaler's, the frame equals it there).
        vk::ImageView source = display.hdr_hudless;
        if (float_display && source) {
            hudless_size = image_size;
        } else {
            source = frame_generator->Hudless(view_info.format, hudless_size);
        }
        if (source && hudless_size == image_size) {
            if (hudless_frame.width != frame->width || hudless_frame.height != frame->height ||
                !hudless_frame.image) {
                // The previous frame's generation may still read it on the GPU (the switch from
                // 16:9 menus to ultrawide gameplay crashed in that frame's DLSS-G evaluation).
                if (hudless_frame.image) {
                    draw_scheduler.WaitSubmitted();
                }
                RecreateFrame(&hudless_frame, frame->width, frame->height);
            }
            const vk::ImageMemoryBarrier2 to_target{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = hudless_frame.image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
            const auto scaled = fsr_pass.Render(cmdbuf, source, hudless_size,
                                                {frame->width, frame->height}, fsr_settings,
                                                frame->is_hdr && !encode_hdr);
            pp_pass.Render(cmdbuf, scaled, hudless_size, hudless_frame, pp_settings);
            hudless = hudless_frame.image;
            hudless_view = hudless_frame.image_view;
        }
        if (frame->generated[0].image) {
            std::array<FrameGenerator::Output, FrameGenerator::MaxGenerated> outputs{};
            for (u32 i = 0; i < outputs.size(); ++i) {
                outputs[i] = {frame->generated[i].image, frame->generated[i].view};
            }
            frame->generated_count = frame_generator->Generate(
                cmdbuf, frame->image, frame->image_view,
                vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                hudless, hudless_view, frame->width, frame->height, outputs);
        }
    }
    g_frame_gen_shown.store(frame->generated_count, std::memory_order_relaxed);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass), and
        // the generated frames shown before it.
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        for (u32 i = 0; i < frame->generated_count; ++i) {
            char name[16];
            std::snprintf(name, sizeof(name), "generated%u", i);
            DumpRaw(instance, draw_scheduler, cmdbuf, frame->generated[i].image,
                    vk::ImageLayout::eGeneral, frame->width, frame->height, name,
                    frame_dump_index);
        }
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }
    static const bool flash_log = std::getenv("BB_FLASH_LOG") != nullptr;
    if (flash_log) {
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        FlashSample(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                    frame->width, frame->height);
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->generated_count = 0;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

// bbport: the save list's pictures (bbport_thumbnail.h), from BilalReloaded's port (CaptureThumbnail, PR #349 by
// the-best-is-best). The game frame (in TransferSrcOptimal after its blit to the swapchain) is scaled into a small
// RGBA8 image (a frame wider than 16:9, the widened gameplay frame, by its middle 16:9), which is copied to a mapped
// buffer; ReadThumbnail takes it once that frame's present has finished. The overlay is drawn later, straight into
// the swapchain image, so it is never in the picture.
void Presenter::CaptureThumbnail(vk::CommandBuffer cmdbuf, const Frame* frame) {
    using BbThumbnail::Height;
    using BbThumbnail::Width;
    const vk::Device device = instance.GetDevice();
    const vk::Format format = FrameGenerator::ColorFormat; // the frame's own format: the bytes stay as shown
    if (!thumbnail.image) {
        const vk::FormatProperties props = instance.GetPhysicalDevice().getFormatProperties(format);
        if (!(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)) {
            thumbnail.failed = true;
            LOG_WARNING(Render_Vulkan, "Save pictures: {} cannot be a blit target", vk::to_string(format));
            return;
        }
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {Width, Height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
        });
        const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VkDeviceSize(Width) * Height * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo buffer_alloc{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VkBuffer buffer{};
        VmaAllocationInfo buffer_out{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &image_alloc, &image,
                           &thumbnail.image_allocation, nullptr) != VK_SUCCESS) {
            thumbnail.failed = true;
            return;
        }
        thumbnail.image = vk::Image{image};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_info, &buffer_alloc, &buffer,
                            &thumbnail.buffer_allocation, &buffer_out) != VK_SUCCESS ||
            !buffer_out.pMappedData) {
            if (buffer) {
                vmaDestroyBuffer(instance.GetAllocator(), buffer, thumbnail.buffer_allocation);
            }
            thumbnail.failed = true;
            return;
        }
        thumbnail.buffer = vk::Buffer{buffer};
        thumbnail.mapped = buffer_out.pMappedData;
        SetObjectName(device, thumbnail.image, "Save picture");
    }
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier to_dst{
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = thumbnail.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                           to_dst);
    s32 x0 = 0, x1 = s32(frame->width);
    if (u64(frame->width) * 9 > u64(frame->height) * 16 + 16) { // wider than 16:9: its middle
        const s32 w = s32(u64(frame->height) * 16 / 9);
        x0 = (s32(frame->width) - w) / 2;
        x1 = x0 + w;
    }
    const vk::ImageBlit blit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets = std::array{vk::Offset3D{x0, 0, 0}, vk::Offset3D{x1, s32(frame->height), 1}},
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(Width), s32(Height), 1}},
    };
    cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, thumbnail.image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
    const vk::ImageMemoryBarrier to_src{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = thumbnail.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                           to_src);
    const vk::BufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = MakeImageSubresourceLayers(),
        .imageOffset = {0, 0, 0},
        .imageExtent = {Width, Height, 1},
    };
    cmdbuf.copyImageToBuffer(thumbnail.image, vk::ImageLayout::eTransferSrcOptimal, thumbnail.buffer, copy);
    const vk::BufferMemoryBarrier to_host{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = thumbnail.buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {}, to_host,
                           {});
    thumbnail.pending = frame->present_done;
}

void Presenter::ReadThumbnail() {
    if (!thumbnail.pending ||
        instance.GetDevice().getFenceStatus(thumbnail.pending) != vk::Result::eSuccess) {
        return;
    }
    thumbnail.pending = vk::Fence{};
    vmaInvalidateAllocation(instance.GetAllocator(), thumbnail.buffer_allocation, 0, VK_WHOLE_SIZE);
    BbThumbnail::Store(static_cast<const unsigned char*>(thumbnail.mapped));
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame, s64 flip_arg) {
    // bbport: a finished save picture copy is read before any present_done fence is reset.
    ReadThumbnail();
    if (thumbnail.pending == frame->present_done) {
        thumbnail.pending = vk::Fence{}; // its fence is reset below: that copy is dropped
    }
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // bbport: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        if (!is_reusing_frame && is_game_frame) {
            LowLatency::GatePostSubmitted(flip_arg); // nothing is shown: the gate opens
        }
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    // bbport: frame generation: the generated frames first, then the game's own, evenly
    // spaced over the interval between the game's frames (the generated ones belong to the
    // time before this frame). A frame shown again has none.
    // bbport even presents (vk_low_latency.h): a paced game frame waits until the GPU has drawn it
    // (its ready time, which the offset learns from) and then until its target; its upscaling /
    // post-processing and present follow at once, on a GPU the gate keeps idle meanwhile.
    // With GPU holds (vk_present_release.h) nothing waits here: the present work is queued at once
    // behind a hold to the target, and the times the GPU took come back a frame or two later.
    const s64 even_target =
        !is_reusing_frame && is_game_frame ? LowLatency::PresentTarget(flip_arg) : 0;
    const bool even_frame = even_target != 0;
    const bool gpu_hold = even_frame && release && release->Ready();
    if (release) {
        release->Poll([](s64 flip, s64 ready, s64 released) {
            LowLatency::PresentGpuTimes(flip, ready, released);
        });
    }
    if (even_frame && !gpu_hold) {
        const vk::SemaphoreWaitInfo ready{.semaphoreCount = 1,
                                          .pSemaphores = &frame->ready_semaphore,
                                          .pValues = &frame->ready_tick};
        (void)instance.GetDevice().waitSemaphores(ready, 100'000'000);
        LowLatency::PresentReadyAndWait(flip_arg);
    }
    u32 generated = is_reusing_frame ? 0 : frame->generated_count;
    // A held frame's times are its hold's end (its target, or the next frame's slot when that is
    // sooner; LowLatency::PresentHoldEnd), the CPU's are only when it was queued. 0: not held while
    // the GPU does not fit its frames in a period: its present work only takes its ready time.
    const s64 hold_end = gpu_hold ? LowLatency::PresentHoldEnd(flip_arg) : 0;
    const auto start = gpu_hold ? std::chrono::steady_clock::time_point(
                                      std::chrono::nanoseconds(hold_end ? hold_end : even_target))
                                : std::chrono::steady_clock::now();
    if (!is_reusing_frame && is_game_frame) {
        const double ms =
            std::chrono::duration<double, std::milli>(start - last_game_present).count();
        if (ms > 1.0 && ms < 100.0) {
            game_frame_ms = game_frame_ms + (ms - game_frame_ms) * 0.1;
        }
        last_game_present = start;
        BbOverlay::NoteGameFrame();
    }
    const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(game_frame_ms / (generated + 1)));
    // The game must never wait for generated frames: when every frame is in use (the GPU
    // thread would block on the next one, and the game on its GPU work: Uncharted 3 asserts
    // in FrameBegin) they are dropped and the game's frame goes out at once.
    const auto behind = [this] {
        std::scoped_lock lock{free_mutex};
        return free_queue.empty();
    };
    if (generated && behind()) {
        generated = 0;
    }
    const auto wait_until = [](std::chrono::steady_clock::time_point at) {
        // The system sleep is coarse: sleep most of the way, then spin.
        constexpr auto spin = std::chrono::microseconds(1500);
        const auto now = std::chrono::steady_clock::now();
        if (at - now > spin) {
            std::this_thread::sleep_for(at - now - spin);
        }
        while (std::chrono::steady_clock::now() < at) {
            std::this_thread::yield();
        }
    };
    // GPU holds: each image is held to its own time on the GPU (the first, which carries the
    // frame's times, to the target), none on this thread.
    // (1: a time long passed, so PresentImage takes the ready time only and holds nothing)
    const auto hold_at = [&](u32 index) -> s64 {
        return !gpu_hold ? 0 : hold_end ? (start + step * index).time_since_epoch().count() : 1;
    };
    u32 shown = 0;
    for (; shown < generated; ++shown) {
        if (shown > 0) {
            if (behind()) {
                break;
            }
            if (!gpu_hold) {
                wait_until(start + step * shown);
            }
        }
        PresentImage(frame, frame->generated[shown].image, false, 0, hold_at(shown),
                     shown == 0 ? flip_arg : -1);
    }
    if (shown && !behind() && !gpu_hold) {
        wait_until(start + step * shown);
    }
    // bbport: low latency mode: the game's own frame carries its frame id, from the game's frame
    // number (its flip argument; vk_low_latency.h).
    const bool game_frame = !is_reusing_frame && is_game_frame;
    const u64 latency_id = game_frame ? LowLatency::PresentId(flip_arg) : 0;
    thumbnail_game_frame = game_frame;
    even_flip = game_frame ? flip_arg : -1;
    const bool presented = PresentImage(frame, frame->image, true, latency_id, hold_at(shown),
                                        shown == 0 ? flip_arg : -1);
    even_flip = -1;
    thumbnail_game_frame = false;
    if (game_frame) {
        LowLatency::GatePostSubmitted(flip_arg); // also when PresentImage gave up early
    }
    if (!presented) {
        free_frame();
        return;
    }

    free_frame();
    if (game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

bool Presenter::PresentImage(Frame* frame, vk::Image image, bool last, u64 latency_id,
                             s64 hold_until, s64 hold_flip) {
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            return false;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    if (last) {
        const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting present done fence: {}",
                   vk::to_string(reset_result));
    }

    // bbport: the frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }
    // bbport even presents: this image's present waits for a hold to its time on the release
    // queue (vk_present_release.h), after its blit here; nothing here waits for it.
    // With the release queue every present goes there, a paced one after its hold there; else a
    // paced frame's blit is held on this queue (the hold first, the next frame's work after it).
    const bool gpu_holds = release && release->Ready();
    const bool via_release = gpu_holds && swapchain.UsesReleaseQueue();
    const bool held = gpu_holds && hold_until != 0;
    // A paced frame whose target has passed by now is not held (its hold would end at once): no
    // hold commands and nothing after it waits for its blit; only its ready time is taken.
    bool hold_recorded = false;
    if (held && !via_release) {
        const s64 now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (hold_until > now) {
            const s64 period = std::max<s64>(Libraries::VideoOut::FrameCapPeriodNs(), 1'000'000);
            release->RecordHold(cmdbuf, hold_flip, hold_until,
                                std::clamp<s64>(hold_until - now + period, 0, 100'000'000));
            hold_recorded = true;
        } else {
            release->RecordLate(cmdbuf, hold_flip);
        }
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask =
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height,
                                          frame->fill),
                         vk::Filter::eLinear);
        if (last && thumbnail_game_frame && image == frame->image && !frame->is_hdr && !thumbnail.failed &&
            !thumbnail.pending && BbThumbnail::Wanted()) {
            CaptureThumbnail(cmdbuf, frame);
        }
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        BbOverlay::NotePresent();
        if (own_hdr) {
            BbHdr::PollTuneFile(); // the HDR tune file, at most a stat every 250 ms
        }
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            if (own_hdr) {
                RenderOverlayHdr(cmdbuf, extent);
            } else {
                BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            }
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    // The frame's ready time for the hold's offset: when this blit is done, on this queue (the
    // release queue runs its holds in order, so its own start follows the last hold).
    const s32 hold_slot = held && via_release ? release->RecordReady(cmdbuf) : -1;
    if (hold_recorded) {
        PresentRelease::RecordFence(cmdbuf); // the next frame's GPU work waits for this blit
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    if (last) {
        // Also covers the generated frames' submissions before it on this queue.
        info.AddSignal(frame->present_done);
    }
    // bbport even presents: this submission needs only the frame's own draw work submitted; the
    // next frame's first draw flush waits for it at the gate (LowLatency::GateBeforeSubmit).
    const u64 async_up_to = frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()
                                ? frame->ready_tick
                                : ~u64{0};
    info.async_up_to = async_up_to;
    scheduler.Flush(info);
    const u64 present_tick = scheduler.CurrentTick() - 1; // what this submission signals
    if (release && PresentRelease::Probing()) {
        // BB_HOLD_PROBE: when this submission (a held one: its hold and blit) really ends
        release->ProbeNote(scheduler.GetWorkSemaphore()->Handle(), scheduler.CurrentTick() - 1,
                           hold_flip, held ? hold_until : 0);
    }
    if (last && even_flip >= 0) {
        LowLatency::GatePostSubmitted(even_flip); // the gate opens: this frame's post work is queued
    }
    bool released = false;
    if (held && via_release) {
        // The hold on the release queue (it ends by itself at the latest one cap period after its
        // time); the present waits for it.
        const s64 now = std::chrono::steady_clock::now().time_since_epoch().count();
        const s64 period = std::max<s64>(Libraries::VideoOut::FrameCapPeriodNs(), 1'000'000);
        released = release->Submit(hold_slot, swapchain.GetPresentReadySemaphore(),
                                   swapchain.GetPresentReleaseSemaphore(), hold_flip, hold_until,
                                   std::clamp<s64>(hold_until - now + period, 0, 100'000'000));
    }
    if (release) {
        // the hold slots recorded into this submission: their times are read once it has run
        release->Submitted(scheduler.GetWorkSemaphore()->Handle(), present_tick);
    }

    // bbport (hitch diagnosis, BB_HITCH_MS): a present slower than the hitch threshold prints where its time went.
    // BB_PRESENT_GPU_WAIT=1 (experiment): wait for the GPU to finish this frame before taking the submit lock, which
    // every scheduler's submissions share (a present that blocks while holding it stalls the draw recording thread),
    // and log that wait too.
    static const double slow_ms = [] {
        const char* env = std::getenv("BB_HITCH_MS");
        return env ? std::atof(env) : 0.0;
    }();
    static const bool gpu_wait = [] {
        const char* env = std::getenv("BB_PRESENT_GPU_WAIT");
        return env && env[0] == '1';
    }();
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t0 = Clock::now();
    if (gpu_wait) {
        scheduler.Wait(scheduler.CurrentTick() - 1);
    }
    const auto t1 = Clock::now();

    // Present to swapchain.
    bool presented = true;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        const auto t2 = Clock::now();
        presented = swapchain.Present(latency_id, released, async_up_to);
        const auto t3 = Clock::now();
        if (slow_ms > 0 && ms(t0, t3) > slow_ms) {
            char gpu_done[32] = "(not waited)";
            if (gpu_wait) {
                std::snprintf(gpu_done, sizeof(gpu_done), "%.1f ms", ms(t0, t1));
            }
            std::printf("Present slow: %.1f ms: GPU done after %s, submit lock %.1f ms, async submits %.1f ms, "
                        "queue lock %.1f ms, present call %.1f ms\n",
                        ms(t0, t3), gpu_done,
                        ms(t1, t2), swapchain.last_async_ms, swapchain.last_queue_lock_ms,
                        swapchain.last_present_call_ms);
        }
    }
    // Recreate (its Destroy) waits for the device under the submit lock: after releasing it.
    if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    return true;
}

Frame* Presenter::GetRenderFrame(bool fill) {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        char game_frame[96];
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during waiting for a frame ({})",
                   Libraries::VideoOut::GameFrameText(game_frame, sizeof(game_frame), true));
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    const u32 want_width = fill ? window_frame_width : expected_frame_width;
    const u32 want_height = fill ? window_frame_height : expected_frame_height;
    if (frame->width != want_width || frame->height != want_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, want_width, want_height);
    }
    // bbport: the other frame size made ahead (at the title, not when gameplay starts).
    if (window_frame_width != expected_frame_width || window_frame_height != expected_frame_height) {
        const bool other_fill = !fill;
        const u32 other_width = other_fill ? window_frame_width : expected_frame_width;
        const u32 other_height = other_fill ? window_frame_height : expected_frame_height;
        PrewarmFrame(frame, other_width, other_height);
        if (KeepFrameSizes() && fsr_settings.enable) {
            // the FSR pass's images at the gameplay size (filling the window); it may not run at 16:9
            fsr_pass.SetKeepSizes(true);
            fsr_pass.Prewarm({window_frame_width, window_frame_height});
        }
    }
    frame->fill = fill;

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    // A frame that fills the window is scaled to its whole size (FSR 1 scales each axis).
    window_frame_width = width;
    window_frame_height = height;
    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

void PrepareShadersWithoutWindow(AmdGpu::Liverpool* liverpool) {
    // The renderer as the presenter builds it, on a device with no surface: the rasterizer starts the pipeline
    // cache's warm-up, which prepares the list and marks the end. They are left alive (the process ends next).
    auto* instance = new Instance(EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled());
    auto* scheduler = new Scheduler(*instance, true);
    auto* runtime = new Runtime(*instance, *scheduler);
    new Rasterizer(*instance, *scheduler, *runtime, liverpool);
    while (!BbOverlay::PreparationFinished()) {
        BbOverlay::PrintPreparationProgress();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    BbOverlay::PrintPreparationProgress();
    // Every prepared shader and pipeline must reach the disk before the process ends (the exit flush waits at most
    // 3 s: a first preparation's ~20,000 writes were mostly lost and the next start built them again): for as long
    // as saving goes on (0.3.6.57: at most 2 minutes), until none has been saved for 10 s.
    const auto t_flush = std::chrono::steady_clock::now();
    auto& db = Storage::DataBase::Instance();
    const u32 to_write = std::max<u32>(1, db.PendingWrites());
    db.SetUnthrottled(true);
    db.WaitForWrites(10.0, [&](u32 left) {
        BbOverlay::SetShaderPreparation(4, to_write - std::min(to_write, left), to_write);
        BbOverlay::PrintPreparationProgress();
        return true;
    });
    db.SetUnthrottled(false);
    BbOverlay::SetShaderPreparation(0, 0, 0);
    // Only a complete cache gets the "prepared" marker (else the next start prepares again); the exit flush writes
    // it once nothing is pending.
    FlushPipelineCacheForExit();
    const bool marked = DeferredPreparedMarkerWritten();
    std::printf("Preparation only: cache written in %.1f s (%u writes left; prepared marker %s)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_flush).count(),
                db.PendingWrites(), marked ? "written" : "not written");
}

} // namespace Vulkan
