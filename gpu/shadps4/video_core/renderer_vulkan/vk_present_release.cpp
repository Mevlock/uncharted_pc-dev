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

#include "core/libraries/videoout/driver.h"
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

/// Mode 3: the live host-timed releaser, for ReleaseAllForExit (any thread).
std::mutex g_host_release_mutex;
PresentRelease* g_host_release = nullptr;

/// BB_EVEN_HOST_SPIN_US (default 400, 50-2000): the most our thread spins before a release (after
/// the timer); the window it spins follows the timer's own misses, from 100 us up to this.
s64 HostSpinMaxNs() {
    static const s64 ns = [] {
        const char* env = std::getenv("BB_EVEN_HOST_SPIN_US");
        const long us = env && env[0] ? std::atol(env) : 400;
        return s64(std::clamp<long>(us, 50, 2000)) * 1000;
    }();
    return ns;
}

/// Mode 4: W, how long before the target the present work is submitted (the GPU spins about
/// that long): BB_EVEN_SPIN_MIN_US / BB_EVEN_SPIN_MAX_US (default 150 / 600); equal: fixed.
s64 SpinWMinNs() {
    static const s64 ns = [] {
        const char* env = std::getenv("BB_EVEN_SPIN_MIN_US");
        return s64(std::clamp<long>(env && env[0] ? std::atol(env) : 150, 0, 5000)) * 1000;
    }();
    return ns;
}
s64 SpinWMaxNs() {
    static const s64 ns = [] {
        const char* env = std::getenv("BB_EVEN_SPIN_MAX_US");
        return std::max<s64>(SpinWMinNs(), s64(std::clamp<long>(env && env[0] ? std::atol(env) : 600, 0, 5000)) * 1000);
    }();
    return ns;
}

/// BB_EVEN_HOST_LEAD=0: signal at the target itself; default: earlier by the GPU's usual wake-up
/// after our signal (the median of the last 64, 0-500 us), so that the GPU goes on at the target.
bool HostLead() {
    static const bool on = [] {
        const char* env = std::getenv("BB_EVEN_HOST_LEAD");
        return !(env && env[0] == '0');
    }();
    return on;
}

/// BB_EVEN_HOST_CSV=<file> (diagnosis): one line per host-timed hold, QPC milliseconds (joinable to
/// PresentMon): target, our signal, the GPU's release and the frame's ready time.
FILE* HostCsv() {
    static FILE* f = [] {
        const char* path = std::getenv("BB_EVEN_HOST_CSV");
        FILE* out = path && path[0] ? std::fopen(path, "w") : nullptr;
        if (out) {
            std::fputs("flip,target_qpc_ms,signal_qpc_ms,gpu_release_qpc_ms,ready_qpc_ms,spin_us,oversleep_us\n",
                       out);
        }
        return out;
    }();
    return f;
}

/// The q-quantile (0..1) of v (reordered).
s64 Quantile(std::vector<float>& v, double q) {
    if (v.empty()) {
        return 0;
    }
    const size_t k = std::min(v.size() - 1, size_t(double(v.size()) * q));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return s64(std::llround(v[k]));
}

} // namespace

PresentRelease::PresentRelease(const Instance& instance_) : instance{instance_} {
    host_timed = Mode() == 3;
    short_spin = Mode() == 4;
    if (host_timed ? !instance.IsHostReleaseSupported() : !instance.IsPresentReleaseSupported()) {
        std::puts(host_timed ? "Even presents: the host-timed hold needs 64-bit timestamps and calibrated timestamps; "
                               "not available"
                             : "Even presents: the GPU hold needs VK_KHR_shader_clock (device clock), 64-bit "
                               "timestamps and calibrated timestamps; not available");
        return;
    }
    const vk::Device device = instance.GetDevice();
    tick_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    if (!host_timed) {
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
    }
    auto [pool_result, pool_value] = device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = Slots * 3,
    });
    if (pool_result != vk::Result::eSuccess) {
        return;
    }
    pool = std::move(pool_value);
    if (!host_timed) {
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
    if (host_timed) {
        const vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline,
                                               .initialValue = 0};
        auto [timeline_result, timeline] = device.createSemaphore({.pNext = &type});
        if (timeline_result != vk::Result::eSuccess) {
            std::puts("Even presents: the host-timed hold's semaphore could not be made");
            return;
        }
        host.timeline = timeline;
#ifdef _WIN32
        host.wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803 on): wakes within ~0.1-0.5 ms
        // of its time instead of the system tick
        host.timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        host.high_resolution = host.timer != nullptr;
        if (!host.timer) {
            host.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        if (!host.wake_event || !host.timer) {
            std::puts("Even presents: the host-timed hold's timer could not be made");
            return;
        }
#endif
        host.thread = std::thread([this] { HostThread(); });
    }
    Calibrate();
    made = cal_ticks != 0;
    if (host_timed) {
        std::printf("Even presents: host-timed holds: the GPU waits idle for each paced frame's target, our thread "
                    "releases it (%s timer, spin up to %lld us; timestamp %.3f ns, steady - QPC %lld ns)%s\n",
                    host.high_resolution ? "high-resolution" : "standard",
                    static_cast<long long>(HostSpinMaxNs() / 1000), tick_ns,
                    static_cast<long long>(steady_minus_qpc), made ? "" : "; calibration FAILED");
        if (made) {
            std::scoped_lock lk{g_host_release_mutex};
            g_host_release = this;
        }
        return;
    }
    if (short_spin) {
#ifdef _WIN32
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803 on)
        ss.timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        ss.high_resolution = ss.timer != nullptr;
        if (!ss.timer) {
            ss.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
#endif
        ss.w_ns = std::clamp<s64>(400'000, SpinWMinNs(), SpinWMaxNs());
        std::printf("Even presents: short-spin holds: each paced frame's present work is submitted %lld us before "
                    "its target (learnt, %lld-%lld us) and the GPU spins only that last part (%s timer; timestamp "
                    "%.3f ns, steady - QPC %lld ns)%s\n",
                    static_cast<long long>(ss.w_ns / 1000), static_cast<long long>(SpinWMinNs() / 1000),
                    static_cast<long long>(SpinWMaxNs() / 1000), ss.high_resolution ? "high-resolution" : "standard",
                    tick_ns, static_cast<long long>(steady_minus_qpc), made ? "" : "; calibration FAILED");
        return;
    }
    std::printf("Even presents: the GPU holds each paced frame's present work to its target "
                "(timestamp %.3f ns, steady - QPC %lld ns)%s\n",
                tick_ns, static_cast<long long>(steady_minus_qpc), made ? "" : "; calibration FAILED");
}

int PresentRelease::Mode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_EVEN_RELEASE");
        return env && env[0] >= '0' && env[0] <= '4' ? env[0] - '0' : 4;
    }();
    return LowLatency::EvenPresents() ? mode : 0;
}

bool PresentRelease::Supported(const Instance& instance) {
    return Mode() == 3 ? instance.IsHostReleaseSupported() : instance.IsPresentReleaseSupported();
}

bool PresentRelease::Wanted(const Instance& instance) {
    return Mode() == 2 && instance.GetReleaseQueue() && Supported(instance);
}

PresentRelease::~PresentRelease() {
#ifdef _WIN32
    if (ss.timer) {
        CloseHandle(ss.timer);
    }
#endif
    if (host_timed) {
        {
            std::scoped_lock lk{g_host_release_mutex};
            if (g_host_release == this) {
                g_host_release = nullptr;
            }
        }
        // every hold is let go first (the device's idle waits that follow need them), then the thread ends
        HostReleaseAll();
        host.quit.store(true, std::memory_order_release);
        {
            std::scoped_lock lk{host.mutex};
        }
        host.cv.notify_all();
#ifdef _WIN32
        if (host.wake_event) {
            SetEvent(host.wake_event);
        }
#endif
        if (host.thread.joinable()) {
            host.thread.join();
        }
        HostSignal(host.next_value.load(std::memory_order_acquire) + 1024); // the thread never started
        if (host.timeline) {
            // the presenter's and every scheduler's work is finished before it goes (~Presenter):
            // no submission waits for it any more
            instance.GetDevice().destroySemaphore(host.timeline);
        }
#ifdef _WIN32
        if (host.timer) {
            CloseHandle(host.timer);
        }
        if (host.wake_event) {
            CloseHandle(host.wake_event);
        }
#endif
    }
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
    if (stamps) {
        stamps[index] = 0;
    }
    // mode 3: the thread's times of this slot's last use (its job is long done: 16 holds ago)
    host.signal_at[index].store(0, std::memory_order_relaxed);
    host.spin_ns[index].store(0, std::memory_order_relaxed);
    host.oversleep_ns[index].store(-1, std::memory_order_relaxed);
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

u32 PresentRelease::RecordHold(vk::CommandBuffer cmdbuf, s64 flip_arg, s64 until_ns, s64 limit_ns) {
    if (Part("skip")) {
        return Slots;
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
    ss.intended[index] = ss.submitted[index] = 0;
    return index;
}

u32 PresentRelease::RecordSpinReady(vk::CommandBuffer cmdbuf, s64 flip_arg, s64 until_ns) {
    if (Part("skip")) {
        return Slots;
    }
    // In a submission of its own, queued at once: a timestamp is only written once its submission
    // is in, so in the spin's submission (queued just before the target) an early frame's ready
    // time was that submission's start, not the frame's end (the offset crept up from it, and the
    // GPU-bound check saw a GPU that fits as late by the holds).
    const u32 index = RecordHostReady(cmdbuf, flip_arg, until_ns);
    ss.intended[index] = ss.submitted[index] = 0;
    return index;
}

void PresentRelease::RecordSpin(vk::CommandBuffer cmdbuf, u32 slot, s64 until_ns, s64 limit_ns) {
    if (slot >= Slots) {
        return;
    }
    if (!Part("cal1s") || SteadyNs() - cal_ns > 1'000'000'000) {
        Calibrate(); // the spin's device clock target from the timestamps: fresh for every spin
    }
    // everything before it on the queue done (the frame and its ready time), then the spin (its
    // start pairs the device clock with the timestamp just before it)
    const vk::MemoryBarrier before{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                           {}, before, {}, {});
    Spin(cmdbuf, slot, until_ns, limit_ns);
}

void PresentRelease::SubmitWait(u32 slot, s64 until_ns, const std::function<bool()>& frame_done) {
    if (slot >= Slots) {
        return;
    }
    const s64 w = ss.w_ns;
    const s64 at = std::min(until_ns - w, SteadyNs() + 100'000'000);
    s64 now = SteadyNs();
    s64 oversleep = -1;
    const s64 wake = at - ss.spin_window_ns;
    if (wake - now > 50'000) {
#ifdef _WIN32
        LARGE_INTEGER due;
        due.QuadPart = -std::max<s64>(1, (wake - now) / 100); // relative, 100 ns units
        if (ss.timer && SetWaitableTimer(ss.timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(ss.timer, 200);
        } else {
            Sleep(DWORD(std::max<s64>(0, (wake - now) / 1'000'000)));
        }
#else
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(wake)));
#endif
        now = SteadyNs();
        oversleep = std::max<s64>(0, now - wake);
        ss.wakes[ss.wake_count++ % ss.wakes.size()] = now - wake;
        if ((ss.wake_count & 15) == 0 && ss.wake_count >= 32) {
            // the CPU spin window: the timer's 95th percentile miss over the last 64 wakes plus
            // 50 us, 100 us to BB_EVEN_HOST_SPIN_US
            const u32 n = std::min<u32>(ss.wake_count, u32(ss.wakes.size()));
            std::array<s64, 64> copy = ss.wakes;
            std::nth_element(copy.begin(), copy.begin() + n * 95 / 100, copy.begin() + n);
            ss.spin_window_ns = std::clamp<s64>(copy[n * 95 / 100] + 50'000, 100'000, HostSpinMaxNs());
        }
    }
    const s64 spin_start = SteadyNs();
    while ((now = SteadyNs()) < at) {
#if defined(_M_X64) || defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
    ss.intended[slot] = at;
    ss.submitted[slot] = now;
    ss.cpu_spin[slot] = now - spin_start;
    ss.oversleep[slot] = oversleep;
    ss.w_used[slot] = w;
    ss.frame_done[slot] = frame_done();
}

void PresentRelease::ShortSpinStats(u32 index, s64 ready_ns, s64 entry_ns, s64 exit_ns) {
    ShortSpinState& s = ss;
    const Slot& slot = slots[index];
    const s64 target = slot.until_ns;
    const s64 now = SteadyNs();
    if (s.window == 0) {
        s.window = now;
    }
    (void)ready_ns;
    if (!target || !s.submitted[index]) {
        ++s.unheld; // not held (RecordLate), or held without SubmitWait
    } else if (!s.frame_done[index] && entry_ns + 20'000 >= target) {
        ++s.late_frames; // the frame itself was not done by its target: the GPU went on when it was
    } else {
        s.gpu_err.push_back(float(exit_ns - target) / 1e3f);
        s.gpu_spin.push_back(float(std::max<s64>(0, exit_ns - entry_ns)) / 1e3f);
        s.cpu.push_back(float(s.cpu_spin[index]) / 1e3f);
        s.w.push_back(float(s.w_used[index]) / 1e3f);
        if (s.oversleep[index] >= 0) {
            s.sleep_err.push_back(float(s.oversleep[index]) / 1e3f);
        }
        if (exit_ns - target > 200'000) {
            ++s.late_releases;
        }
        if (entry_ns > target) {
            ++s.late_starts; // W was too short: the GPU started after the target
        }
        if (s.frame_done[index]) {
            // the GPU's start after the moment we meant to submit (the timer's and the spin's own
            // lateness, the submission, the GPU picking it up): W is its 95th percentile + 50 us
            const s64 start = entry_ns - s.intended[index];
            s.start_late.push_back(float(start) / 1e3f);
            s.starts[s.start_count++ % s.starts.size()] = start;
            if ((s.start_count & 15) == 0 && s.start_count >= 32) {
                const u32 n = std::min<u32>(s.start_count, u32(s.starts.size()));
                std::array<s64, 64> copy = s.starts;
                std::nth_element(copy.begin(), copy.begin() + n * 95 / 100, copy.begin() + n);
                s.w_ns = std::clamp<s64>(copy[n * 95 / 100] + 50'000, SpinWMinNs(), SpinWMaxNs());
            }
        }
    }
    if (now - s.window < 5'000'000'000) {
        return;
    }
    const size_t n = s.gpu_err.size();
    double spin_sum = 0, cpu_sum = 0, cpu_max = 0;
    for (const float x : s.gpu_spin) {
        spin_sum += x;
    }
    for (const float x : s.cpu) {
        cpu_sum += x;
        cpu_max = std::max<double>(cpu_max, x);
    }
    const double seconds = double(now - s.window) / 1e9;
    const double period_us = double(std::max<s64>(Libraries::VideoOut::FrameCapPeriodNs(), 1)) / 1e3;
    const auto q = [](std::vector<float>& v, double at) { return static_cast<long long>(Quantile(v, at)); };
    const double gpu_spin_avg = n ? spin_sum / double(n) : 0.0;
    std::printf("Even presents (short spin), %.1f s: %zu held, %llu frames late (not done by the target), %llu not "
                "held | GPU release vs target, us: median %lld p99 %lld max %lld (%llu over 200, %llu started after "
                "the target) | W %lld us now (median %lld), GPU start after our submit time median %lld p99 %lld | "
                "GPU spin per frame avg %.0f us (%.1f%% of the frame time) | timer miss median %lld p99 %lld (%s), "
                "CPU spin avg %.0f max %.0f us (%.2f%% of a core)\n",
                seconds, n, static_cast<unsigned long long>(s.late_frames),
                static_cast<unsigned long long>(s.unheld), q(s.gpu_err, 0.5), q(s.gpu_err, 0.99), q(s.gpu_err, 1.0),
                static_cast<unsigned long long>(s.late_releases), static_cast<unsigned long long>(s.late_starts),
                static_cast<long long>(s.w_ns / 1000), q(s.w, 0.5), q(s.start_late, 0.5), q(s.start_late, 0.99),
                gpu_spin_avg, period_us > 1 ? 100.0 * gpu_spin_avg / period_us : 0.0, q(s.sleep_err, 0.5),
                q(s.sleep_err, 0.99), s.high_resolution ? "high-resolution" : "standard",
                n ? cpu_sum / double(n) : 0.0, cpu_max, cpu_sum / 1e4 / seconds);
    std::fflush(stdout);
    s.gpu_err.clear();
    s.gpu_spin.clear();
    s.sleep_err.clear();
    s.cpu.clear();
    s.start_late.clear();
    s.w.clear();
    s.late_frames = s.late_releases = s.late_starts = s.unheld = 0;
    s.window = now;
}

void PresentRelease::RecordLate(vk::CommandBuffer cmdbuf, s64 flip_arg) {
    if (Part("skip")) {
        return;
    }
    if (SteadyNs() - cal_ns > 100'000'000) {
        Calibrate(); // the samples' times only: kept fresh, not every frame
    }
    const u32 index = TakeSlot();
    cmdbuf.resetQueryPool(*pool, index * 3, 3);
    // A timestamp is written once every earlier command on the queue has completed (the frame):
    // its ready time, with no barrier; no hold, so its release is the same time.
    for (u32 q = 0; q < 3; ++q) {
        cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, index * 3 + q);
    }
    slots[index] = {.pending = true, .flip_arg = flip_arg, .until_ns = 0};
}

u32 PresentRelease::RecordHostReady(vk::CommandBuffer cmdbuf, s64 flip_arg, s64 until_ns) {
    if (SteadyNs() - cal_ns > 100'000'000) {
        Calibrate(); // the times only (no device clock to fit): fresh enough every 100 ms
    }
    const u32 index = TakeSlot();
    cmdbuf.resetQueryPool(*pool, index * 3, 3);
    // everything before it on the queue done (the frame), then its ready time
    const vk::MemoryBarrier before{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                           {}, before, {}, {});
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, index * 3);
    slots[index] = {.pending = true, .flip_arg = flip_arg, .until_ns = until_ns};
    return index;
}

u64 PresentRelease::QueueRelease(u32 slot, s64 until_ns, s64 limit_ns) {
    if (!HostTimed() || host.closed.load(std::memory_order_acquire)) {
        return 0;
    }
    const s64 now = SteadyNs();
    const s64 deadline = now + std::clamp<s64>(limit_ns, 0, 100'000'000);
    const s64 lead = HostLead() ? host.lead_ns.load(std::memory_order_relaxed) : 0;
    const u64 value = host.next_value.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::scoped_lock lk{host.mutex};
        const bool was_empty = host.jobs.empty();
        host.jobs.push_back({.value = value,
                             .at = std::min(until_ns - lead, deadline),
                             .target = until_ns,
                             .deadline = deadline,
                             .slot = slot});
        if (was_empty) {
            // the thread waits for a job (a later job never goes before the first: the values,
            // and so the presents, go in order)
            host.cv.notify_one();
#ifdef _WIN32
            SetEvent(host.wake_event);
#endif
        }
    }
    // An exit after the value was handed out but before its job: ReleaseAllForExit signals 1024
    // past the last value it saw, so this one is covered as well.
    return value;
}

void PresentRelease::RecordHostRelease(vk::CommandBuffer cmdbuf, u32 slot) {
    // the second submission's first commands: once its wait (our signal) and the earlier work are done
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, slot * 3 + 1);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, slot * 3 + 2);
}

void PresentRelease::HostSignal(u64 value) {
    std::scoped_lock lk{host.signal_mutex};
    if (!host.timeline || value <= host.signalled) {
        return; // a timeline only goes up (an exit's release may have passed it already)
    }
    const vk::SemaphoreSignalInfo signal{.semaphore = host.timeline, .value = value};
    // device lost: nothing waits any more; the value is taken as signalled all the same
    (void)instance.GetDevice().signalSemaphore(signal);
    host.signalled = value;
}

void PresentRelease::HostThread() {
#ifdef _WIN32
    host.thread_id.store(u32(GetCurrentThreadId()), std::memory_order_release);
    // it sleeps nearly always; when it wakes, it must run at once
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    std::vector<s64> sorted;
    for (;;) {
        HostJob job{};
        {
            std::unique_lock lk{host.mutex};
#ifdef _WIN32
            while (host.jobs.empty() && !host.closed.load(std::memory_order_acquire) &&
                   !host.quit.load(std::memory_order_acquire)) {
                lk.unlock();
                WaitForSingleObject(host.wake_event, 100);
                lk.lock();
            }
#else
            host.cv.wait_for(lk, std::chrono::milliseconds(100), [&] {
                return !host.jobs.empty() || host.closed.load(std::memory_order_acquire) ||
                       host.quit.load(std::memory_order_acquire);
            });
#endif
            if (host.closed.load(std::memory_order_acquire)) {
                host.jobs.clear();
            } else if (!host.jobs.empty()) {
                job = host.jobs.front();
            }
        }
        if (host.closed.load(std::memory_order_acquire)) {
            // past every value handed out (and any handed out while this runs)
            HostSignal(host.next_value.load(std::memory_order_acquire) + 1024);
            host.drained.store(true, std::memory_order_release);
            if (host.quit.load(std::memory_order_acquire)) {
                return;
            }
#ifdef _WIN32
            WaitForSingleObject(host.wake_event, 100);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
            continue;
        }
        if (job.value == 0) {
            if (host.quit.load(std::memory_order_acquire)) {
                return;
            }
            continue;
        }
        // the timer to shortly before the release, then a short spin
        const s64 window = host.spin_window_ns.load(std::memory_order_relaxed);
        s64 now = SteadyNs();
        const s64 wake = job.at - window;
        if (wake - now > 50'000) {
#ifdef _WIN32
            LARGE_INTEGER due;
            due.QuadPart = -std::max<s64>(1, (wake - now) / 100); // relative, 100 ns units
            if (SetWaitableTimer(host.timer, &due, 0, nullptr, nullptr, FALSE)) {
                const HANDLE handles[2] = {host.timer, host.wake_event};
                const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, 200);
                if (woke == WAIT_OBJECT_0 + 1) {
                    CancelWaitableTimer(host.timer);
                    // an exit (the loop sees it), or a job queued while the queue looked empty:
                    // the same job again
                    continue;
                }
            } else {
                Sleep(DWORD(std::max<s64>(0, (wake - now) / 1'000'000)));
            }
#else
            std::this_thread::sleep_until(
                std::chrono::steady_clock::time_point(std::chrono::nanoseconds(wake)));
#endif
            now = SteadyNs();
            const s64 oversleep = now - wake;
            host.oversleep_ns[job.slot].store(std::max<s64>(0, oversleep), std::memory_order_relaxed);
            host.wakes[host.wake_count++ % host.wakes.size()] = oversleep;
            if ((host.wake_count & 15) == 0 && host.wake_count >= 32) {
                // the spin window: the timer's 95th percentile miss over the last 64 wakes, plus
                // 50 us, between 100 us and BB_EVEN_HOST_SPIN_US
                const u32 n = std::min<u32>(host.wake_count, u32(host.wakes.size()));
                sorted.assign(host.wakes.begin(), host.wakes.begin() + n);
                const u32 k = n * 95 / 100;
                std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
                host.spin_window_ns.store(std::clamp<s64>(sorted[k] + 50'000, 100'000, HostSpinMaxNs()),
                                          std::memory_order_relaxed);
            }
            if (host.closed.load(std::memory_order_acquire)) {
                continue;
            }
        }
        // the spin: the processor's pause between clock reads, no system call
        const s64 spin_start = SteadyNs();
        while ((now = SteadyNs()) < job.at) {
#if defined(_M_X64) || defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        host.spin_ns[job.slot].store(now - spin_start, std::memory_order_relaxed);
        HostSignal(job.value);
        host.signal_at[job.slot].store(SteadyNs(), std::memory_order_release);
        {
            std::scoped_lock lk{host.mutex};
            if (!host.jobs.empty() && host.jobs.front().value == job.value) {
                host.jobs.pop_front();
            }
        }
    }
}

void PresentRelease::HostReleaseAll() {
    if (!host_timed) {
        return;
    }
    host.closed.store(true, std::memory_order_seq_cst);
#ifdef _WIN32
    const bool on_thread = host.thread_id.load(std::memory_order_acquire) == u32(GetCurrentThreadId());
    if (host.wake_event) {
        SetEvent(host.wake_event);
    }
#else
    const bool on_thread = false;
#endif
    host.cv.notify_all();
    if (!on_thread && host.thread.joinable()) {
        // our thread signals (a fault handler's thread may have faulted inside the driver); 100 ms at most
        for (int i = 0; i < 100 && !host.drained.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (host.drained.load(std::memory_order_acquire)) {
            return;
        }
    }
    HostSignal(host.next_value.load(std::memory_order_acquire) + 1024);
    host.drained.store(true, std::memory_order_release);
}

void PresentRelease::ReleaseAllForExit() {
    // a fault handler must not wait long for the presenter (which takes this lock to make or end
    // the releaser): 50 ms at most, then without it
    std::unique_lock lk{g_host_release_mutex, std::defer_lock};
    for (int i = 0; i < 50 && !lk.try_lock(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!lk.owns_lock()) {
        return; // the presenter makes or ends the releaser (its destructor releases them all)
    }
    if (PresentRelease* release = g_host_release) {
        std::printf("Even presents: host-timed holds released for the exit\n");
        std::fflush(stdout);
        release->HostReleaseAll();
    }
}

void PresentRelease::HostStats(u32 index, s64 ready_ns, s64 release_ns) {
    Host& h = host;
    const Slot& slot = slots[index];
    const s64 target = slot.until_ns;
    const s64 signal = h.signal_at[index].load(std::memory_order_acquire);
    const s64 spin = h.spin_ns[index].load(std::memory_order_relaxed);
    const s64 oversleep = h.oversleep_ns[index].load(std::memory_order_relaxed);
    if (FILE* csv = HostCsv()) {
        const auto qpc_ms = [&](s64 ns) { return ns ? double(ns - steady_minus_qpc) / 1e6 : 0.0; };
        std::fprintf(csv, "%lld,%.4f,%.4f,%.4f,%.4f,%.1f,%.1f\n", static_cast<long long>(slot.flip_arg),
                     qpc_ms(target), qpc_ms(signal), qpc_ms(release_ns), qpc_ms(ready_ns), double(spin) / 1e3,
                     double(oversleep) / 1e3);
    }
    const s64 now = SteadyNs();
    if (h.window == 0) {
        h.window = now;
    }
    if (!target) {
        // not held (RecordLate)
    } else if (ready_ns + 20'000 >= target) {
        ++h.late_frames; // the frame itself was not ready by its target: the GPU went on when it was
    } else if (signal) {
        h.host_err.push_back(float(signal - target) / 1e3f);
        h.gpu_err.push_back(float(release_ns - target) / 1e3f);
        h.wake_err.push_back(float(release_ns - signal) / 1e3f);
        h.spin.push_back(float(spin) / 1e3f);
        if (oversleep >= 0) {
            h.sleep_err.push_back(float(oversleep) / 1e3f);
        }
        if (release_ns - target > 200'000) {
            ++h.late_releases;
        }
        // the GPU's usual wake-up after our signal: the lead (the median of the last 64, 0-500 us)
        h.wakeups[h.wakeup_count++ % h.wakeups.size()] = release_ns - signal;
        if ((h.wakeup_count & 15) == 0 && h.wakeup_count >= 32) {
            const u32 n = std::min<u32>(h.wakeup_count, u32(h.wakeups.size()));
            std::array<s64, 64> copy = h.wakeups;
            std::nth_element(copy.begin(), copy.begin() + n / 2, copy.begin() + n);
            h.lead_ns.store(std::clamp<s64>(copy[n / 2], 0, 500'000), std::memory_order_relaxed);
        }
    } else {
        ++h.missed_limit; // released for the exit, not by its job
    }
    if (now - h.window < 5'000'000'000) {
        return;
    }
    const size_t n = h.host_err.size();
    double spin_sum = 0, spin_max = 0;
    for (const float s : h.spin) {
        spin_sum += s;
        spin_max = std::max<double>(spin_max, s);
    }
    const double seconds = double(now - h.window) / 1e9;
    const auto q = [](std::vector<float>& v, double at) { return static_cast<long long>(Quantile(v, at)); };
    std::printf("Even presents (host-timed), %.1f s: %zu held, %llu frames late (not ready by the target), %llu "
                "released for the exit | vs target, us: our signal median %lld p99 %lld; GPU release median %lld "
                "p99 %lld max %lld (%llu over 200) | GPU wake-up after our signal median %lld p99 %lld, lead %lld "
                "| timer miss median %lld p99 %lld (%s), spin window %lld | spin per release avg %.0f max %.0f us "
                "(%.2f%% of a core)\n",
                seconds, n, static_cast<unsigned long long>(h.late_frames),
                static_cast<unsigned long long>(h.missed_limit), q(h.host_err, 0.5), q(h.host_err, 0.99),
                q(h.gpu_err, 0.5), q(h.gpu_err, 0.99), q(h.gpu_err, 1.0),
                static_cast<unsigned long long>(h.late_releases), q(h.wake_err, 0.5), q(h.wake_err, 0.99),
                static_cast<long long>(h.lead_ns.load(std::memory_order_relaxed) / 1000), q(h.sleep_err, 0.5),
                q(h.sleep_err, 0.99), h.high_resolution ? "high-resolution" : "standard",
                static_cast<long long>(h.spin_window_ns.load(std::memory_order_relaxed) / 1000),
                n ? spin_sum / double(n) : 0.0, spin_max, spin_sum / 1e4 / seconds);
    std::fflush(stdout);
    if (FILE* csv = HostCsv()) {
        std::fflush(csv);
    }
    h.host_err.clear();
    h.gpu_err.clear();
    h.wake_err.clear();
    h.sleep_err.clear();
    h.spin.clear();
    h.late_frames = h.late_releases = h.missed_limit = 0;
    h.window = now;
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
    slot = {.pending = true, .flip_arg = flip_arg, .until_ns = until_ns, .fenced = true};
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


void PresentRelease::Submitted(vk::Semaphore timeline, u64 tick) {
    submitted_timeline = timeline;
    for (Slot& slot : slots) {
        if (slot.pending && !slot.submitted) {
            slot.submitted = true;
            slot.tick = tick;
        }
    }
}

void PresentRelease::Poll(const std::function<void(s64, s64, s64)>& done) {
    if (!made) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    u64 reached = 0;
    bool reached_read = false;
    while (slots[oldest].pending) {
        Slot& slot = slots[oldest];
        // Only once its command buffer has run: until then its queries still hold the slot's last
        // use (16 holds earlier: frames behind the GPU fed old times to the offset and the GPU-bound
        // check), or were never written (its first use).
        if (!slot.submitted || !submitted_timeline) {
            return;
        }
        if (!reached_read) {
            u64 value = 0;
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue(device, submitted_timeline, &value) !=
                VK_SUCCESS) {
                return;
            }
            reached = value;
            reached_read = true;
        }
        if (reached < slot.tick ||
            (slot.fenced && fences[oldest] && device.getFenceStatus(*fences[oldest]) != vk::Result::eSuccess)) {
            return; // not run yet: the later ones neither
        }
        std::array<u64, 6> data{}; // value, availability: ready (graphics), hold start, release
        const vk::Result result = device.getQueryPoolResults(
            *pool, oldest * 3, 3, sizeof(data), data.data(), 2 * sizeof(u64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if ((result != vk::Result::eSuccess && result != vk::Result::eNotReady) || !data[1] ||
            !data[3] || !data[5]) {
            return; // not run yet: the later ones neither
        }
        const u64 started = stamps ? stamps[oldest] : 0;
        const u64 ready = data[0], entry = data[2], exit = data[4];
        if (host_timed) {
            HostStats(oldest, TicksToNs(ready), TicksToNs(exit));
        } else if (short_spin) {
            ShortSpinStats(oldest, TicksToNs(ready), TicksToNs(entry), TicksToNs(exit));
        }
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
