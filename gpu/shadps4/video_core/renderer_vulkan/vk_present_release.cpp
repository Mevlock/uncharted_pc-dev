// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_present_release.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>

#include "video_core/host_shaders/present_release_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#ifdef _WIN32
#include <windows.h>
#undef MemoryBarrier // winnt.h's macro, not vk::MemoryBarrier
#endif

namespace Vulkan {

namespace {

struct PushConstants {
    u32 until[2];
    u32 limit[2];
    u32 stamps[2];
};

s64 SteadyNs() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

#ifdef _WIN32
s64 QpcToNs(u64 qpc) {
    static const s64 freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return s64(f.QuadPart);
    }();
    const s64 q = s64(qpc);
    return q / freq * 1'000'000'000 + q % freq * 1'000'000'000 / freq;
}
constexpr VkTimeDomainKHR HostDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
s64 QpcToNs(u64 ns) {
    return s64(ns);
}
constexpr VkTimeDomainKHR HostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif

} // namespace

PresentRelease::PresentRelease(const Instance& instance_) : instance{instance_} {
    if (!instance.IsPresentReleaseSupported()) {
        std::puts("Even presents: the GPU hold needs VK_KHR_shader_clock (device clock) and calibrated "
                  "timestamps; not available");
        return;
    }
    const vk::Device device = instance.GetDevice();
    tick_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(PushConstants)};
    auto [layout_result, layout_value] = device.createPipelineLayoutUnique({
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
    if (layout_result != vk::Result::eSuccess) {
        return;
    }
    layout = std::move(layout_value);
    const vk::ShaderModule module = CompileSPV(PRESENT_RELEASE_COMP, device);
    if (!module) {
        return;
    }
    auto [pipeline_result, pipeline_value] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                .layout = *layout,
            });
    device.destroyShaderModule(module);
    if (pipeline_result != vk::Result::eSuccess) {
        return;
    }
    pipeline = std::move(pipeline_value);
    auto [pool_result, pool_value] = device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = Slots * 3,
    });
    if (pool_result != vk::Result::eSuccess) {
        return;
    }
    pool = std::move(pool_value);
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = Slots * sizeof(u64),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return;
    }
    buffer = raw;
    stamps = static_cast<u64*>(info.pMappedData);
    address = device.getBufferAddress({.buffer = buffer});
    if (!stamps || !address) {
        return;
    }
#ifdef _WIN32
    // The steady clock against QueryPerformanceCounter (libc++ reads it; the same rate), read
    // together: the smallest of a few tries.
    s64 best = INT64_MAX;
    for (int i = 0; i < 8; ++i) {
        const s64 a = SteadyNs();
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const s64 b = SteadyNs();
        if (b - a < best) {
            best = b - a;
            steady_minus_qpc = a + (b - a) / 2 - QpcToNs(u64(q.QuadPart));
        }
    }
#endif
    if (Wanted(instance)) {
    auto [pool_cmd_result, pool_cmd] = device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetReleaseQueueFamilyIndex(),
    });
    if (pool_cmd_result != vk::Result::eSuccess) {
        return;
    }
    command_pool = std::move(pool_cmd);
    auto [buffers_result, buffers] = device.allocateCommandBuffersUnique({
        .commandPool = *command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = Slots,
    });
    if (buffers_result != vk::Result::eSuccess) {
        return;
    }
    for (u32 i = 0; i < Slots; ++i) {
        cmdbufs[i] = std::move(buffers[i]);
        auto [fence_result, fence] =
            device.createFenceUnique({.flags = vk::FenceCreateFlagBits::eSignaled});
        if (fence_result != vk::Result::eSuccess) {
            return;
        }
        fences[i] = std::move(fence);
    }
    LowLatency::MarkOutOfBandPresentQueue(instance.GetReleasePresentQueue() ? instance.GetReleasePresentQueue()
                                                                             : instance.GetReleaseQueue());
    }
    Calibrate();
    made = cal_ticks != 0;
    std::printf("Even presents: the GPU holds each paced frame's present work to its target "
                "(timestamp %.3f ns, steady - QPC %lld ns)%s\n",
                tick_ns, static_cast<long long>(steady_minus_qpc), made ? "" : "; calibration FAILED");
}

int PresentRelease::Mode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_EVEN_RELEASE");
        return env && env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
    }();
    return LowLatency::EvenPresents() ? mode : 0;
}

bool PresentRelease::Supported(const Instance& instance) {
    return instance.IsPresentReleaseSupported();
}

bool PresentRelease::Wanted(const Instance& instance) {
    return Mode() == 2 && instance.GetReleaseQueue() && Supported(instance);
}

PresentRelease::~PresentRelease() {
    if (instance.GetReleaseQueue() && command_pool) {
        std::scoped_lock lk{g_release_queue_mutex};
        (void)instance.GetReleaseQueue().waitIdle();
    }
    if (buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer, allocation);
    }
}

void PresentRelease::Calibrate() {
    const std::array<VkCalibratedTimestampInfoKHR, 2> infos{{
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = HostDomain},
    }};
    std::array<u64, 2> values{};
    u64 deviation = 0;
    const VkDevice device = instance.GetDevice();
    const s64 call_start = SteadyNs();
    const VkResult result =
        instance.IsCalibratedTimestampsKhr()
            ? VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsKHR(device, 2, infos.data(),
                                                                          values.data(), &deviation)
            : VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsEXT(device, 2, infos.data(),
                                                                          values.data(), &deviation);
    if (result != VK_SUCCESS || values[0] == 0) {
        return;
    }
    cal_ticks = values[0];
    cal_ns = QpcToNs(values[1]) + steady_minus_qpc;
    if (Probing()) {
        NoteCalibration(deviation, SteadyNs() - call_start);
    }
}

void PresentRelease::NoteCalibration(u64 deviation, s64 call_ns) {
    // the host time of timestamp 0 by this calibration: steady if the two clocks agree
    const double offset = double(cal_ns) - double(cal_ticks) * tick_ns;
    CalStats& c = cal_stats;
    if (c.count == 0) {
        c.offset_min = c.offset_max = offset;
        c.since = cal_ns;
    }
    ++c.count;
    c.call_ns_sum += call_ns;
    c.call_ns_max = std::max(c.call_ns_max, call_ns);
    c.deviation_max = std::max(c.deviation_max, deviation);
    c.offset_min = std::min(c.offset_min, offset);
    c.offset_max = std::max(c.offset_max, offset);
    if (cal_ns - c.since >= 1'000'000'000) {
        std::printf("Hold probe: calibration over 1 s: %u calls (CPU %.1f us avg, %.1f max), deviation max %.1f us, host time of "
                    "timestamp 0 spread %.1f us (%.3f ms); hold start - timestamp %u pairs, spread "
                    "%.2f us (%.2f..%.2f)\n",
                    c.count, double(c.call_ns_sum) / 1e3 / c.count, double(c.call_ns_max) / 1e3,
                    double(c.deviation_max) / 1e3, (c.offset_max - c.offset_min) / 1e3,
                    c.offset_min / 1e6, c.pair_count, (c.pair_max - c.pair_min) * tick_ns / 1e3,
                    c.pair_min * tick_ns / 1e3, c.pair_max * tick_ns / 1e3);
        c = {};
    }
}

s64 PresentRelease::TicksToNs(u64 ticks) const {
    return cal_ns + s64(std::llround((double(s64(ticks - cal_ticks))) * tick_ns));
}

u64 PresentRelease::NsToTicks(s64 ns) const {
    return cal_ticks + u64(std::llround(double(ns - cal_ns) / tick_ns));
}

u32 PresentRelease::TakeSlot() {
    Slot& slot = slots[next];
    if (slot.pending) {
        // Never polled (no present for a while): its times are dropped.
        slot.pending = false;
        oldest = (next + 1) % Slots;
    }
    const u32 index = next;
    next = (next + 1) % Slots;
    // Its last run (Slots holds ago) is long done; a bounded wait all the same.
    if (fences[index] &&
        instance.GetDevice().waitForFences(*fences[index], VK_TRUE, 1'000'000'000) != vk::Result::eSuccess) {
        std::printf("Even presents: a hold's command buffer was still in use after 1 s\n");
    }
    stamps[index] = 0;
    slot = {.pending = false, .flip_arg = -1, .until_ns = 0};
    return index;
}

/// The hold itself: one invocation keeps its queue until the device clock reaches the target (or
/// its limit from its start); timestamps before (index * 3 + 1) and after (+ 2) it.
void PresentRelease::Spin(vk::CommandBuffer cmdbuf, u32 index, s64 until_ns, s64 limit_ns) {
    // Until the device clock is known against the timestamps (200 ms of holds), the holds only
    // take their times.
    u64 until_clock = 0, limit_clock = 0;
    if (fitted) {
        until_clock = u64(std::llround(fit_a * double(NsToTicks(until_ns)) + fit_b));
        limit_clock = u64(std::llround(fit_a * double(std::max<s64>(limit_ns, 0)) / tick_ns));
    }
    PushConstants push{};
    push.until[0] = u32(until_clock);
    push.until[1] = u32(until_clock >> 32);
    push.limit[0] = u32(limit_clock);
    push.limit[1] = u32(limit_clock >> 32);
    const u64 at = address + index * sizeof(u64);
    push.stamps[0] = u32(at);
    push.stamps[1] = u32(at >> 32);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, index * 3 + 1);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmdbuf.dispatch(1, 1, 1);
    const vk::MemoryBarrier after{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
    };
    if (Part("nohost")) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, {});
    } else {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                               vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
                               {}, after, {}, {});
    }
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, index * 3 + 2);
}

void PresentRelease::RecordHold(vk::CommandBuffer cmdbuf, s64 flip_arg, s64 until_ns, s64 limit_ns) {
    if (Part("skip")) {
        return;
    }
    if (!Part("cal1s") || SteadyNs() - cal_ns > 1'000'000'000) {
        Calibrate();
    }
    const u32 index = TakeSlot();
    cmdbuf.resetQueryPool(*pool, index * 3, 3);
    // everything before it on the queue done (the frame), then its ready time
    const vk::MemoryBarrier before{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                           {}, before, {}, {});
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, index * 3);
    Spin(cmdbuf, index, until_ns, limit_ns);
    slots[index] = {.pending = true, .flip_arg = flip_arg, .until_ns = until_ns};
}

void PresentRelease::RecordFence(vk::CommandBuffer cmdbuf) {
    if (Part("nofence") || Part("skip")) {
        return;
    }
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, {});
}

s32 PresentRelease::RecordReady(vk::CommandBuffer cmdbuf) {
    const u32 index = TakeSlot();
    cmdbuf.resetQueryPool(*pool, index * 3, 3);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, index * 3);
    return s32(index);
}

bool PresentRelease::Submit(s32 slot_index, vk::Semaphore blit_done, vk::Semaphore released, s64 flip_arg,
                            s64 until_ns, s64 limit_ns) {
    if (slot_index < 0 || slot_index >= s32(Slots)) {
        return false;
    }
    Calibrate();
    const u32 index = u32(slot_index);
    Slot& slot = slots[index];
    const vk::Device device = instance.GetDevice();
    (void)device.resetFences(*fences[index]);
    const vk::CommandBuffer cmdbuf = *cmdbufs[index];
    (void)cmdbuf.reset();
    (void)cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    Spin(cmdbuf, index, until_ns, limit_ns);
    (void)cmdbuf.end();
    slot = {.pending = true, .flip_arg = flip_arg, .until_ns = until_ns};
    // The hold starts once the frame's blit is done on the graphics queue (GPU to GPU), and its
    // end lets the present go (GPU to the presentation engine): no wait the host releases.
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eAllCommands;
    const vk::SubmitInfo submit{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &blit_done,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &released,
    };
    std::scoped_lock lk{g_release_queue_mutex};
    if (instance.GetReleaseQueue().submit(submit, *fences[index]) == vk::Result::eSuccess) {
        return true;
    }
    slot.pending = false;
    std::printf("Even presents: a hold's submission failed\n");
    return false;
}


void PresentRelease::Poll(const std::function<void(s64, s64, s64)>& done) {
    if (!made) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    while (slots[oldest].pending) {
        Slot& slot = slots[oldest];
        std::array<u64, 6> data{}; // value, availability: ready (graphics), hold start, release
        const vk::Result result = device.getQueryPoolResults(
            *pool, oldest * 3, 3, sizeof(data), data.data(), 2 * sizeof(u64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if ((result != vk::Result::eSuccess && result != vk::Result::eNotReady) || !data[1] ||
            !data[3] || !data[5]) {
            return; // not run yet: the later ones neither
        }
        const u64 started = stamps[oldest];
        const u64 ready = data[0], entry = data[2], exit = data[4];
        slot.pending = false;
        oldest = (oldest + 1) % Slots;
        if (started) {
            // The device clock against the timestamps: the rate over the longest span seen, the
            // offset from this pair (the dispatch starts a few microseconds after its timestamp).
            if (pairs == 0) {
                first_ticks = entry;
                first_clock = started;
            }
            ++pairs;
            if (Probing() && fitted) {
                // the dispatch's start against the fit from the earlier pairs (ticks)
                const double d = (double(started) - fit_b) / fit_a - double(entry);
                CalStats& c = cal_stats;
                c.pair_min = c.pair_count ? std::min(c.pair_min, d) : d;
                c.pair_max = c.pair_count ? std::max(c.pair_max, d) : d;
                ++c.pair_count;
            }
            // Holds start once the span is 200 ms (the rate known to 1e-5 or better).
            const double span = double(s64(entry - first_ticks));
            if (span * tick_ns > 200'000'000.0) {
                fit_a = double(s64(started - first_clock)) / span;
                fit_b = double(started) - fit_a * double(entry);
                if (!fitted) {
                    fitted = true;
                    std::printf("Even presents: device clock = %.6f x timestamps (%u pairs)\n", fit_a,
                                pairs);
                }
            }
        }
        done(slot.flip_arg, TicksToNs(ready), TicksToNs(exit));
    }
}

bool PresentRelease::Part(const char* name) {
    static const std::string parts = [] {
        const char* env = std::getenv("BB_HOLD_PART");
        if (env && env[0]) {
            std::printf("Even presents: BB_HOLD_PART=%s (diagnosis)\n", env);
        }
        return std::string(",") + (env ? env : "") + ",";
    }();
    return parts.find(std::string(",") + name + ",") != std::string::npos;
}

bool PresentRelease::Probing() {
    static const bool on = [] {
        const char* env = std::getenv("BB_HOLD_PROBE");
        return env && env[0];
    }();
    return on;
}

namespace {
struct ProbeEntry {
    VkSemaphore timeline;
    u64 tick;
    s64 flip;
    s64 until_ns;
    u64 call_qpc;
};
std::mutex probe_mutex;
std::deque<ProbeEntry> probe_queue;

u64 Qpc() {
#ifdef _WIN32
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return u64(q.QuadPart);
#else
    return u64(SteadyNs());
#endif
}
double QpcMs(u64 qpc) {
    return double(QpcToNs(qpc)) / 1e6;
}
} // namespace

void PresentRelease::ProbeNote(vk::Semaphore timeline, u64 tick, s64 flip_arg, s64 until_ns) {
    if (!Probing()) {
        return;
    }
    static std::once_flag started;
    std::call_once(started, [this] {
        const VkDevice device = instance.GetDevice();
        const s64 steady_qpc = steady_minus_qpc;
        std::thread([device, steady_qpc] {
            FILE* out = std::fopen(std::getenv("BB_HOLD_PROBE"), "w");
            if (!out) {
                std::printf("Hold probe: cannot write %s\n", std::getenv("BB_HOLD_PROBE"));
                return;
            }
            std::fputs("flip,target_qpc_ms,call_qpc_ms,done_qpc_ms,done_minus_target_ms\n", out);
            u32 lines = 0;
            for (;;) {
                ProbeEntry e{};
                {
                    std::scoped_lock lk{probe_mutex};
                    if (!probe_queue.empty()) {
                        e = probe_queue.front();
                        probe_queue.pop_front();
                    }
                }
                if (!e.timeline) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }
                // spins (yielding) until the submission is done: ~10 us resolution
                u64 value = 0;
                const s64 limit = SteadyNs() + 500'000'000;
                while (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue(device, e.timeline,
                                                                                 &value) == VK_SUCCESS &&
                       value < e.tick && SteadyNs() < limit) {
                    std::this_thread::yield();
                }
                const u64 done = Qpc();
                const double target_ms = double(e.until_ns - steady_qpc) / 1e6;
                std::fprintf(out, "%lld,%.4f,%.4f,%.4f,%.4f\n", static_cast<long long>(e.flip),
                             target_ms, QpcMs(e.call_qpc), QpcMs(done), QpcMs(done) - target_ms);
                if (++lines % 256 == 0) {
                    std::fflush(out);
                }
            }
        }).detach();
    });
    std::scoped_lock lk{probe_mutex};
    if (probe_queue.size() < 4096) {
        probe_queue.push_back({timeline, tick, flip_arg, until_ns, Qpc()});
    }
}

} // namespace Vulkan
