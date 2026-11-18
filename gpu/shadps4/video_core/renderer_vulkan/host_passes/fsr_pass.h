//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class FsrPass {
public:
    struct Settings {
        bool enable{true};
        bool use_rcas{true};
        float rcas_attenuation{0.25f};
    };

    void Create(vk::Device device, VmaAllocator allocator, u32 num_images);

    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                         vk::Extent2D output_size, Settings settings, bool hdr);

    /// bbport: keep the images of the previous output size for a switch back (the presenter's
    /// two frame sizes: 16:9 and filling the window) instead of making them again.
    void SetKeepSizes(bool keep) {
        keep_sizes = keep;
    }
    /// bbport: makes the images of `size` ahead of the first frame of that size (kept aside).
    void Prewarm(vk::Extent2D size);

private:
    struct Img {
        u8 id{};
        bool dirty{true};

        VideoCore::UniqueImage intermediary_image;
        vk::UniqueImageView intermediary_image_view;

        VideoCore::UniqueImage output_image;
        vk::UniqueImageView output_image_view;
    };

    void ResizeAndInvalidate(u32 width, u32 height);
    void CreateImages(Img& img) const {
        CreateImages(img, cur_size);
    }
    void CreateImages(Img& img, vk::Extent2D size) const;
    std::vector<Img> NewImageSet() const;
    void KeepSet(vk::Extent2D size, std::vector<Img>&& imgs);

    vk::Device device{};
    u32 num_images{};

    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniqueDescriptorSet easu_descriptor_set{};
    vk::UniqueDescriptorSet rcas_descriptor_set{};
    vk::UniqueSampler sampler{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline easu_pipeline{};
    vk::UniquePipeline rcas_pipeline{};

    vk::Extent2D cur_size{};
    u32 cur_image{};
    std::vector<Img> available_imgs;
    VmaAllocator allocator{};
    bool keep_sizes{};
    /// bbport: image sets of other output sizes kept for a switch back (at most two).
    std::vector<std::pair<vk::Extent2D, std::vector<Img>>> kept_sets;
};

} // namespace Vulkan::HostPasses
