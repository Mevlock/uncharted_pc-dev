//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fsr_easu_comp.h"
#include "video_core/host_shaders/fsr_rcas_comp.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#define A_CPU
#include "core/debug_state.h"
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/fsr/ffx_fsr1.h"

typedef u32 uvec4[4];

struct FSRConstants {
    uvec4 Const0;
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    uvec4 Sample;
};

namespace Vulkan::HostPasses {

void FsrPass::Create(vk::Device device, VmaAllocator allocator, u32 num_images) {
    this->device = device;
    this->num_images = num_images;

    sampler = Check<"create upscaling sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = -1000.0f,
        .maxLod = 1000.0f,
    }));

    std::array<vk::DescriptorSetLayoutBinding, 3> layoutBindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler.get(),
        },
    }};

    descriptor_set_layout =
        Check<"create fsr descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = layoutBindings.size(),
            .pBindings = layoutBindings.data(),
        }));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(FSRConstants),
    };

    const auto& cs_easu_module = CompileSPV(FSR_EASU_COMP, device);
    ASSERT(cs_easu_module);
    SetObjectName(device, cs_easu_module, "fsr.comp [EASU]");

    const auto& cs_rcas_module = CompileSPV(FSR_RCAS_COMP, device);
    ASSERT(cs_rcas_module);
    SetObjectName(device, cs_rcas_module, "fsr.comp [RCAS]");

    pipeline_layout = Check<"fsp pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    SetObjectName(device, pipeline_layout.get(), "fsr pipeline layout");

    const vk::ComputePipelineCreateInfo easu_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_easu_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    easu_pipeline =
        Check<"fsp easu compute pipelines">(device.createComputePipelineUnique({}, easu_pinfo));
    SetObjectName(device, easu_pipeline.get(), "fsr easu pipeline");

    const vk::ComputePipelineCreateInfo rcas_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_rcas_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    rcas_pipeline =
        Check<"fsp rcas compute pipelines">(device.createComputePipelineUnique({}, rcas_pinfo));
    SetObjectName(device, rcas_pipeline.get(), "fsr rcas pipeline");

    device.destroyShaderModule(cs_easu_module);
    device.destroyShaderModule(cs_rcas_module);

    this->allocator = allocator;
    available_imgs = NewImageSet();
}

std::vector<FsrPass::Img> FsrPass::NewImageSet() const {
    std::vector<Img> imgs(num_images);
    for (u32 i = 0; i < num_images; ++i) {
        auto& img = imgs[i];
        img.id = static_cast<u8>(i);
        img.intermediary_image = VideoCore::UniqueImage(device, allocator);
        img.output_image = VideoCore::UniqueImage(device, allocator);
    }
    return imgs;
}

void FsrPass::Prewarm(vk::Extent2D size) {
    if (!keep_sizes || size.width == 0 || size.height == 0 || size == cur_size) {
        return;
    }
    for (const auto& [kept_size, imgs] : kept_sets) {
        if (kept_size == size) {
            return;
        }
    }
    auto imgs = NewImageSet();
    for (auto& img : imgs) {
        CreateImages(img, size);
    }
    KeepSet(size, std::move(imgs));
}

void FsrPass::KeepSet(vk::Extent2D size, std::vector<Img>&& imgs) {
    if (kept_sets.size() >= 2) {
        kept_sets.erase(kept_sets.begin()); // the longest kept
    }
    kept_sets.emplace_back(size, std::move(imgs));
}

vk::ImageView FsrPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                              vk::Extent2D input_size, vk::Extent2D output_size, Settings settings,
                              bool hdr) {
    if (!settings.enable) {
        DebugState.is_using_fsr = false;
        return input;
    }
    // bbport (from BilalReloaded's port): the picture already at the window's size gets RCAS alone (the launcher's
    // Sharpening), as it does when it is enlarged; before, sharpening only ran while upscaling (none at native 1080p in a
    // 1080p window). BB_RCAS_NATIVE=0: as before.
    static const bool rcas_native = [] {
        const char* e = std::getenv("BB_RCAS_NATIVE");
        return !(e && e[0] == '0');
    }();
    bool rcas_only = false;
    if (input_size.width >= output_size.width && input_size.height >= output_size.height) {
        if (!(rcas_native && settings.use_rcas && input_size == output_size)) {
            DebugState.is_using_fsr = false;
            return input;
        }
        rcas_only = true;
    }

    DebugState.is_using_fsr = true;

    if (output_size != cur_size) {
        ResizeAndInvalidate(output_size.width, output_size.height);
    }
    auto [width, height] = cur_size;

    auto& img = available_imgs[cur_image];
    if (++cur_image >= available_imgs.size()) {
        cur_image = 0;
    }

    if (img.dirty) {
        CreateImages(img);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/FSR",
        });
    }

    static const int thread_group_work_region_dim = 16;
    int dispatch_x = (width + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
    int dispatch_y = (height + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array enter_barrier{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = img.intermediary_image,
            .subresourceRange = simple_subresource,
        },
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = enter_barrier.size(),
        .pImageMemoryBarriers = enter_barrier.data(),
    });

    FSRConstants consts{};
    FsrEasuCon(reinterpret_cast<AU1*>(&consts.Const0), reinterpret_cast<AU1*>(&consts.Const1),
               reinterpret_cast<AU1*>(&consts.Const2), reinterpret_cast<AU1*>(&consts.Const3),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height), (AF1)width,
               (AF1)height);
    consts.Sample[0] = hdr && !settings.use_rcas ? 1 : 0;

    if (rcas_only) {
        const std::array out_barrier{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = img.output_image,
                .subresourceRange = simple_subresource,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = out_barrier.size(),
            .pImageMemoryBarriers = out_barrier.data(),
        });
        consts = {};
        FsrRcasCon(reinterpret_cast<AU1*>(&consts.Const0), settings.rcas_attenuation);
        consts.Sample[0] = hdr ? 1 : 0;
        std::array<vk::DescriptorImageInfo, 3> img_info{{
            {
                .imageView = input,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            },
            {
                .imageView = img.output_image_view.get(),
                .imageLayout = vk::ImageLayout::eGeneral,
            },
            {
                .sampler = sampler.get(),
            },
        }};
        std::array<vk::WriteDescriptorSet, 3> set_writes{{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &img_info[0],
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &img_info[1],
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampler,
                .pImageInfo = &img_info[2],
            },
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, rcas_pipeline.get());
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                    set_writes);
        cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(FSRConstants), &consts);
        cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
    } else if (settings.use_rcas) {

        { // easu
            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {
                    .imageView = input,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                },
                {
                    .imageView = img.intermediary_image_view.get(),
                    .imageLayout = vk::ImageLayout::eGeneral,
                },
                {
                    .sampler = sampler.get(),
                },
            }};

            std::array<vk::WriteDescriptorSet, 3> set_writes{{
                {
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampledImage,
                    .pImageInfo = &img_info[0],
                },
                {
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage,
                    .pImageInfo = &img_info[1],
                },
                {
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampler,
                    .pImageInfo = &img_info[2],
                },
            }};

            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, easu_pipeline.get());
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                        set_writes);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(FSRConstants), &consts);
            cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
        }

        std::array img_barrier{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = img.intermediary_image,
                .subresourceRange = simple_subresource,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = img.output_image,
                .subresourceRange = simple_subresource,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = img_barrier.size(),
            .pImageMemoryBarriers = img_barrier.data(),
        });

        { // rcas
            consts = {};
            FsrRcasCon(reinterpret_cast<AU1*>(&consts.Const0), settings.rcas_attenuation);
            consts.Sample[0] = hdr ? 1 : 0;

            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {
                    .imageView = img.intermediary_image_view.get(),
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                },
                {
                    .imageView = img.output_image_view.get(),
                    .imageLayout = vk::ImageLayout::eGeneral,
                },
                {
                    .sampler = sampler.get(),
                },
            }};

            std::array<vk::WriteDescriptorSet, 3> set_writes{{
                {
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampledImage,
                    .pImageInfo = &img_info[0],
                },
                {
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage,
                    .pImageInfo = &img_info[1],
                },
                {
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampler,
                    .pImageInfo = &img_info[2],
                },
            }};

            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, rcas_pipeline.get());
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                        set_writes);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(FSRConstants), &consts);
            cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
        }

    } else {
        // only easu
        std::array<vk::DescriptorImageInfo, 3> img_info{{
            {
                .imageView = input,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            },
            {
                .imageView = img.output_image_view.get(),
                .imageLayout = vk::ImageLayout::eGeneral,
            },
            {
                .sampler = sampler.get(),
            },
        }};

        std::array<vk::WriteDescriptorSet, 3> set_writes{{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &img_info[0],
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &img_info[1],
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampler,
                .pImageInfo = &img_info[2],
            },
        }};

        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, easu_pipeline.get());
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                    set_writes);
        cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(FSRConstants), &consts);
        cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
    }

    const std::array return_barrier{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = img.output_image,
            .subresourceRange = simple_subresource,
        },
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = return_barrier.size(),
        .pImageMemoryBarriers = return_barrier.data(),
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    return img.output_image_view.get();
}

void FsrPass::ResizeAndInvalidate(u32 width, u32 height) {
    const vk::Extent2D size{.width = width, .height = height};
    // bbport: the presenter switches between two frame sizes (16:9 and filling the window, U3's
    // gameplay vs menus and scenes). Making these images again at a switch (with the frames:
    // ~316 MiB of new video memory in one frame at U3's gameplay start) was followed ~0.5 s later
    // by the kernel blocking a queue submission for 18-24 ms (the horse ride's worst hitch).
    // Image sets of the other sizes are kept and swapped in; the gameplay size is made ahead
    // (Prewarm, from the presenter).
    if (keep_sizes) {
        std::vector<Img> next;
        for (auto it = kept_sets.begin(); it != kept_sets.end(); ++it) {
            if (it->first == size) {
                next = std::move(it->second);
                kept_sets.erase(it);
                break;
            }
        }
        if (next.empty()) {
            next = NewImageSet(); // made at first use, as before
        }
        if (cur_size.width != 0 && cur_size.height != 0) {
            KeepSet(cur_size, std::move(available_imgs));
        }
        available_imgs = std::move(next);
        cur_size = size;
        return;
    }
    this->cur_size = size;
    for (u32 i = 0; i < num_images; ++i) {
        available_imgs[i].dirty = true;
    }
}

void FsrPass::CreateImages(Img& img, vk::Extent2D size) const {
    img.dirty = false;

    // Destroy previous resources before re-creating at new size.
    // Views first, then images (views reference the images).
    img.intermediary_image_view.reset();
    img.output_image_view.reset();
    img.intermediary_image.Destroy();
    img.output_image.Destroy();

    vk::ImageCreateInfo image_create_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{
            .width = size.width,
            .height = size.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        // .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.intermediary_image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.intermediary_image),
                  "FSR Intermediary Image #{}", img.id);
    image_create_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                              vk::ImageUsageFlagBits::eColorAttachment;
    img.output_image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.output_image), "FSR Output Image #{}", img.id);

    vk::ImageViewCreateInfo image_view_create_info{
        .image = img.intermediary_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    img.intermediary_image_view = Check<"create fsr intermediary image view">(
        device.createImageViewUnique(image_view_create_info));
    SetObjectName(device, img.intermediary_image_view.get(), "FSR Intermediary ImageView #{}",
                  img.id);

    image_view_create_info.image = img.output_image;
    img.output_image_view =
        Check<"create fsr output image view">(device.createImageViewUnique(image_view_create_info));
    SetObjectName(device, img.output_image_view.get(), "FSR Output ImageView #{}", img.id);
}

} // namespace Vulkan::HostPasses
