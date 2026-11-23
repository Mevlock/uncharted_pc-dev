// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport (AMD warm start): the game's pipelines as the driver's own binaries (VK_KHR_pipeline_binary), kept in the
// shader pack (Storage::BlobType::PipelineBinary) and handed back to the driver at the next start.
//
// AMD's Windows driver keeps none of its compiled pipelines in a VkPipelineCache (vkGetPipelineCacheData stays ~820 KiB
// whatever it built), and its own disk cache did not make them fast: every start built every stored pipeline again
// (Uncharted 3 ~12 s with the game held, 2 ~4.7 s, 1 ~2 s on an RX 7800 XT). With this, a pipeline the pack holds the
// binaries of is created from them; any other is compiled with VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR and its
// binaries are saved.
//
// - One helper for every game pipeline (graphics, its HDR twin, compute), whoever creates it: the preload and the
//   variant ship, the draw-preparation workers and the GPU thread all build through the pipeline constructors, which
//   call it. The port's own pipelines (upscalers, frame generation, blits, ...) and the pipeline-library parts
//   (BB_GPL=1) stay on the plain path.
// - The blob's name is the driver's own key for the create info (vkGetPipelineKeyKHR): it covers the SPIR-V, the
//   specialization, the state and the formats, so a settings-dependent shader can never pick another's binary.
// - The "tag" blob names the driver the binaries are of (format version, vendor, device, driver version, pipeline
//   cache UUID, the driver's global key). Another driver: every stored binary is removed and this start compiles and
//   captures again. A binary the driver refuses is removed, then compiled and captured again.
// - Off (Instance::IsPipelineBinaryEnabled: not supported, NVIDIA, BB_PIPELINE_BINARY=0, or no shader cache): the
//   plain path, exactly as before, with the VkPipelineCache. On, the VkPipelineCache still serves a pipeline whose
//   capture the driver refused (the spec forbids a pipeline cache with captures and with binaries).
// - Thread-safe: each call works on its own pipeline and binaries; counters are atomics; the tag is checked once,
//   single-threaded, at the PipelineCache's construction (Setup), before any pipeline is made.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

namespace PipelineBinaries {

struct Created {
    vk::Result result;
    vk::UniquePipeline value;
};

/// Once, after the shader cache is open and before any pipeline is made: on when the instance enabled the extension
/// and the cache is open; checks the tag (another driver: the stored binaries are dropped).
void Setup(const Instance& instance);

/// True when the game's pipelines are created through the binaries this session.
bool Active();

/// Creates a game pipeline: from its stored binaries, else compiled and captured; off, in `cache` as before. Holds
/// PipelineCacheUse() shared around each creation. `preloading`: built by the preload (the stored and listed keys),
/// else by the game (the draw-preparation workers, the GPU thread): counted apart in the stats line.
Created CreateGraphics(const Instance& instance, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
                       bool preloading);
Created CreateCompute(const Instance& instance, vk::PipelineCache cache, const vk::ComputePipelineCreateInfo& info,
                      bool preloading);

/// Creations through the VkPipelineCache that the driver compiled (its creation feedback: not a hit in the cache),
/// since the start: the driver's cache data changed and is to be saved.
u64 DriverCacheMisses();

/// One log line with this session's counts (when active): "used, captured, refused, capture failed".
void PrintStats(const char* when);

} // namespace PipelineBinaries
} // namespace Vulkan
