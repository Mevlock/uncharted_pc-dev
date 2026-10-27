// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/range_set.h"

namespace VideoCore {

class BufferCache;

class FaultManager {
    static constexpr size_t MaxPendingFaults = 8;

public:
    explicit FaultManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                          BufferCache& buffer_cache, u32 sparse_pagebits, u64 sparse_num_pages);

    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return &fault_buffer;
    }

    void ProcessFaultBuffer();

    /// bbport: whether the reports of shader reads up to `tick` are all applied (or none is
    /// pending): VRAM give-back probation releases a window only then.
    [[nodiscard]] bool ReportsApplied(u64 tick) const noexcept {
        return applied_tick.load(std::memory_order_acquire) >= tick ||
               std::ranges::all_of(fault_areas, [](u64 t) { return t == 0; });
    }

private:
    Vulkan::Scheduler& scheduler;
    BufferCache& buffer_cache;
    RangeSet fault_ranges;
    u64 sparse_pagesize;
    u64 sparse_num_pages;
    u64 fault_buffer_size;
    Buffer fault_buffer;
    Buffer download_buffer;
    std::array<u64, MaxPendingFaults> fault_areas{};
    std::atomic<u64> applied_tick{0};
    u32 current_area{};
    vk::UniqueDescriptorSetLayout fault_process_desc_layout;
    vk::UniquePipeline fault_process_pipeline;
    vk::UniquePipelineLayout fault_process_pipeline_layout;
};

} // namespace VideoCore
