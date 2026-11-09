// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_guest_sync_stats.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    for (const auto& t : push_templates) {
        instance.GetDevice().destroyDescriptorUpdateTemplate(t.handle);
    }
}

namespace {
/// bbport (perf10 idea 3, experiment, OFF by default; BB_PUSH_TEMPLATES=1 turns it on): push
/// descriptors through update templates instead of copying WriteDescriptorSet arrays and their
/// info arrays into the chunk. U3 market, 2026-11-09: usual settings -0.5..-0.8% in three ABBA
/// rounds (lower crowd minimum each time), slow CPU +1.9 / -1.8% (noise): no gain, kept off.
bool PushTemplatesOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_PUSH_TEMPLATES");
        return env && env[0] == '1';
    }();
    return on;
}
/// One slot per descriptor, large enough for a buffer or image info (a texel buffer view uses 8).
constexpr size_t PushSlot = 24;
static_assert(sizeof(VkDescriptorBufferInfo) == PushSlot && sizeof(VkDescriptorImageInfo) == PushSlot);

enum class InfoKind { Buffer, Image, TexelView, None };
InfoKind KindOf(vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return InfoKind::Buffer;
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment:
        return InfoKind::Image;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return InfoKind::TexelView;
    default:
        return InfoKind::None;
    }
}
} // namespace

bool Pipeline::PushWithTemplate(vk::PipelineLayout layout, const DescriptorWrites& set_writes) const {
    constexpr size_t MaxWrites = 64;
    const size_t count = set_writes.size();
    if (count > MaxWrites) {
        return false;
    }
    std::array<u64, MaxWrites> keys;
    u32 slots = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& w = set_writes[i];
        const InfoKind kind = KindOf(w.descriptorType);
        if (w.pNext || kind == InfoKind::None || w.descriptorCount == 0 ||
            w.dstBinding > 0xFFFF || w.dstArrayElement > 0xFFFF || w.descriptorCount > 0xFFFF ||
            (kind == InfoKind::Buffer && !w.pBufferInfo) || (kind == InfoKind::Image && !w.pImageInfo) ||
            (kind == InfoKind::TexelView && !w.pTexelBufferView)) {
            return false;
        }
        keys[i] = u64(w.dstBinding) | u64(w.dstArrayElement) << 16 | u64(w.descriptorCount) << 32 |
                  u64(static_cast<u32>(w.descriptorType) & 0xFFFF) << 48;
        slots += w.descriptorCount;
    }
    const PushTemplate* found = nullptr;
    for (const auto& t : push_templates) {
        if (t.keys.size() == count && std::equal(t.keys.begin(), t.keys.end(), keys.begin())) {
            found = &t;
            break;
        }
    }
    if (!found) {
        if (push_templates.size() >= 16) {
            return false; // a pipeline whose write layout keeps changing: the plain path
        }
        boost::container::small_vector<vk::DescriptorUpdateTemplateEntry, 32> entries;
        size_t offset = 0;
        for (const auto& w : set_writes) {
            entries.push_back({
                .dstBinding = w.dstBinding,
                .dstArrayElement = w.dstArrayElement,
                .descriptorCount = w.descriptorCount,
                .descriptorType = w.descriptorType,
                .offset = offset,
                .stride = PushSlot,
            });
            offset += size_t(w.descriptorCount) * PushSlot;
        }
        const vk::DescriptorUpdateTemplateCreateInfo ci = {
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = vk::DescriptorUpdateTemplateType::ePushDescriptorsKHR,
            .descriptorSetLayout = *desc_layout,
            .pipelineBindPoint =
                IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics,
            .pipelineLayout = layout,
            .set = 0,
        };
        const auto [result, handle] = instance.GetDevice().createDescriptorUpdateTemplate(ci);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        push_templates.push_back({std::vector<u64>(keys.begin(), keys.begin() + count), handle, slots});
        found = &push_templates.back();
    }

    const size_t bytes = size_t(slots) * PushSlot;
    const bool deferred = scheduler.IsRecordingDeferred();
    thread_local std::vector<std::byte> direct;
    std::byte* data;
    if (deferred) {
        data = static_cast<std::byte*>(scheduler.AllocateRecordData(bytes, 8));
    } else {
        direct.resize(bytes);
        data = direct.data();
    }
    std::byte* out = data;
    for (const auto& w : set_writes) {
        switch (KindOf(w.descriptorType)) {
        case InfoKind::Buffer:
            std::memcpy(out, w.pBufferInfo, size_t(w.descriptorCount) * PushSlot);
            out += size_t(w.descriptorCount) * PushSlot;
            break;
        case InfoKind::Image:
            std::memcpy(out, w.pImageInfo, size_t(w.descriptorCount) * PushSlot);
            out += size_t(w.descriptorCount) * PushSlot;
            break;
        default:
            for (u32 i = 0; i < w.descriptorCount; ++i, out += PushSlot) {
                std::memcpy(out, &w.pTexelBufferView[i], sizeof(vk::BufferView));
            }
            break;
        }
    }
    const vk::DescriptorUpdateTemplate handle = found->handle;
    const void* pdata = data;
    scheduler.Record([handle, layout, pdata](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushDescriptorSetWithTemplateKHR(handle, layout, 0, pdata);
    });
    return true;
}


void Pipeline::BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                             size_t set1_first) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // bbport (BB_GPL): a split layout's set 1 (the pre-raster stages) comes from the descriptor heap; the
    // writes before `set1_first` are set 0 and take the usual path below.
    if (split_sets && set1_first < set_writes.size()) {
        const auto set1 = desc_heap.Commit(*pre_raster_layout);
        const std::span<vk::WriteDescriptorSet> writes1{set_writes.data() + set1_first,
                                                        set_writes.size() - set1_first};
        for (auto& write : writes1) {
            write.dstSet = set1;
        }
        instance.GetDevice().updateDescriptorSets(writes1, {});
        scheduler.Record([bind_point, layout, set1](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindDescriptorSets(bind_point, layout, 1, set1, {});
        });
        set_writes.resize(set1_first);
    }

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (GuestSync::On()) {
            // Diagnosis: how often a push repeats the last one (same bind point and layout).
            u64 hash = 0xcbf29ce484222325ull;
            const auto mix = [&](const void* p, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    hash = (hash ^ static_cast<const u8*>(p)[i]) * 0x100000001b3ull;
                }
            };
            u64 descs = 0;
            for (const auto& w : set_writes) {
                mix(&w.dstBinding, sizeof(w.dstBinding));
                mix(&w.descriptorType, sizeof(w.descriptorType));
                descs += w.descriptorCount;
                if (w.pBufferInfo) mix(w.pBufferInfo, w.descriptorCount * sizeof(*w.pBufferInfo));
                if (w.pImageInfo) mix(w.pImageInfo, w.descriptorCount * sizeof(*w.pImageInfo));
                if (w.pTexelBufferView) mix(w.pTexelBufferView, w.descriptorCount * sizeof(*w.pTexelBufferView));
            }
            static VkPipelineLayout last_layout[2]{};
            static u64 last_hash[2]{};
            const u32 bp = IsCompute() ? 1 : 0;
            auto& c = GuestSync::Get();
            c.pushes.fetch_add(1, std::memory_order_relaxed);
            c.push_writes.fetch_add(set_writes.size(), std::memory_order_relaxed);
            c.push_descs.fetch_add(descs, std::memory_order_relaxed);
            if (last_layout[bp] == static_cast<VkPipelineLayout>(layout) && last_hash[bp] == hash) {
                c.same_pushes.fetch_add(1, std::memory_order_relaxed);
            }
            last_layout[bp] = layout;
            last_hash[bp] = hash;
        }
        if (PushTemplatesOn() && PushWithTemplate(layout, set_writes)) {
            return;
        }
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
