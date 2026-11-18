// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain.
    bool AcquireNextImage();

    /// Presents the current image and move to the next one
    /// latency_id: the game frame's id in low latency mode (0: a generated or repeated frame).
    /// bbport even presents: with the release queue (UsesReleaseQueue) every present goes there;
    /// `released`: it waits for its hold (GetPresentReleaseSemaphore) instead of the blit.
    /// `async_up_to`: the draw work it must follow (WaitAsyncSubmits; the presenter: its frame's).
    bool Present(u64 latency_id = 0, bool released = false, u64 async_up_to = ~u64{0});

    /// bbport even presents (vk_present_release.h): presents go to the release queue (a compute
    /// queue that can present), each paced one after its GPU hold.
    bool UsesReleaseQueue() const {
        return use_release;
    }

    /// bbport (hitch diagnosis): the last Present's waits for the async submissions and the queue lock, and the
    /// present call itself (ms).
    double last_async_ms = 0, last_queue_lock_ms = 0, last_present_call_ms = 0;

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    /// bbport even presents: the images' format (the HDR one when HDR is on).
    vk::Format ImageFormat() const {
        return needs_hdr ? hdr_format.format : surface_format.format;
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    /// bbport even presents: signalled by the current image's GPU hold, waited by its present.
    [[nodiscard]] vk::Semaphore GetPresentReleaseSemaphore() const {
        return present_release[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    bool GetHDR() const {
        return needs_hdr;
    }

    /// bbport: the HDR10 swapchain format (BbHdr::Output, or the guest's own HDR).
    vk::Format GetHdrFormat() const {
        return hdr_format.format;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    std::vector<vk::Semaphore> present_release; ///< bbport even presents
    bool use_release = false;                   ///< bbport even presents
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    /// bbport: HDR10 metadata (VK_EXT_hdr_metadata) from the HDR settings; sent again when they change.
    void UpdateHdrMetadata();
    float metadata_peak = 0.0f;
    float metadata_paper = 0.0f;
    bool needs_hdr = false;    // The game requested HDR swapchain
    vk::SurfaceFormatKHR hdr_format{vk::Format::eA2B10G10R10UnormPack32,
                                    vk::ColorSpaceKHR::eHdr10St2084EXT};
    bool supports_hdr = false; // SC supports HDR output
};

} // namespace Vulkan
