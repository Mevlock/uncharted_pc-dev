//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        /// bbport: a picture larger than the output is filtered (Lanczos 2) instead of point-sampled
        /// (supersampled render resolutions). BB_DOWNSCALE_FILTER=0: one bilinear tap as before.
        u32 downscale = 1;
        /// bbport (hdr 2, HDR10 output of the game's picture, BbHdr): the game's white and the
        /// brightest highlights, in nits.
        float paper_nits = 203.0f;
        float peak_nits = 1000.0f;
        /// bbport (hdr 2): the picture's decode gamma and Oklab chroma scale (BbHdr::Tuning; for
        /// the scene HDR composite 2.2 and 1.0, its own domain).
        float sdr_gamma = 2.2f;
        float sdr_saturation = 1.0f;
        /// bbport (hdr 2, both the SDR picture and the scene HDR composite): gamut expansion,
        /// 0 accurate Rec. 709 in Rec. 2020, 1 read as Display P3, 2 read as Rec. 2020.
        float gamut_expansion = 0.0f;
        float hud_expansion = 0.0f;
    };
    /// bbport: `hdr` values.
    static constexpr u32 HdrGuestPq = 1;
    static constexpr u32 HdrEncode = 2;

    /// `hdr_overlay_format` (bbport, HDR10 output only): the swapchain format the overlay is
    /// blended onto by RenderOverlay; none built when undefined.
    void Create(vk::Device device, vk::Format surface_format,
                vk::Format hdr_overlay_format = vk::Format::eUndefined);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

    /// bbport (HDR10 output): blends `overlay` (8-bit, premultiplied by ImGui's blending into
    /// transparent black, ShaderReadOnlyOptimal) onto `target` (ColorAttachmentOptimal) at
    /// paper white.
    void RenderOverlay(vk::CommandBuffer cmdbuf, vk::ImageView overlay, vk::ImageView target,
                       vk::Extent2D extent, const Settings& settings);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipeline overlay_pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
