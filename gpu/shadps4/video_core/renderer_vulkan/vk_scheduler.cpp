// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "bbport_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/landing_gate.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_threads.h"

namespace Vulkan {

namespace {
/// bbport diagnosis: BB_GPU_BUSY=1 — a timestamp where each command buffer of the draw scheduler
/// starts and ends; every 5 s: how busy the GPU was, its idle gaps, and how long submissions
/// took from submit to done (how far the GPU runs behind).
struct GpuBusy {
    static constexpr u32 Slots = 2048;
    static constexpr u32 Marks = 128; // segment timestamps per command buffer
    static constexpr u32 Labels = 16;
    vk::Device device;
    vk::QueryPool pool;
    vk::QueryPool marks; // Slots x Marks
    struct Block {
        std::array<u8, Marks> labels;
        u32 count;
        u8 initial;
    };
    std::vector<Block> blocks = std::vector<Block>(Slots);
    u8 last_label = 0;
    std::array<double, Labels> label_ns{};
    u64 overflowed = 0;
    double period_ns = 1.0;
    const Scheduler* owner = nullptr;
    s64 open = -1; // slot of the command buffer being recorded
    u64 next = 0;
    struct Pending {
        u32 slot;
        u64 tick;
        std::chrono::steady_clock::time_point submitted;
    };
    std::vector<Pending> pending;
    std::vector<std::pair<u64, u64>> spans; // GPU [start, end) of finished command buffers
    double latency_ms = 0;
    u64 latency_count = 0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();

    static GpuBusy* Get(const Instance& instance) {
        static GpuBusy* busy = [&]() -> GpuBusy* {
            const char* env = std::getenv("BB_GPU_BUSY");
            if (!env || env[0] != '1') {
                return nullptr;
            }
            auto* b = new GpuBusy;
            b->device = instance.GetDevice();
            b->period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
            b->pool = Check(b->device.createQueryPool({
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = Slots * 2,
            }));
            b->device.resetQueryPool(b->pool, 0, Slots * 2);
            b->marks = Check(b->device.createQueryPool({
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = Slots * Marks,
            }));
            b->device.resetQueryPool(b->marks, 0, Slots * Marks);
            std::printf("GPU busy: on (timestamp period %.2f ns)\n", b->period_ns);
            return b;
        }();
        return busy;
    }
};
} // namespace

std::mutex Scheduler::submit_mutex;

extern void (*g_gpu_stall_report)();
extern "C" void (*bbgpu_thread_stack_dump)(const char* reason); // bbgpu.cpp
bool (*g_draw_pipe_busy)() = nullptr;
/// bbport: runs the draws handed to the recording thread so far (set by the rasterizer).
void (*g_drain_draw_pipe)() = nullptr;

namespace {
// bbport: the last submissions to the graphics queue (all schedulers share it), printed when a
// GPU wait stalls: which timeline value each one signals and which semaphores it waits for.
struct SubmitRecord {
    const Scheduler* scheduler;
    VkSemaphore timeline;
    u64 signal;
    u32 waits;
    std::array<VkSemaphore, 4> wait_semaphore;
    std::array<u64, 4> wait_value;
};
std::mutex g_recent_mutex;
std::array<SubmitRecord, 32> g_recent;
u64 g_recent_count;
std::vector<std::pair<const Scheduler*, const Instance*>> g_schedulers;
} // namespace

void Scheduler::ReportStall() {
    std::scoped_lock lk{g_recent_mutex};
    for (const auto& [scheduler, inst] : g_schedulers) {
        const VkSemaphore timeline = scheduler->work_semaphore.Handle();
        const auto [result, value] = inst->GetDevice().getSemaphoreCounterValue(timeline);
        std::printf("GPU stall: scheduler %p timeline %p: GPU reached %llu, next tick %llu\n",
                    static_cast<const void*>(scheduler), static_cast<void*>(timeline),
                    static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(scheduler->work_semaphore.CurrentTick()));
    }
    const u64 count = std::min<u64>(g_recent_count, g_recent.size());
    for (u64 i = g_recent_count - count; i < g_recent_count; ++i) {
        const auto& r = g_recent[i % g_recent.size()];
        std::printf("GPU stall: submit #%llu scheduler %p signals %p=%llu, waits",
                    static_cast<unsigned long long>(i), static_cast<const void*>(r.scheduler),
                    static_cast<void*>(r.timeline), static_cast<unsigned long long>(r.signal));
        for (u32 w = 0; w < r.waits; ++w) {
            std::printf(" %p=%llu", static_cast<void*>(r.wait_semaphore[w]),
                        static_cast<unsigned long long>(r.wait_value[w]));
        }
        std::printf("\n");
    }
    std::fflush(stdout);
    // bbport: a tick nobody submits means the port's threads wait for each other: their stacks
    // name the cycle (once a session; bbgpu_thread_stack_dump, set by the host).
    if (bbgpu_thread_stack_dump) {
        bbgpu_thread_stack_dump("GPU stall");
    }
}

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    {
        std::scoped_lock lk{g_recent_mutex};
        g_schedulers.emplace_back(this, &instance);
        g_gpu_stall_report = &Scheduler::ReportStall;
    }
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    {
        std::scoped_lock lk{g_recent_mutex};
        std::erase_if(g_schedulers, [this](const auto& entry) { return entry.first == this; });
    }
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal, u64 landing_seq) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        if (VideoCore::LandingGate::Active()) {
            WaitDeferredSignals();
        }
        VideoCore::LandingGate::Wait(landing_seq);
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done,
            landing_seq](vk::CommandBuffer) mutable {
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done), landing_seq] {
            auto fire = [signal = std::move(signal), done = std::move(done)] {
                signal();
                done->fetch_add(1, std::memory_order_release);
            };
            // A signal behind deferred raycaster copies waits for them on the landing thread:
            // this thread must not block (submissions wait for the copy threads).
            VideoCore::LandingGate::Run(landing_seq, std::move(fire));
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        if (VideoCore::LandingGate::Stopped()) {
            break; // shutdown: the labels waiting on the landing thread were dropped
        }
        // Helps the copy threads the signals wait for.
        BbCopy::WaitAsync();
        std::this_thread::yield();
    }
}

void Scheduler::WaitHostCopies() {
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    BbCopy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    BbStats::WaitTimer timer{BbStats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:VkRecorder");
    BbThreads::MakeRenderThread();
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            BbThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        BbThreads::SampleCore(BbThreads::CoreVulkanRecording);
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread.
        chunk->Execute(current_cmdbuf);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::WaitSubmitted() {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1);
    }
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    work_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // bbport: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !BbToggle::Disabled(BbToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

bool Scheduler::SegmentsEnabled() const {
    auto* busy = GpuBusy::Get(instance);
    return busy && busy->owner == this;
}

void Scheduler::MarkSegment(u32 label) {
    auto* busy = GpuBusy::Get(instance);
    if (!busy || busy->owner != this) {
        return;
    }
    busy->last_label = u8(label);
    if (busy->open < 0) {
        return;
    }
    auto& block = busy->blocks[busy->open];
    if (block.count >= GpuBusy::Marks) {
        ++busy->overflowed;
        return;
    }
    const u32 index = block.count++;
    block.labels[index] = u8(label);
    const u32 query = u32(busy->open) * GpuBusy::Marks + index;
    Record([pool = busy->marks, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, query);
    });
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    if (auto* busy = GpuBusy::Get(instance); busy && busy->owner == this) {
        const u32 slot = u32(busy->next++ % GpuBusy::Slots);
        current_cmdbuf.resetQueryPool(busy->pool, slot * 2, 2);
        current_cmdbuf.resetQueryPool(busy->marks, slot * GpuBusy::Marks, GpuBusy::Marks);
        current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, busy->pool, slot * 2);
        busy->open = slot;
        busy->blocks[slot].count = 0;
        busy->blocks[slot].initial = busy->last_label;
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::scoped_lock lk{submit_mutex};
    // bbport: BB_SUBMIT_THREADS=1 names each thread that submits through this scheduler (once).
    static const bool submit_threads = std::getenv("BB_SUBMIT_THREADS") != nullptr;
    if (submit_threads) {
        static std::mutex seen_mutex;
        static std::vector<std::pair<const Scheduler*, u32>> seen;
        const u32 tid = static_cast<u32>(GetCurrentThreadId());
        std::scoped_lock seen_lk{seen_mutex};
        if (std::ranges::find(seen, std::pair<const Scheduler*, u32>{this, tid}) == seen.end()) {
            seen.emplace_back(this, tid);
            PWSTR description = nullptr;
            char name[64] = "?";
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
                WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr);
                LocalFree(description);
            }
            std::printf("Scheduler %p: first submission from thread %u '%s'%s\n",
                        static_cast<const void*>(this), tid, name, on_submit ? " (draw scheduler)" : "");
        }
    }
    // bbport: the GPU command thread must not submit while the draw recording thread still has
    // work in this command buffer (its sparse binds would miss this submission). Reported once
    // per caller with a stack (module offsets for llvm-symbolizer).
    if (on_submit && g_draw_pipe_busy && g_draw_pipe_busy()) {
        static std::mutex report_mutex;
        static std::vector<void*> reported;
        void* frames[24]{};
        const u16 count = CaptureStackBackTrace(1, 24, frames, nullptr);
        std::scoped_lock report_lk{report_mutex};
        if (count > 2 && std::ranges::find(reported, frames[1]) == reported.end()) {
            reported.push_back(frames[1]);
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            std::printf("Scheduler: GPU thread submits while the draw pipe is busy:");
            for (u16 i = 0; i < count; ++i) {
                std::printf(" %llx", static_cast<unsigned long long>(
                                         reinterpret_cast<uintptr_t>(frames[i]) - base));
            }
            std::printf("\n");
        }
    }
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    auto* busy = GpuBusy::Get(instance);
    if (busy && !busy->owner && on_submit) {
        busy->owner = this; // the draw scheduler; its next command buffer starts the timing
    }
    const bool busy_timed = busy && busy->owner == this && busy->open >= 0;
    if (busy_timed) {
        current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, busy->pool,
                                      u32(busy->open) * 2 + 1);
    }
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    {
        std::scoped_lock lk{g_recent_mutex};
        auto& record = g_recent[g_recent_count++ % g_recent.size()];
        record = {this, timeline, signal_value, info.num_wait_semas, {}, {}};
        for (u32 w = 0; w < info.num_wait_semas && w < 4; ++w) {
            record.wait_semaphore[w] = info.wait_semas[w];
            record.wait_value[w] = info.wait_ticks[w];
        }
    }
    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        std::printf("Device lost during submit\n");
        DeviceLost(instance.GetDevice());
    }

    work_semaphore.Refresh();
    if (busy_timed) {
        busy->pending.push_back({u32(busy->open), signal_value, std::chrono::steady_clock::now()});
        busy->open = -1;
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(busy->pending, [&](const GpuBusy::Pending& p) {
            if (!work_semaphore.IsFree(p.tick)) {
                return false;
            }
            u64 stamps[2]{};
            if (busy->device.getQueryPoolResults(busy->pool, p.slot * 2, 2, sizeof(stamps), stamps,
                                                 sizeof(u64), vk::QueryResultFlagBits::e64) ==
                    vk::Result::eSuccess &&
                stamps[1] >= stamps[0]) {
                busy->spans.emplace_back(stamps[0], stamps[1]);
                // GPU time between segment marks, charged to the label active before each.
                const auto& block = busy->blocks[p.slot];
                const u32 count = std::min(block.count, GpuBusy::Marks);
                std::array<u64, GpuBusy::Marks> marks{};
                if (count == 0 ||
                    busy->device.getQueryPoolResults(busy->marks, p.slot * GpuBusy::Marks, count,
                                                     count * sizeof(u64), marks.data(), sizeof(u64),
                                                     vk::QueryResultFlagBits::e64) ==
                        vk::Result::eSuccess) {
                    u64 previous = stamps[0];
                    u8 label = block.initial;
                    for (u32 i = 0; i < count; ++i) {
                        const u64 at = std::clamp(marks[i], previous, stamps[1]);
                        busy->label_ns[label % GpuBusy::Labels] += (at - previous) * busy->period_ns;
                        previous = at;
                        label = block.labels[i];
                    }
                    busy->label_ns[label % GpuBusy::Labels] += (stamps[1] - previous) * busy->period_ns;
                }
            }
            busy->latency_ms += std::chrono::duration<double, std::milli>(now - p.submitted).count();
            ++busy->latency_count;
            return true;
        });
        if (now - busy->window >= std::chrono::seconds(5) && busy->spans.size() > 2) {
            auto& spans = busy->spans;
            std::ranges::sort(spans);
            u64 covered = 0, cursor = spans.front().first, gaps_over_1ms = 0, gap_total = 0;
            for (const auto& [start, end] : spans) {
                if (start > cursor) {
                    const u64 gap = start - cursor;
                    gap_total += gap;
                    gaps_over_1ms += gap * busy->period_ns >= 1e6;
                }
                if (end > cursor) {
                    covered += end - std::max(start, cursor);
                    cursor = end;
                }
            }
            const double total = double(cursor - spans.front().first);
            std::printf("GPU busy (5 s): %.1f%% of the time, %zu command buffers, idle gaps %.2f "
                        "ms/s (%llu over 1 ms); upper bound on submit-to-done %.2f ms\n",
                        100.0 * covered / total, spans.size(),
                        gap_total * busy->period_ns / 1e6 / (total * busy->period_ns / 1e9),
                        (unsigned long long)gaps_over_1ms,
                        busy->latency_ms / std::max<u64>(busy->latency_count, 1));
            const double seconds = total * busy->period_ns / 1e9;
            std::printf("GPU busy by guest queue (ms/s):");
            for (u32 l = 0; l < GpuBusy::Labels; ++l) {
                if (busy->label_ns[l] > 0) {
                    std::printf(" q%u %.1f", l, busy->label_ns[l] / 1e6 / seconds);
                }
            }
            std::printf("%s\n", busy->overflowed ? " (some marks dropped)" : "");
            busy->label_ns = {};
            busy->overflowed = 0;
            spans.clear();
            busy->latency_ms = 0;
            busy->latency_count = 0;
            busy->window = now;
        }
    }
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
