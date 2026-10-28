// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    u32 motion_vectors = 0;
    u32 ui_squeeze = 0; ///< bbport: ultrawide UI vertex shader (HwVertexRuntimeInfo::ui_squeeze_bits)
    u32 split_sets = 0; ///< bbport (BB_GPL): split layout, fragment set 0 + pre-raster set 1 (PipelineSelection::split_sets)
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

/// bbport (BB_GPL): one shared pipeline-library part and the layout view it was built with.
struct LibraryPart {
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout layout;
};

/// bbport (BB_GPL): the parts built so far, by part key; shared by every pipeline linked from them.
class LibraryParts {
public:
    static std::shared_ptr<LibraryPart> Find(u64 key);
    /// Keeps the first part inserted under a key (two threads may build the same part at once).
    static std::shared_ptr<LibraryPart> Insert(u64 key, vk::UniquePipeline pipeline,
                                               vk::UniquePipelineLayout layout);
};

/// bbport (BB_GPL): the key of a part, hashed from the values it is built from.
class PartKeyWriter {
public:
    explicit PartKeyWriter(u64 part) : hash{0x9E3779B97F4A7C15ull ^ part} {}
    template <typename T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        }
    }
    u64 Get() const {
        return hash;
    }

private:
    u64 hash;
};

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading);
    ~GraphicsPipeline();

    /// bbport: a pipeline built on a compiler thread from copies of the stage infos takes the
    /// live ones once it is published (they are refreshed for every draw).
    void PublishStages(std::span<const Shader::Info*, MaxShaderStages> infos) {
        std::ranges::copy(infos, stages.begin());
    }

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    /// bbport: points the stages at other Infos describing the same programs (a worker builds
    /// the pipeline from its own copies, which the pipeline must not keep).
    void RebindStages(const std::array<const Shader::Info*, MaxShaderStages>& infos) {
        std::ranges::copy(infos, stages.begin());
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// bbport (draw batching, BB_DRAW_BATCH=1): the batched variant (Profile::batched_graphics),
    /// built on the GPU command thread the first time this pipeline draws; null when it has none.
    mutable std::atomic<const GraphicsPipeline*> batch_variant{nullptr};
    mutable std::atomic<bool> batch_tried{false};
    /// Of the variant: per stage (fragment, vertex), whether its flat buffer was added (one more
    /// binding after the stage's buffers than the normal variant has).
    std::array<bool, 2> batch_added_flatbuf{};

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    void BuildDescSetLayout(bool preloading);
    /// bbport (BB_GPL): builds `pipeline` as four pipeline-library parts from the given state and links them.
    void BuildFromLibraries(vk::Device device, vk::PipelineCache pipeline_cache,
                            const std::string& debug_str,
                            std::span<const vk::DynamicState> dynamic_states,
                            std::span<const vk::PipelineShaderStageCreateInfo> shader_stages,
                            const vk::PipelineVertexInputStateCreateInfo& vertex_input_info,
                            const vk::PipelineInputAssemblyStateCreateInfo& input_assembly,
                            const vk::PipelineTessellationStateCreateInfo& tessellation_state,
                            const vk::PipelineViewportStateCreateInfo& viewport_info,
                            const vk::PipelineRasterizationStateCreateInfo& rasterization,
                            const vk::PipelineMultisampleStateCreateInfo& multisampling,
                            const vk::PipelineDepthStencilStateCreateInfo* depth_stencil,
                            const vk::PipelineColorBlendStateCreateInfo& color_blending,
                            const vk::PipelineRenderingCreateInfo& rendering);

private:
    GraphicsPipelineKey key;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    std::array<std::shared_ptr<LibraryPart>, 4> library_parts; ///< bbport (BB_GPL): the parts it was linked from
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
