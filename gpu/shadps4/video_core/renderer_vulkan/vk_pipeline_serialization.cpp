// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <xxhash.h>
#include "bbport_threads.h"
#include "bbport_overlay.h"
#include "bbport_toggles.h"
#include "common/hash.h"
#include "common/serdes.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/cta_split.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "bbport_focus_fix.h"
#include "bbport_nan_guard.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 21u; // bbport: shadPS4 #5447, #5427, #5364, #5365 (CFG fallthrough), #5429 (LDS barriers); page table watch bit
static constexpr u32 ShaderMetaVersion = 15u; // bbport: BufferSpecialization::coherent
static constexpr u32 PipelineKeyVersion = 7u; // bbport: GraphicsPipelineKey::split_sets
// bbport: translator options that change the shaders it produces are part of the stored version,
// so switching one retranslates the shaders instead of reusing the cached ones.
static u32 TranslatedBinaryVersion() {
    // The optional 512-thread CTA split (BB_PASCAL_CTA_SPLIT, cta_split.h) changes one shader;
    // the ReadConst window size (BB_READCONST_WINDOW_DWORDS) changes every shader with one, and
    // BB_WAVE_LDS_BARRIERS=0 the compute shaders of larger workgroups.
    return ShaderBinaryVersion | (Shader::Gcn::ScopeSplitScalar() ? 0u : 0x10000u) |
           (Shader::CtaSplitEnabled() ? 0x20000u : 0u) |
           (Shader::WaveLdsBarriers() ? 0u : 0x40000u) |
           ((Shader::ReadConstWindowDwords() >> 8) << 20);
}
} // namespace Serialization

namespace Vulkan {

bool BbMovieShaderScaled(u64 pgm_hash); // vk_pipeline_cache.cpp

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

// bbport: a shader is cached as two files, its meta and its binary, saved one after the other. If
// Windows refuses to replace one of them (another thread reads it), the pair comes from two
// translations and the shader draws with another version's code (BilalReloaded's port saw a
// museum floor drawn flat red from one such pair). The meta keeps a checksum of the binary
// saved just before it on the same thread; a pair that doesn't match is translated again.
struct SavedBinary {
    u64 pgm_hash;
    size_t perm_idx;
    u64 checksum;
};
static thread_local SavedBinary t_saved_binary{};

// bbport (shader variant list): the inputs of the variant translated last on this thread, for its meta.
struct SavedInputs {
    u64 pgm_hash;
    size_t perm_idx;
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data;
    std::vector<Shader::UdRead> reads;
};
static thread_local SavedInputs t_saved_inputs{};

void RegisterShaderInputs(const Shader::Info& info, const std::vector<Shader::UdRead>& reads,
                          size_t perm_idx) {
    t_saved_inputs.pgm_hash = info.pgm_hash;
    t_saved_inputs.perm_idx = perm_idx;
    t_saved_inputs.user_data.fill(0);
    std::memcpy(t_saved_inputs.user_data.data(), info.user_data.data(),
                std::min(info.user_data.size_bytes(), sizeof(t_saved_inputs.user_data)));
    t_saved_inputs.reads = reads;
}

static u64 BinaryChecksum(const std::vector<u32>& spv) {
    return XXH3_64bits(spv.data(), spv.size() * sizeof(u32)) | 1; // 0: no binary saved
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    // bbport: vertex shaders with the BB_VS_CHECK instrumentation embed a session-local address.
    if (Shader::VertexCheck::address && info.hw_stage == Shader::HwStage::Vertex) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::TranslatedBinaryVersion());

    meta.Write(perm_hash);
    meta.Write(perm_idx);
    const bool same = t_saved_binary.pgm_hash == info.pgm_hash && t_saved_binary.perm_idx == perm_idx;
    meta.Write(same ? t_saved_binary.checksum : u64{0});

    spec.Serialize(ar);
    info.Serialize(ar);
    // bbport (shader variant list): the translation inputs, when this thread just translated this variant.
    const bool inputs = t_saved_inputs.pgm_hash == info.pgm_hash && t_saved_inputs.perm_idx == perm_idx;
    meta.Write(u32{inputs ? 1u : 0u});
    if (inputs) {
        meta.Write(t_saved_inputs.user_data.data(), sizeof(t_saved_inputs.user_data));
        meta.Write(t_saved_inputs.reads);
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    t_saved_binary = {pgm_hash, perm_idx, BinaryChecksum(spv)};
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

/// bbport (shader variant list): the translation inputs stored with a variant's meta.
struct ShaderInputs {
    bool present = false;
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::vector<Shader::UdRead> reads;
};

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx,
                    u64* binary_checksum = nullptr, ShaderInputs* inputs = nullptr) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::TranslatedBinaryVersion()) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);
    u64 checksum{};
    meta.Read(checksum);
    if (binary_checksum) {
        *binary_checksum = checksum;
    }

    spec.Deserialize(ar);
    info.Deserialize(ar);
    u32 has_inputs{};
    meta.Read(has_inputs);
    if (has_inputs) {
        ShaderInputs scratch;
        ShaderInputs& in = inputs ? *inputs : scratch;
        in.present = true;
        meta.Read(in.user_data.data(), sizeof(in.user_data));
        meta.Read(in.reads);
    }

    // bbport: BB_VS_CHECK instruments every vertex shader: none comes from the cache.
    if (info.hw_stage == Shader::HwStage::Vertex && Shader::VertexCheck::address) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

namespace {

/// Runs fn(i) for i in [0, count) on up to `threads` threads (the caller is one of them).
template <typename Fn>
void ParallelFor(size_t count, u32 threads, Fn&& fn) {
    if (count == 0) {
        return;
    }
    threads = static_cast<u32>(std::max<size_t>(1, std::min<size_t>(threads, count)));
    std::atomic<size_t> next{0};
    const auto run = [&] {
        for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    for (u32 t = 1; t < threads; ++t) {
        pool.emplace_back([&] {
            BbThreads::MakeBelowNormal();
            run();
        });
    }
    run();
    for (auto& thread : pool) {
        thread.join();
    }
}

/// One stored shader permutation, read from the cache.
struct LoadedStage {
    std::unique_ptr<Program> program; ///< holds the stored Info until it is inserted
    Shader::StageSpecialization spec;
    size_t perm_idx{};
    vk::ShaderModule module{};
    bool parsed = false; ///< metadata read: its permutation slot is known
    // Filled by the serial insertion:
    Program* owner{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    bool ready = false; ///< owner and module are usable
    // bbport (shader variant list): no stored binary, but the translation inputs are known.
    ShaderInputs inputs{};
    u64 checksum{};
    bool replay = false;
    u64 pgm_hash{};
    Shader::HwStage hw_stage{};
    Shader::SwStage sw_stage{};
    std::vector<u32> flat; ///< the stored flattened user-data tables
    std::unique_ptr<Shader::Info> stored_info; ///< BB_VARIANT_SYNTH: its resource lists
};

/// bbport (shader variant list, BB_VARIANT_SYNTH=1): descriptor tables made from a variant's specialization alone, in
/// place of the recorded ones: each bound resource gets a made-up descriptor at its stored location carrying only what
/// the specialization keeps (view type, sRGB/integer, buffer stride/format/swizzle, sampler flags). If translation
/// gives the same binary from these, predicted variants (no recording at all) can be translated the same way.
static std::vector<u32> SynthesizeFlatBuffer(const Shader::Info& info,
                                             const Shader::StageSpecialization& spec,
                                             size_t size, bool copy_mtype) {
    // The stored Info still holds the recorded tables: the diagnostic mode reads them.
    const Shader::Info* recorded = copy_mtype ? &info : nullptr;
    std::vector<u32> flat(size, 0);
    const auto put = [&](const auto& fetch, const auto& value) {
        const auto* dwords = reinterpret_cast<const u32*>(&value);
        u8 mask = fetch.load_mask;
        for (size_t k = 0; k < fetch.N; ++k, mask >>= 1) {
            if ((mask & 1) && fetch.offsets[k] < flat.size()) {
                flat[fetch.offsets[k]] = dwords[k];
            }
        }
    };
    u32 binding = 0;
    for (size_t i = 0; i < info.buffers.size(); ++i, ++binding) {
        if (!spec.bitset.test(binding) || i >= spec.buffers.size()) {
            continue;
        }
        const auto& s = spec.buffers[i];
        AmdGpu::Buffer buffer{};
        buffer.base_address = 0x100000;
        buffer.num_records = 0x10000;
        buffer.stride = s.stride;
        buffer.swizzle_enable = s.swizzle_enable;
        buffer.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buffer.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        buffer.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);
        if (s.is_formatted) {
            buffer.data_format = s.data_format;
            buffer.num_format = s.num_format;
            buffer.dst_sel_x = u32(s.dst_select.r);
            buffer.dst_sel_y = u32(s.dst_select.g);
            buffer.dst_sel_z = u32(s.dst_select.b);
            buffer.dst_sel_w = u32(s.dst_select.a);
        }
        if (s.swizzle_enable) {
            buffer.index_stride = s.index_stride;
            buffer.element_size = s.element_size;
        }
        buffer.mtype = s.coherent ? 3 : 0;
        // Diagnostic (BB_VARIANT_SYNTH=2): the memory type of the recorded descriptor.
        if (recorded) {
            buffer.mtype = info.buffers[i].GetSharp(*recorded).mtype;
        }
        put(info.buffers[i].sharp_fetch, buffer);
    }
    for (size_t i = 0; i < info.images.size(); ++i, ++binding) {
        if (!spec.bitset.test(binding) || i >= spec.images.size()) {
            continue;
        }
        const auto& s = spec.images[i];
        auto image = AmdGpu::Image::Null(info.images[i].is_depth);
        image.base_address = 0x1000;
        image.width = 255;
        image.height = 255;
        image.type = u64(s.is_cube ? AmdGpu::ImageType::Cube : s.type);
        if (s.is_srgb) {
            image.num_format = u64(AmdGpu::NumberFormat::Srgb);
        } else if (s.is_integer) {
            image.num_format = u64(AmdGpu::NumberFormat::Uint);
        }
        if (s.is_storage) {
            image.dst_sel_x = u64(s.dst_select.r);
            image.dst_sel_y = u64(s.dst_select.g);
            image.dst_sel_z = u64(s.dst_select.b);
            image.dst_sel_w = u64(s.dst_select.a);
        }
        put(info.images[i].sharp_fetch, image);
    }
    for (size_t i = 0; i < info.samplers.size() && i < spec.samplers.size(); ++i) {
        AmdGpu::Sampler sampler{};
        sampler.force_unnormalized.Assign(spec.samplers[i].force_unnormalized);
        sampler.force_degamma.Assign(spec.samplers[i].force_degamma);
        put(info.samplers[i].sharp_fetch, sampler);
    }
    return flat;
}

/// bbport (shader variant list, BB_VARIANT_LIST): the descriptors of a variant's resources made from its
/// specialization, by resource index (Shader::SharpOverride), for building it with no game memory at all.
static Shader::SharpOverride MakeSharpOverride(const Shader::StageSpecialization& spec) {
    Shader::SharpOverride ov;
    u32 binding = 0;
    for (const auto& s : spec.buffers) {
        AmdGpu::Buffer buffer{};
        if (spec.bitset.test(binding++)) {
            buffer.base_address = 0x100000;
            buffer.num_records = 0x10000;
            buffer.stride = s.stride;
            buffer.swizzle_enable = s.swizzle_enable;
            buffer.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
            buffer.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
            buffer.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
            buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);
            if (s.is_formatted) {
                buffer.data_format = s.data_format;
                buffer.num_format = s.num_format;
                buffer.dst_sel_x = u32(s.dst_select.r);
                buffer.dst_sel_y = u32(s.dst_select.g);
                buffer.dst_sel_z = u32(s.dst_select.b);
                buffer.dst_sel_w = u32(s.dst_select.a);
            }
            if (s.swizzle_enable) {
                buffer.index_stride = s.index_stride;
                buffer.element_size = s.element_size;
            }
            buffer.mtype = s.coherent ? 3 : 0;
        }
        ov.buffers.push_back(buffer);
    }
    for (const auto& s : spec.images) {
        AmdGpu::Image image{};
        if (spec.bitset.test(binding++)) {
            image = AmdGpu::Image::Null(false);
            image.base_address = 0x1000;
            image.width = 255;
            image.height = 255;
            image.type = u64(s.is_cube ? AmdGpu::ImageType::Cube : s.type);
            if (s.is_srgb) {
                image.num_format = u64(AmdGpu::NumberFormat::Srgb);
            } else if (s.is_integer) {
                image.num_format = u64(AmdGpu::NumberFormat::Uint);
            }
            if (s.is_storage) {
                image.dst_sel_x = u64(s.dst_select.r);
                image.dst_sel_y = u64(s.dst_select.g);
                image.dst_sel_z = u64(s.dst_select.b);
                image.dst_sel_w = u64(s.dst_select.a);
            }
        }
        ov.images.push_back(image);
    }
    for (const auto& s : spec.samplers) {
        AmdGpu::Sampler sampler{};
        sampler.force_unnormalized.Assign(s.force_unnormalized);
        sampler.force_degamma.Assign(s.force_degamma);
        ov.samplers.push_back(sampler);
    }
    return ov;
}

/// One variant as a text line (BB_VARIANT_LIST): what the game files cannot give, nothing else. Fields are
/// tab-separated key=value; structures are hex of their bytes, lists '|'-separated:
/// h shader hash, hw/sw stage, perm, ck binary checksum (reference only); cb color buffers, in input linkage,
/// fsx dual-source.clip-distance.samples.motion; vsx clip-disable.user-clip-planes.motion.squeeze.depth-emulation;
/// sw software-stage part; start bindings; bits bound resources; buf/img/samp/attr/fm specializations; fetch.
static std::string HexBytes(const void* data, size_t size) {
    std::string out;
    const auto* b = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        out += fmt::format("{:02x}", b[i]);
    }
    return out;
}

static bool UnhexBytes(std::string_view hex, void* data, size_t size) {
    if (hex.size() != size * 2) {
        return false;
    }
    auto* b = static_cast<u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        b[i] = u8(std::stoul(std::string{hex.substr(i * 2, 2)}, nullptr, 16));
    }
    return true;
}

template <typename T>
static std::string HexList(const T& list) {
    std::string out;
    for (const auto& e : list) {
        out += HexBytes(&e, sizeof(e)) + "|";
    }
    return out;
}

template <typename T>
static bool UnhexList(std::string_view text, T& list) {
    list.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('|', pos);
        auto& e = list.emplace_back();
        if (!UnhexBytes(text.substr(pos, end - pos), &e, sizeof(e))) {
            return false;
        }
        pos = end == std::string_view::npos ? text.size() : end + 1;
    }
    return true;
}

static std::string VariantLine(u64 hash, size_t perm, u64 checksum,
                               const Shader::StageSpecialization& spec) {
    const auto& rt = spec.runtime_info;
    std::string line = fmt::format("h={:016x}\thw={}\tsw={}\tperm={}\tck={:016x}", hash, u32(rt.hw_stage),
                                   u32(rt.sw_stage), perm, checksum);
    if (rt.hw_stage == Shader::HwStage::Fragment) {
        const auto& fs = rt.hw.fs;
        line += "\tcb=" + HexList(fs.color_buffers);
        line += "\tin=";
        for (u32 i = 0; i < fs.num_inputs && i < fs.inputs.size(); ++i) {
            line += HexBytes(&fs.inputs[i], sizeof(fs.inputs[i])) + "|";
        }
        line += fmt::format("\tfsx={}.{}.{}.{}", u32(fs.dual_source_blending),
                            u32(fs.clip_distance_emulation), fs.num_samples, u32(fs.motion_vectors));
    } else if (rt.hw_stage == Shader::HwStage::Vertex) {
        const auto& vs = rt.hw.vs;
        line += fmt::format("\tvsx={}.{}.{}.{:x}.{}", u32(vs.clip_disable), vs.user_clip_plane_mask,
                            u32(vs.motion_vectors), vs.ui_squeeze_bits,
                            u32(vs.emulate_depth_negative_one_to_one));
    }
    line += "\tswi=" + HexBytes(&rt.sw, sizeof(rt.sw));
    line += "\tstart=" + HexBytes(&spec.start, sizeof(spec.start));
    line += "\tbits=" + spec.bitset.to_string();
    line += "\tbuf=" + HexList(spec.buffers);
    line += "\timg=" + HexList(spec.images);
    line += "\tsamp=" + HexList(spec.samplers);
    line += "\tattr=" + HexList(spec.vs_attribs);
    line += "\tfm=" + HexList(spec.fmasks);
    line += "\tfetch=";
    if (spec.fetch_shader_data) {
        Serialization::Archive ar;
        spec.fetch_shader_data->Serialize(ar);
        const auto bytes = ar.TakeOff();
        line += HexBytes(bytes.data(), bytes.size());
    }
    return line;
}

} // namespace

struct ListedVariant {
    u64 hash{};
    size_t perm{};
    std::vector<u64> checksums; ///< reference binaries (any one counts)
    Shader::StageSpecialization spec;
    /// A predicted line: image setups by descriptor-table slot (imgslot=) instead of in translation order.
    std::vector<Shader::ImageSpecialization> slot_images;
    bool by_slot = false;
    /// startps=<hash>.<perm>: a vertex shader whose bindings start where that listed pixel shader's end (the stage
    /// bound before it), worked out when the pixel shader is built.
    bool start_from_ps = false;
    u64 start_ps_hash{};
    size_t start_ps_perm{};
};

namespace {

static std::optional<ListedVariant> ParseVariantLine(const std::string& line) {
    std::map<std::string, std::string> f;
    size_t pos = 0;
    while (pos < line.size()) {
        size_t end = line.find('\t', pos);
        if (end == std::string::npos) {
            end = line.size();
        }
        const auto field = line.substr(pos, end - pos);
        const auto eq = field.find('=');
        if (eq != std::string::npos) {
            f[field.substr(0, eq)] = field.substr(eq + 1);
        }
        pos = end + 1;
    }
    if (!f.contains("h") || !f.contains("hw")) {
        return std::nullopt;
    }
    try {
        ListedVariant v;
        v.hash = std::stoull(f["h"], nullptr, 16);
        v.perm = f.contains("perm") ? std::stoull(f["perm"]) : 0;
        for (size_t a = 0; f.contains("ck") && a < f["ck"].size();) {
            const size_t b = std::min(f["ck"].find(',', a), f["ck"].size());
            v.checksums.push_back(std::stoull(f["ck"].substr(a, b - a), nullptr, 16));
            a = b + 1;
        }
        auto& rt = v.spec.runtime_info;
        rt.Initialize(Shader::HwStage(std::stoul(f["hw"])), Shader::SwStage(std::stoul(f["sw"])));
        if (rt.hw_stage == Shader::HwStage::Fragment) {
            auto& fs = rt.hw.fs;
            std::vector<Shader::PsColorBuffer> cbs;
            if (!UnhexList(f["cb"], cbs) || cbs.size() != fs.color_buffers.size()) {
                return std::nullopt;
            }
            std::ranges::copy(cbs, fs.color_buffers.begin());
            // No in=: each input reads the vertex output of its own index (what the game's linkage gives).
            if (!f.contains("in")) {
                for (u32 i = 0; i < fs.inputs.size(); ++i) {
                    fs.inputs[i] = {.param_index = u8(i)};
                }
            } else {
                std::vector<Shader::HwFragmentRuntimeInfo::PsInput> inputs;
                if (!UnhexList(f["in"], inputs) || inputs.size() > fs.inputs.size()) {
                    return std::nullopt;
                }
                std::ranges::copy(inputs, fs.inputs.begin());
            }
            unsigned dual = 0, cde = 0, samples = 1, motion = 0;
            std::sscanf(f["fsx"].c_str(), "%u.%u.%u.%u", &dual, &cde, &samples, &motion);
            fs.dual_source_blending = dual;
            fs.clip_distance_emulation = cde;
            fs.num_samples = u8(samples);
            fs.motion_vectors = motion;
        } else if (rt.hw_stage == Shader::HwStage::Vertex) {
            auto& vs = rt.hw.vs;
            unsigned clip = 0, ucp = 0, motion = 0, squeeze = 0, neg = 0;
            std::sscanf(f["vsx"].c_str(), "%u.%u.%u.%x.%u", &clip, &ucp, &motion, &squeeze, &neg);
            vs.clip_disable = clip;
            vs.user_clip_plane_mask = ucp;
            vs.motion_vectors = motion;
            vs.ui_squeeze_bits = squeeze;
            vs.emulate_depth_negative_one_to_one = neg;
        }
        // Missing fields take their usual values (zero software-stage part and bindings, no buffers, default samplers).
        if ((f.contains("swi") && !UnhexBytes(f["swi"], &rt.sw, sizeof(rt.sw))) ||
            (f.contains("start") && !UnhexBytes(f["start"], &v.spec.start, sizeof(v.spec.start)))) {
            return std::nullopt;
        }
        if (f.contains("startps")) {
            const auto dot = f["startps"].find('.');
            v.start_from_ps = true;
            v.start_ps_hash = std::stoull(f["startps"].substr(0, dot), nullptr, 16);
            v.start_ps_perm = dot == std::string::npos ? 0 : std::stoull(f["startps"].substr(dot + 1));
        }
        if (f.contains("imgslot")) {
            v.by_slot = true;
            if (!UnhexList(f["imgslot"], v.slot_images)) {
                return std::nullopt;
            }
        }
        v.spec.bitset = std::bitset<Shader::StageSpecialization::MaxStageResources>(f["bits"]);
        if (!UnhexList(f["buf"], v.spec.buffers) || !UnhexList(f["img"], v.spec.images) ||
            !UnhexList(f["samp"], v.spec.samplers) || !UnhexList(f["attr"], v.spec.vs_attribs) ||
            !UnhexList(f["fm"], v.spec.fmasks)) {
            return std::nullopt;
        }
        if (!f["fetch"].empty()) {
            std::vector<u8> bytes(f["fetch"].size() / 2);
            UnhexBytes(f["fetch"], bytes.data(), bytes.size());
            Serialization::Archive ar{std::move(bytes)};
            Shader::Gcn::FetchShaderData fetch;
            fetch.Deserialize(ar);
            v.spec.fetch_shader_data = fetch;
        }
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

/// bbport (shader variant list): where a shader sits in the game's files: a file under BB_APP0_DIR and the offset of
/// its OrbShdr binary info block (shader_hash = the port's program hash, length = code bytes). The shaders sit there
/// uncompressed; the locations are the same for every copy of a game version.
struct ShaderLocation {
    u32 file;
    u64 at;
    u32 length;
};
struct ShaderLocations {
    std::vector<std::string> files; ///< relative to BB_APP0_DIR, '/' separated
    std::unordered_map<u64, ShaderLocation> shaders;
};

static std::filesystem::path ShaderLocationsPath(const std::filesystem::path& app0) {
    using namespace Common::FS;
    return GetUserPath(PathType::CacheDir) /
           ("shader_locations_" + app0.filename().string() + ".txt");
}

/// Text: "F <path>" lines number the files from 0, then "<hash hex> <file> <offset> <length>" per shader.
static ShaderLocations LoadShaderLocations(const std::filesystem::path& path) {
    ShaderLocations locations;
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        return locations;
    }
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == 'F' && line[1] == ' ') {
            std::string name{line + 2};
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
                name.pop_back();
            }
            locations.files.push_back(std::move(name));
            continue;
        }
        unsigned long long hash, at;
        unsigned file, length;
        if (std::sscanf(line, "%llx %u %llu %u", &hash, &file, &at, &length) == 4 &&
            file < locations.files.size()) {
            locations.shaders.emplace(hash, ShaderLocation{file, at, length});
        }
    }
    std::fclose(f);
    return locations;
}

static void SaveShaderLocations(const std::filesystem::path& path, const ShaderLocations& locations) {
    const auto temp = std::filesystem::path{path}.concat(".tmp");
    std::FILE* f = _wfopen(temp.c_str(), L"wb");
    if (!f) {
        return;
    }
    for (const auto& name : locations.files) {
        std::fprintf(f, "F %s\n", name.c_str());
    }
    std::vector<std::pair<u64, ShaderLocation>> sorted(locations.shaders.begin(),
                                                       locations.shaders.end());
    std::ranges::sort(sorted, {}, &std::pair<u64, ShaderLocation>::first);
    for (const auto& [hash, loc] : sorted) {
        std::fprintf(f, "%016llx %u %llu %u\n", (unsigned long long)hash, loc.file,
                     (unsigned long long)loc.at, loc.length);
    }
    const bool ok = std::fclose(f) == 0;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(temp, path, ec);
    } else {
        std::filesystem::remove(temp, ec);
    }
}

/// The code of the shader whose info block is at `at` in `f`, if that block still names `hash`. The code starts with
/// s_mov_b32 vcc_hi, n where the info block is (n + 1) * 2 dwords on (AmdGpu::SearchBinaryInfo), or ends at the block.
/// The shader type each shader's binary info block gives (0 pixel, 1 vertex, 4 compute), as its code is read.
static std::mutex g_shader_types_mutex;
static std::unordered_map<u64, u32> g_shader_types;

static std::optional<std::vector<u32>> ReadShaderCodeAt(std::FILE* f, u64 at, u64 hash, u32 length,
                                                       std::vector<u32>* registers = nullptr) {
    if (at < 8 || length < 8 || length >= (1u << 22)) {
        return std::nullopt;
    }
    const u64 window = std::min<u64>(at, u64(length) + 0x10000);
    std::vector<u8> bytes(window + 28);
    if (_fseeki64(f, s64(at - window), SEEK_SET) != 0 ||
        std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        return std::nullopt;
    }
    u64 info_hash;
    std::memcpy(&info_hash, bytes.data() + window + 16, 8);
    if (std::memcmp(bytes.data() + window, "OrbShdr", 7) != 0 || info_hash != hash) {
        return std::nullopt;
    }
    {
        u32 bits;
        std::memcpy(&bits, bytes.data() + window + 8, 4);
        std::scoped_lock lk{g_shader_types_mutex};
        g_shader_types[hash] = (bits >> 2) & 0xF;
    }
    s64 start = -1;
    for (u64 back = 8; back <= window; ++back) {
        u32 w[2];
        std::memcpy(w, bytes.data() + window - back, 8);
        if (w[0] == 0xBEEB03FFu && (u64(w[1]) + 1) * 8 == back) {
            start = s64(window - back);
            break;
        }
    }
    if (start < 0 && length <= window) {
        start = s64(window - length);
    }
    if (start < 0 || u64(start) + length > bytes.size()) {
        return std::nullopt;
    }
    std::vector<u32> code(length / 4);
    std::memcpy(code.data(), bytes.data() + start, code.size() * 4);
    // The GNM stage registers sit right in front of the code, after a 0xffffffff address placeholder (PS: rsrc1,
    // rsrc2, z format, color format, input enable, input address, input control, barycentric control, depth shader
    // control, color mask; VS: rsrc1, rsrc2, output config, position format, clip output control; CS: rsrc1, rsrc2,
    // threads x, y, z), followed by the semantic table.
    if (registers) {
        registers->clear();
        const s64 head = std::max<s64>(0, start - 0x100);
        for (s64 o = start - 4; o >= head; o -= 4) {
            u32 w;
            std::memcpy(&w, bytes.data() + o, 4);
            if (w == 0xFFFFFFFFu) {
                registers->resize(size_t(start - o - 4) / 4);
                std::memcpy(registers->data(), bytes.data() + o + 4, registers->size() * 4);
                break;
            }
        }
    }
    return code;
}

/// Every shader info block in the game's files.
static ShaderLocations ScanShaderLocations(const std::filesystem::path& app0, u64* files_scanned) {
    ShaderLocations locations;
    static constexpr size_t chunk = 64ull << 20;
    std::vector<char> buf(chunk + 64);
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(app0, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->file_size(ec) < 64) {
            continue;
        }
        std::FILE* f = _wfopen(it->path().c_str(), L"rb");
        if (!f) {
            continue;
        }
        ++*files_scanned;
        const u32 file = u32(locations.files.size());
        bool any = false;
        size_t carry = 0;
        u64 base = 0;
        while (true) {
            const size_t got = std::fread(buf.data() + carry, 1, chunk, f);
            const size_t have = carry + got;
            if (have < 28) {
                break;
            }
            // The last 27 bytes go into the next chunk (an info block is 28).
            const size_t scan_end = have - 27;
            for (const char* p = buf.data();
                 (p = static_cast<const char*>(std::memchr(p, 'O', buf.data() + scan_end - p)));
                 ++p) {
                if (std::memcmp(p, "OrbShdr", 7) != 0) {
                    continue;
                }
                u32 bits;
                u64 hash;
                std::memcpy(&bits, p + 8, 4);
                std::memcpy(&hash, p + 16, 8);
                const u32 length = bits >> 8;
                if (hash && length >= 8 && length < (1u << 22)) {
                    any |= locations.shaders
                               .try_emplace(hash, ShaderLocation{file, base + u64(p - buf.data()),
                                                                 length})
                               .second;
                }
            }
            if (!got) {
                break;
            }
            carry = have - scan_end;
            std::memmove(buf.data(), buf.data() + scan_end, carry);
            base += scan_end;
        }
        std::fclose(f);
        if (any) {
            locations.files.push_back(
                std::filesystem::relative(it->path(), app0, ec).generic_string());
        }
    }
    return locations;
}

/// The code of the given shaders, from their saved locations; a scan of the game's files finds (and saves) the
/// locations when one is missing or no longer right.
static std::unordered_map<u64, std::vector<u32>> ReadGameShaderCode(
    const std::set<u64>& wanted, u64* files_scanned,
    std::unordered_map<u64, std::vector<u32>>* registers = nullptr) {
    std::unordered_map<u64, std::vector<u32>> found;
    const char* app0_env = std::getenv("BB_APP0_DIR");
    if (!app0_env || wanted.empty()) {
        return found;
    }
    const std::filesystem::path app0{app0_env};
    const auto locations_path = ShaderLocationsPath(app0);
    u32 stale = 0, opened = 0, unreadable = 0;
    // One game file open at a time (the C runtime allows about 512 at once; a level's shaders come from hundreds),
    // the shaders of each read in file order.
    const auto read_from = [&](const ShaderLocations& locations) {
        std::map<u32, std::vector<std::pair<u64, u64>>> by_file; // file -> (offset, hash)
        for (const u64 hash : wanted) {
            const auto it = locations.shaders.find(hash);
            if (!found.contains(hash) && it != locations.shaders.end()) {
                by_file[it->second.file].emplace_back(it->second.at, hash);
            }
        }
        for (auto& [file, shaders] : by_file) {
            std::FILE* f = _wfopen((app0 / locations.files[file]).c_str(), L"rb");
            if (!f) {
                stale += u32(shaders.size());
                continue;
            }
            std::ranges::sort(shaders);
            for (const auto& [at, hash] : shaders) {
                ++opened;
                std::vector<u32> regs;
                if (auto code = ReadShaderCodeAt(f, at, hash, locations.shaders.at(hash).length,
                                                 registers ? &regs : nullptr)) {
                    if (registers) {
                        (*registers)[hash] = std::move(regs);
                    }
                    found.emplace(hash, std::move(*code));
                } else {
                    ++unreadable;
                }
            }
            std::fclose(f);
        }
    };
    // The saved locations come from a scan of every game file: a shader missing there is not in the game files. Only
    // no saved locations, or one that no longer matches (the files changed), scans again.
    const auto saved = LoadShaderLocations(locations_path);
    read_from(saved);
    if (saved.shaders.empty() || stale > 0) {
        std::printf("Shader locations: %s: scanning the game files\n",
                    saved.shaders.empty() ? "none saved" : "a saved file cannot be opened");
        unreadable = 0;
        const auto scanned = ScanShaderLocations(app0, files_scanned);
        read_from(scanned);
        SaveShaderLocations(locations_path, scanned);
    }
    if (unreadable > 0) {
        std::printf("Shader locations: %zu of %zu shaders read; %u saved locations did not read back\n",
                    found.size(), wanted.size(), unreadable);
    }
    return found;
}

/// One stored pipeline key.
struct StoredPipeline {
    bool parsed = false;
    bool compute = false;
    GraphicsPipelineKey gkey{};
    GraphicsPipeline::SerializationSupport gsdata{};
    ComputePipelineKey ckey{};
    ComputePipeline::SerializationSupport csdata{};
    std::unique_ptr<GraphicsPipeline> gpipeline;
    std::unique_ptr<ComputePipeline> cpipeline;
};

} // namespace

void PipelineCache::StartWarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }
    // BB_WARM_MODE=0: no preload (the pipelines compile as the game needs them).
    if (const char* mode = std::getenv("BB_WARM_MODE"); mode && mode[0] == '0') {
        Storage::DataBase::Instance().FinishPreload();
        warm_done.store(true, std::memory_order_release);
        return;
    }
    Storage::DataBase::Instance().Open();
    warm_done.store(false, std::memory_order_release);
    warm_thread = std::thread([this] { WarmUpThread(); });
}

void PipelineCache::WaitWarm() {
    warm_done.wait(false, std::memory_order_acquire);
}

/// bbport (shader variant study, BB_SPEC_DUMP=<file>): every stored variant's specialization, one line each, grouped by
/// where each part comes from: the shader's own registers (props), the render pass (fs color buffers, z, samples),
/// the paired stage (fs inputs, vs outputs), the resources (buffers, images, samplers) and the mesh (vs attributes,
/// fetch layout). Reads cache metas of version 13 and 14. Offline analysis: scratchpad specstudy.py.
template <typename T>
static std::string HexOf(const T& value) {
    std::string out;
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) {
        out += fmt::format("{:02x}", bytes[i]);
    }
    return out;
}

static void DumpSpecs(Storage::DataBase& db, const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        return;
    }
    // BB_SPEC_DUMP_LINES=<file>: the same variants as BB_VARIANT_LIST lines.
    const char* lines_path = std::getenv("BB_SPEC_DUMP_LINES");
    std::FILE* lines = lines_path ? std::fopen(lines_path, "w") : nullptr;
    u32 count = 0;
    db.ForEachBlob(Storage::BlobType::ShaderMeta, [&](std::vector<u8>&& data) {
        // Version 12 stored Bindings without descriptor_set (its last member, after the 32-byte header).
        if (data.size() > 44 && data[0] == 12) {
            data.insert(data.begin() + 32 + 12, 4, u8{0});
            data[0] = 13;
        }
        Serialization::Archive ar{std::move(data)};
        Serialization::Reader r{ar};
        u32 version{}, binary_version{};
        r.Read(version);
        r.Read(binary_version);
        if (version < 13 || version > 15) {
            return;
        }
        size_t perm_hash{}, perm_idx{};
        u64 checksum{};
        r.Read(perm_hash);
        r.Read(perm_idx);
        r.Read(checksum);
        Shader::StageSpecialization spec;
        spec.Deserialize(ar);
        Shader::Info info;
        info.Deserialize(ar);
        u32 reads = 0;
        if (version >= 14) {
            u32 has_inputs{};
            r.Read(has_inputs);
            if (has_inputs) {
                std::array<u32, 16> user_data;
                std::vector<Shader::UdRead> ud_reads;
                r.Read(user_data.data(), sizeof(user_data));
                r.Read(ud_reads);
                reads = u32(ud_reads.size()) + 1;
            }
        }
        const auto& rt = spec.runtime_info;
        std::string line = fmt::format("{:016x}\t{}\t{}\treads={}\tprops={}", info.pgm_hash, perm_idx,
                                       u32(info.hw_stage), reads, HexOf(rt.props));
        switch (info.hw_stage) {
        case Shader::HwStage::Fragment: {
            const auto& fs = rt.hw.fs;
            std::string cbs;
            for (const auto& cb : fs.color_buffers) {
                cbs += fmt::format("{}.{}.{}.{}.{}.{}|", u32(cb.data_format), u32(cb.num_format),
                                   u32(cb.num_conversion), u32(cb.export_format),
                                   u32(cb.blend_self_scale), HexOf(cb.swizzle));
            }
            std::string inputs;
            for (u32 i = 0; i < fs.num_inputs && i < fs.inputs.size(); ++i) {
                const auto& in = fs.inputs[i];
                inputs += fmt::format("{}.{}.{}.{}|", in.param_index, u32(in.is_default),
                                      u32(in.is_flat), in.default_value);
            }
            line += fmt::format("\tpass=cb:{}z:{},s:{},mrtz:{},dual:{},mv:{}\tlink=en:{},addr:{},in:{}",
                                cbs, u32(fs.z_export_format), fs.num_samples, fs.mrtz_mask,
                                u32(fs.dual_source_blending), u32(fs.motion_vectors),
                                HexOf(fs.en_flags), HexOf(fs.addr_flags), inputs);
            break;
        }
        case Shader::HwStage::Vertex: {
            const auto& vs = rt.hw.vs;
            line += fmt::format("\tpass=clip:{},ucp:{},mv:{},sq:{},neg:{}\tlink=out:{},{}\tstep={},{},{},{},{}",
                                u32(vs.clip_disable), vs.user_clip_plane_mask, u32(vs.motion_vectors),
                                vs.ui_squeeze_bits, u32(vs.emulate_depth_negative_one_to_one),
                                vs.num_outputs, HexOf(vs.outputs), rt.sw.vs.step_rate_0,
                                rt.sw.vs.step_rate_1, rt.sw.vs.vertex_sgpr_offset,
                                rt.sw.vs.instance_sgpr_offset, u32(rt.sw.vs.tess_emulated_primitive));
            break;
        }
        case Shader::HwStage::Compute: {
            const auto& cs = rt.hw.cs;
            line += fmt::format("\tdispatch=lds:{},wg:{}x{}x{},tg:{}{}{}", cs.shared_memory_size,
                                cs.workgroup_size[0], cs.workgroup_size[1], cs.workgroup_size[2],
                                u32(cs.tgid_enable[0]), u32(cs.tgid_enable[1]),
                                u32(cs.tgid_enable[2]));
            break;
        }
        default:
            line += "\tother=" + HexOf(rt.hw);
            break;
        }
        std::string bufs, imgs, samps, attribs;
        for (size_t i = 0; i < spec.buffers.size(); ++i) {
            const auto& b = spec.buffers[i];
            bufs += fmt::format("{}.{}.{}.{}.{}.{}.{}.{}|", u32(b.stride), u32(b.is_formatted),
                                b.is_formatted ? u32(b.data_format) : 0u,
                                b.is_formatted ? u32(b.num_format) : 0u, u32(b.swizzle_enable),
                                b.swizzle_enable ? u32(b.index_stride) : 0u,
                                b.swizzle_enable ? u32(b.element_size) : 0u, u32(b.coherent));
        }
        for (const auto& im : spec.images) {
            imgs += fmt::format("{}.{}.{}.{}.{}.{}.{}|", u32(im.type), u32(im.is_integer),
                                u32(im.is_storage), u32(im.is_cube), u32(im.is_srgb),
                                u32(im.num_conversion), im.num_bindings);
        }
        for (const auto& sm : spec.samplers) {
            samps += fmt::format("{}.{}|", u32(sm.force_unnormalized), u32(sm.force_degamma));
        }
        for (const auto& a : spec.vs_attribs) {
            attribs += fmt::format("{}.{}.{}|", a.divisor, u32(a.num_class), HexOf(a.dst_select));
        }
        std::string fetch;
        if (spec.fetch_shader_data) {
            for (const auto& a : spec.fetch_shader_data->attributes) {
                fetch += fmt::format("{}.{}.{}.{}|", a.num_elements, a.data_format, a.num_format,
                                     a.instance_data);
            }
        }
        // Where each image's descriptor is read from (flattened user data dword), for slot order.
        std::string imgloc;
        for (const auto& im : info.images) {
            imgloc += fmt::format("{}.{}|", u32(im.sharp_fetch.offsets[0]), u32(im.sharp_fetch.load_mask));
        }
        line += fmt::format("\tbits={}\tbuf={}\timg={}\tsamp={}\tattr={}\tfetch={}\timgloc={}\n",
                            spec.bitset.to_string().substr(96), bufs, imgs, samps, attribs, fetch,
                            imgloc);
        std::fputs(line.c_str(), f);
        if (lines) {
            std::fprintf(lines, "%s\n", VariantLine(info.pgm_hash, perm_idx, checksum, spec).c_str());
        }
        ++count;
    });
    std::fclose(f);
    if (lines) {
        std::fclose(lines);
    }
    std::printf("Spec dump: %u variants to %s\n", count, path);
}

/// bbport (shader variant list): the runtime info of a stage built by BuildRuntimeInfo from the stage registers in
/// front of its code in the game files, plus the per-draw state the files cannot give (taken from `draw`: color
/// buffers, input linkage, clip state, motion vectors, the software-stage part).
Shader::RuntimeInfo PipelineCache::RuntimeInfoFromRegisters(Shader::HwStage hw_stage,
                                                            Shader::SwStage sw_stage,
                                                            const std::vector<u32>& r,
                                                            const Shader::RuntimeInfo& draw) {
    static thread_local std::unique_ptr<AmdGpu::Regs> storage;
    if (!storage) {
        storage = std::make_unique<AmdGpu::Regs>();
    }
    auto& regs = *storage;
    std::memset(&regs, 0, sizeof(regs));
    PipelineSelection sel{};
    sel.regs = &regs;
    const auto set = [&](auto& field, size_t i) {
        if (i < r.size()) {
            std::memcpy(&field, &r[i], std::min(sizeof(field), sizeof(u32)));
        }
    };
    const auto settings = [&](auto& program) {
        if (r.size() >= 2) {
            std::memcpy(&program.settings, r.data(), std::min(sizeof(program.settings), 8ull));
        }
    };
    switch (hw_stage) {
    case Shader::HwStage::Fragment: {
        settings(regs.ps_program);
        set(regs.z_export_format, 2);
        set(regs.ps_input_ena, 4);
        set(regs.ps_input_addr, 5);
        regs.num_interp = r.size() > 6 ? (r[6] & 0x3f) : 0;
        set(regs.barycentric_control, 7);
        set(regs.depth_shader_control, 8);
        const auto& fs = draw.hw.fs;
        for (u32 i = 0; i < regs.num_interp && i < fs.inputs.size(); ++i) {
            regs.ps_inputs[i].input_offset = fs.inputs[i].param_index;
            regs.ps_inputs[i].use_default = fs.inputs[i].is_default;
            regs.ps_inputs[i].flat_shade = fs.inputs[i].is_flat;
            regs.ps_inputs[i].default_value = fs.inputs[i].default_value;
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; ++i) {
            sel.graphics_key.color_buffers[i] = fs.color_buffers[i];
        }
        sel.motion = fs.motion_vectors;
        break;
    }
    case Shader::HwStage::Vertex:
        settings(regs.vs_program);
        set(regs.vs_output_config, 2);
        set(regs.shader_pos_format, 3);
        set(regs.vs_output_control, 4);
        regs.clipper_control.user_clip_plane_enable = draw.hw.vs.user_clip_plane_mask;
        regs.clipper_control.clip_disable = draw.hw.vs.clip_disable;
        sel.motion = draw.hw.vs.motion_vectors;
        sel.ui_squeeze = draw.hw.vs.ui_squeeze_bits != 0;
        break;
    case Shader::HwStage::Compute:
        settings(regs.cs_program);
        if (r.size() >= 5) {
            regs.cs_program.num_thread_x.full = u16(r[2]);
            regs.cs_program.num_thread_y.full = u16(r[3]);
            regs.cs_program.num_thread_z.full = u16(r[4]);
        }
        sel.cs = &regs.cs_program;
        break;
    default:
        return draw;
    }
    auto info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    // Per draw: the software-stage part, and what the blend and clip state decide.
    info.sw = draw.sw;
    if (hw_stage == Shader::HwStage::Fragment) {
        info.hw.fs.dual_source_blending = draw.hw.fs.dual_source_blending;
        info.hw.fs.clip_distance_emulation = draw.hw.fs.clip_distance_emulation;
        info.hw.fs.num_samples = draw.hw.fs.num_samples;
    } else if (hw_stage == Shader::HwStage::Vertex) {
        info.hw.vs.emulate_depth_negative_one_to_one = draw.hw.vs.emulate_depth_negative_one_to_one;
        info.hw.vs.ui_squeeze_bits = draw.hw.vs.ui_squeeze_bits;
    }
    return info;
}

/// bbport (shader variant list): builds one listed variant from the game files and its line alone: the stage registers
/// give the runtime info (with the line's per-draw state), the resources' descriptors are made from the line's
/// specializations (images by slot for a predicted line), the vertex attributes' as replayed reads. Returns the module
/// (null on failure, `info` then empty), its binary, its Info and the specialization the game would compute for it.
vk::ShaderModule PipelineCache::BuildListedVariant(const ListedVariant& v, std::span<const u32> program_code,
                                                   const std::vector<u32>& registers, std::vector<u32>& spv,
                                                   std::unique_ptr<Shader::Info>& info_out,
                                                   Shader::StageSpecialization& spec_out,
                                                   Shader::Backend::Bindings* binding_end) {
    static const std::vector<Shader::UdRead> no_reads;
    static const std::vector<u32> no_flat;
    static thread_local Shader::Pools list_pools;
    const auto& draw = v.spec.runtime_info;
    // BB_VARIANT_TRACE=<file>: "start"/"done" per variant, flushed, to find one that brings the process down.
    static std::FILE* trace = [] {
        const char* path = std::getenv("BB_VARIANT_TRACE");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    static std::mutex trace_mutex;
    const auto trace_line = [&](const char* what) {
        if (trace) {
            std::scoped_lock lk{trace_mutex};
            std::fprintf(trace, "%s %016llx %u %zu\n", what, (unsigned long long)v.hash, u32(draw.hw_stage),
                         v.perm);
            std::fflush(trace);
        }
    };
    trace_line("start");
    // A line whose stage is not the shader's own (its binary info's type) is refused: translating a shader as another
    // stage can bring the process down.
    {
        std::scoped_lock lk{g_shader_types_mutex};
        const auto it = g_shader_types.find(v.hash);
        const u32 want = draw.hw_stage == Shader::HwStage::Fragment ? 0
                         : draw.hw_stage == Shader::HwStage::Vertex ? 1
                         : draw.hw_stage == Shader::HwStage::Compute ? 4
                                                                     : 99;
        if (it == g_shader_types.end() || it->second != want) {
            trace_line("wrong-stage");
            info_out.reset();
            return {};
        }
    }
    auto runtime_info = RuntimeInfoFromRegisters(draw.hw_stage, draw.sw_stage, registers, draw);
    static const std::array<u32, Shader::ShaderParams::NumShaderUserData> no_user_data{};
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{no_user_data},
        .code = program_code,
        .hash = v.hash,
    };
    const auto device = instance.GetDevice();
    Shader::StageSpecialization spec = v.spec;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        Shader::t_translation_replay = nullptr;
        Shader::t_sharp_override = nullptr;
        info_out.reset();
        return {};
    }
    runtime_fault_recover = &recover;
    // A first translation with given descriptors says what the shader reads and writes, and from where.
    const auto probe_translate = [&](const Shader::StageSpecialization& with) {
        const auto descriptors = MakeSharpOverride(with);
        const Shader::TranslationReplay probe_replay{&no_flat, &no_reads, &v.spec.fetch_shader_data};
        auto probe_info = std::make_unique<Shader::Info>(draw.hw_stage, draw.sw_stage, params);
        auto probe_runtime = runtime_info;
        auto probe_binding = v.spec.start;
        std::vector<u32> probe_spv;
        Shader::t_translation_replay = &probe_replay;
        Shader::t_sharp_override = &descriptors;
        const auto probe_module = TranslateModule(*probe_info, probe_runtime, program_code, v.perm, probe_binding,
                                                  list_pools, probe_spv);
        Shader::t_sharp_override = nullptr;
        Shader::t_translation_replay = nullptr;
        if (probe_module) {
            device.destroyShaderModule(probe_module);
        }
        return probe_info;
    };
    // A predicted line gives its images by descriptor slot: plain images first say where each of the shader's images
    // is read from; ranked by that, they take the slot setups.
    if (v.by_slot) {
        Shader::StageSpecialization plain;
        plain.images.resize(Shader::NUM_IMAGES);
        plain.bitset.set();
        const auto probe_info = probe_translate(plain);
        std::vector<size_t> order(probe_info->images.size());
        for (size_t k = 0; k < order.size(); ++k) {
            order[k] = k;
        }
        std::ranges::sort(order, {}, [&](size_t k) { return probe_info->images[k].sharp_fetch.offsets[0]; });
        spec.images.assign(probe_info->images.size(), {});
        for (size_t rank = 0; rank < order.size() && rank < v.slot_images.size(); ++rank) {
            spec.images[order[rank]] = v.slot_images[rank];
        }
        spec.buffers.clear();
        spec.bitset.reset();
        for (size_t k = 0; k < spec.images.size(); ++k) {
            spec.bitset.set(k);
        }
    }
    // BB_VARIANT_COHERENT=written (lines of caches from before BufferSpecialization::coherent): a compute shader's
    // buffers are taken as coherent (memory type 3) exactly when it writes them.
    static const bool coherent_from_writes = [] {
        const char* env = std::getenv("BB_VARIANT_COHERENT");
        return env && std::string_view{env} == "written";
    }();
    if (coherent_from_writes && draw.hw_stage == Shader::HwStage::Compute && !spec.buffers.empty()) {
        const auto probe_info = probe_translate(spec);
        for (size_t k = 0; k < spec.buffers.size() && k < probe_info->buffers.size(); ++k) {
            spec.buffers[k].coherent = probe_info->buffers[k].is_written;
        }
    }
    const auto overrides = MakeSharpOverride(spec);
    // The vertex attributes' descriptors are read through a user-data table pointer: made from the attribute
    // specializations (number class, swizzle) as the reads of a replay.
    std::vector<Shader::UdRead> attribute_reads;
    if (v.spec.fetch_shader_data) {
        const auto& attributes = v.spec.fetch_shader_data->attributes;
        for (size_t a = 0; a < attributes.size() && a < v.spec.vs_attribs.size(); ++a) {
            const auto& s = v.spec.vs_attribs[a];
            AmdGpu::Buffer buffer{};
            buffer.base_address = 0x100000;
            buffer.num_records = 0x10000;
            buffer.stride = 16;
            buffer.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
            buffer.num_format = u32(s.num_class == AmdGpu::NumberClass::Sint   ? AmdGpu::NumberFormat::Sint
                                    : s.num_class == AmdGpu::NumberClass::Uint ? AmdGpu::NumberFormat::Uint
                                                                                 : AmdGpu::NumberFormat::Float);
            buffer.dst_sel_x = u32(s.dst_select.r);
            buffer.dst_sel_y = u32(s.dst_select.g);
            buffer.dst_sel_z = u32(s.dst_select.b);
            buffer.dst_sel_w = u32(s.dst_select.a);
            Shader::UdRead read{attributes[a].sgpr_base, attributes[a].dword_offset, u32(sizeof(buffer)), {}};
            std::memcpy(read.data.data(), &buffer, sizeof(buffer));
            attribute_reads.push_back(read);
        }
    }
    const Shader::TranslationReplay replay{&no_flat, &attribute_reads, &v.spec.fetch_shader_data};
    auto info = std::make_unique<Shader::Info>(draw.hw_stage, draw.sw_stage, params);
    auto binding = v.spec.start;
    Shader::t_translation_replay = &replay;
    Shader::t_sharp_override = &overrides;
    const auto module = TranslateModule(*info, runtime_info, program_code, v.perm, binding, list_pools, spv);
    if (binding_end) {
        *binding_end = binding;
    }
    // The specialization the game computes for this variant, from the same descriptors.
    info->RefreshFlatBuf();
    spec_out = Shader::StageSpecialization(*info, runtime_info, profile, v.spec.start);
    Shader::t_sharp_override = nullptr;
    Shader::t_translation_replay = nullptr;
    runtime_fault_recover = nullptr;
    trace_line("done");
    info_out = std::move(info);
    return module;
}

/// Listed variants in two rounds: those whose bindings start where a listed pixel shader's end (startps=) are built
/// after the others, with that start (lines whose pixel shader did not build are left out). `build(i, end)` builds
/// listed[i], sets its end bindings and says whether it built.
/// One listed variant built; an exception (the translator throws on what it cannot handle) only drops that variant.
template <typename Fn>
static bool BuildGuarded(Fn& build, size_t i, Shader::Backend::Bindings& end, const ListedVariant& v) {
    try {
        return build(i, end);
    } catch (const std::exception& e) {
        std::printf("Variant list: %016llx (hw %u) not built: %s\n", (unsigned long long)v.hash,
                    u32(v.spec.runtime_info.hw_stage), e.what());
    } catch (...) {
        std::printf("Variant list: %016llx (hw %u) not built: exception\n", (unsigned long long)v.hash,
                    u32(v.spec.runtime_info.hw_stage));
    }
    Shader::t_sharp_override = nullptr;
    Shader::t_translation_replay = nullptr;
    runtime_fault_recover = nullptr;
    return false;
}

template <typename Fn>
static void ForListedVariants(std::vector<ListedVariant>& listed, u32 threads, Fn&& build) {
    std::vector<size_t> first, second;
    for (size_t i = 0; i < listed.size(); ++i) {
        (listed[i].start_from_ps ? second : first).push_back(i);
    }
    std::mutex ends_mutex;
    std::map<std::pair<u64, size_t>, Shader::Backend::Bindings> ends;
    ParallelFor(first.size(), threads, [&](size_t j) {
        const auto& v = listed[first[j]];
        Shader::Backend::Bindings end{};
        if (BuildGuarded(build, first[j], end, v) && v.spec.runtime_info.hw_stage == Shader::HwStage::Fragment) {
            std::scoped_lock lk{ends_mutex};
            ends[{v.hash, v.perm}] = end;
        }
    });
    std::vector<size_t> ready;
    for (const size_t i : second) {
        if (const auto it = ends.find({listed[i].start_ps_hash, listed[i].start_ps_perm}); it != ends.end()) {
            listed[i].spec.start = it->second;
            ready.push_back(i);
        }
    }
    ParallelFor(ready.size(), threads, [&](size_t j) {
        Shader::Backend::Bindings end{};
        BuildGuarded(build, ready[j], end, listed[ready[j]]);
    });
}

void PipelineCache::WarmUpThread() {
    Common::SetCurrentThreadName("bb:PipelineWarm");
    BbThreads::MakeBelowNormal();
    auto& db = Storage::DataBase::Instance();
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };

    if (const char* dump = std::getenv("BB_SPEC_DUMP")) {
        DumpSpecs(db, dump);
    }
    // Check if cache is compatible
    std::vector<u8> profile_data{};
    db.Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    const auto write_profile = [&] {
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        db.Save(Storage::BlobType::ShaderProfile, "profile", std::move(profile_data));
    };
    bool compatible = true;
    if (profile_data.empty()) {
        db.FinishPreload();
        write_profile();
        compatible = false;
    } else {
        if (profile_data.size() != sizeof(Shader::Profile)) {
            LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                        profile_data.size(), sizeof(Shader::Profile));
        }
        Shader::Profile cached_profile{};
        if (profile_data.size() == sizeof(Shader::Profile)) {
            std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        }
        if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
            // bbport: upstream closed the cache for the session here, so it was never rewritten
            // and every later session compiled every shader again (stutters on each new area).
            // Start a fresh cache for this build and GPU instead.
            LOG_WARNING(Render,
                        "Pipeline cache isn't compatible with current system: rebuilding it");
            db.Clear();
            db.FinishPreload();
            write_profile();
            compatible = false;
        }
    }
    if (!compatible) {
        warm_done.store(true, std::memory_order_release);
        warm_done.notify_all();
        return;
    }

    u32 threads = std::clamp<u32>(BbThreads::Available() > 2 ? BbThreads::Available() - 2 : 1, 1, 16);
    if (const char* env = std::getenv("BB_WARM_THREADS")) { // 1 = the old serial load, for timing
        threads = std::clamp<u32>(static_cast<u32>(std::atoi(env)), 1, 64);
    }
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        threads = 1; // the zip reader is not thread-safe
    }
    const auto device = instance.GetDevice();

    // 1. The stored pipeline keys.
    const auto names = db.ListBlobNames(Storage::BlobType::PipelineKey);
    std::vector<StoredPipeline> stored(names.size());
    ParallelFor(stored.size(), threads, [&](size_t i) {
        std::vector<u8> data;
        db.Load(Storage::BlobType::PipelineKey, names[i], data);
        if (data.empty()) {
            return;
        }
        const size_t data_size = data.size();
        Serialization::Archive ar{std::move(data)};
        Serialization::Reader pldata{ar};
        u32 version{};
        pldata.Read(version);
        // bbport (variant list prototype, BB_VARIANT_SHIP): keys of version 6 lack GraphicsPipelineKey::split_sets
        // (4 bytes, and 4 of padding at the end: 8 bytes shorter). A graphics key of another size is skipped.
        constexpr size_t v6_key_size = sizeof(GraphicsPipelineKey) - 8;
        const bool v6 = version == 6 && (std::getenv("BB_VARIANT_SHIP") || std::getenv("BB_KEY_DUMP"));
        if (version != Serialization::PipelineKeyVersion && !v6) {
            // A key of an older cache layout can never load again: drop it.
            std::error_code ec;
            std::filesystem::remove(db.Directory() / (names[i] + ".key"), ec);
            return;
        }
        u32 is_compute{};
        pldata.Read(is_compute);
        auto& entry = stored[i];
        entry.compute = is_compute != 0;
        if (entry.compute) {
            entry.ckey.Deserialize(ar);
            entry.csdata.Deserialize(ar);
        } else if (v6) {
            // The whole record must be a current one less those 8 bytes.
            static const size_t v7_size = [] {
                Serialization::Archive probe;
                Serialization::Writer w{probe};
                w.Write(u32{0});
                w.Write(u32{0});
                GraphicsPipelineKey key{};
                key.Serialize(probe);
                GraphicsPipeline::SerializationSupport support{};
                support.Serialize(probe);
                return probe.TakeOff().size();
            }();
            if (data_size + 8 != v7_size) {
                return;
            }
            std::vector<u8> raw(v6_key_size);
            pldata.Read(raw.data(), raw.size());
            constexpr size_t at = offsetof(GraphicsPipelineKey, split_sets);
            auto* key = reinterpret_cast<u8*>(&entry.gkey);
            std::memcpy(key, raw.data(), at);
            std::memset(key + at, 0, sizeof(u32));
            std::memcpy(key + at + sizeof(u32), raw.data() + at, raw.size() - at);
            std::memset(key + at + sizeof(u32) + (raw.size() - at), 0,
                        sizeof(GraphicsPipelineKey) - (at + sizeof(u32) + (raw.size() - at)));
            entry.gsdata.Deserialize(ar);
        } else {
            entry.gkey.Deserialize(ar);
            entry.gsdata.Deserialize(ar);
        }
        entry.parsed = true;
    });
    const double t_keys = elapsed_ms();
    // bbport (shader variant list, BB_KEY_DUMP=<file>): every stored graphics pipeline key, one line, fields named:
    // what a pipeline adds to its shaders (for predicting pipelines from the game's data).
    if (const char* dump = std::getenv("BB_KEY_DUMP")) {
        if (std::FILE* f = std::fopen(dump, "w")) {
            u32 n = 0;
            for (const auto& entry : stored) {
                if (!entry.parsed || entry.compute) {
                    continue;
                }
                const auto& k = entry.gkey;
                std::string line = "stages=";
                for (const auto h : k.stage_hashes) {
                    line += fmt::format("{:x},", h);
                }
                line += "\tvfmt=";
                for (const auto fmt_ : k.vertex_buffer_formats) {
                    if (fmt_ != vk::Format::eUndefined) {
                        line += fmt::format("{},", u32(fmt_));
                    }
                }
                line += fmt::format("\tncb={}\tcb={}\tblend={}\twmask=", k.num_color_attachments,
                                    HexList(k.color_buffers), HexList(k.blend_controls));
                for (const auto m : k.write_masks) {
                    line += fmt::format("{:x},", u32(m));
                }
                line += fmt::format("\tcbmask={}\tlogic={}\tsamples={}.{}\tcsamples={}\tmrt={:x}\tmv={}\tsq={:x}"
                                    "\tz={}\tstencil={}\tdclamp={}\tprim={}\tpoly={}\tclip={}\tprov={}\tdclip={}\n",
                                    HexBytes(&k.cb_shader_mask, sizeof(k.cb_shader_mask)), u32(k.logic_op),
                                    k.num_samples, k.depth_samples, HexList(k.color_samples), k.mrt_mask,
                                    k.motion_vectors, k.ui_squeeze, u32(k.z_format), u32(k.stencil_format),
                                    u32(k.depth_clamp_enable), u32(k.prim_type), u32(k.polygon_mode),
                                    u32(k.clip_space), u32(k.provoking_vtx_last), u32(k.depth_clip_enable));
                std::fputs(line.c_str(), f);
                ++n;
            }
            std::fclose(f);
            std::printf("Key dump: %u graphics pipeline keys to %s\n", n, dump);
        }
    }

    // 2. The shader permutations those keys name, loaded once each.
    std::vector<u64> stage_hashes;
    for (const auto& entry : stored) {
        if (!entry.parsed) {
            continue;
        }
        if (entry.compute) {
            stage_hashes.push_back(entry.ckey.value);
        } else {
            for (const auto hash : entry.gkey.stage_hashes) {
                if (hash) {
                    stage_hashes.push_back(hash);
                }
            }
        }
    }
    std::ranges::sort(stage_hashes);
    stage_hashes.erase(std::unique(stage_hashes.begin(), stage_hashes.end()), stage_hashes.end());
    std::vector<LoadedStage> stages(stage_hashes.size());
    std::atomic<u32> mismatched_binaries{0};
    static const bool synth_variants = [] {
        const char* env = std::getenv("BB_VARIANT_SYNTH");
        return env && (env[0] == '1' || env[0] == '2');
    }();
    static const bool synth_copy_mtype = [] {
        const char* env = std::getenv("BB_VARIANT_SYNTH");
        return env && env[0] == '2';
    }();
    static const bool replay_variants = [] {
        const char* env = std::getenv("BB_VARIANT_REPLAY");
        return env && env[0] == '1';
    }();
    ParallelFor(stages.size(), threads, [&](size_t i) {
        auto& stage = stages[i];
        std::vector<u8> meta_blob;
        db.Load(Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", stage_hashes[i]),
                meta_blob);
        if (meta_blob.empty()) {
            return;
        }
        Serialization::Archive meta_ar{std::move(meta_blob)};
        stage.program = std::make_unique<Program>();
        stage.spec.info = &stage.program->info;
        std::optional<Shader::Gcn::FetchShaderData> unused;
        // bbport: the scaled movie shader and the clamped bloom shader depend on settings; they
        // are translated each run (and never saved, SaveShaderBinary).
        u64 checksum = 0;
        if (!LoadShaderMeta(meta_ar, stage.program->info, unused, stage.spec, stage.perm_idx,
                            &checksum, &stage.inputs) ||
            Shader::Backend::SPIRV::IsForcedShader(stage.program->info.pgm_hash) ||
            BbMovieShaderScaled(stage.program->info.pgm_hash) ||
            BbNanGuard::BloomClampShader(stage.program->info.pgm_hash) ||
            BbFocusFix::FocusCentreShader(stage.program->info.pgm_hash)) {
            stage.program.reset();
            return;
        }
        stage.parsed = true;
        stage.checksum = checksum;
        // Motion shaders need the session's buffer addresses: without them the slot is only
        // reserved, so a later permutation cannot take its index.
        const auto& info = stage.program->info;
        if (info.hw_stage == Shader::HwStage::Vertex &&
            stage.spec.runtime_info.hw.vs.motion_vectors &&
            Shader::MotionVectors::positions_address == 0) {
            return;
        }
        std::vector<u32> spv{};
        db.Load(Storage::BlobType::ShaderBinary,
                fmt::format("{:#018x}_{}", info.pgm_hash, stage.perm_idx), spv);
        if (spv.empty()) {
            // bbport (shader variant list): translated again from the game's files below.
            if (replay_variants && stage.inputs.present && checksum) {
                stage.checksum = checksum;
                stage.replay = true;
                stage.pgm_hash = info.pgm_hash;
                stage.hw_stage = info.hw_stage;
                stage.sw_stage = info.sw_stage;
                stage.flat = info.flattened_ud_buf;
                if (synth_variants) {
                    stage.stored_info = std::make_unique<Shader::Info>(info);
                }
            }
            return;
        }
        if (BinaryChecksum(spv) != checksum) {
            // Not this meta's binary (or none saved with it): both files go, and the shader is
            // translated again when first used (and saved as a new pair).
            mismatched_binaries.fetch_add(1, std::memory_order_relaxed);
            db.Remove(Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", stage_hashes[i]));
            db.Remove(Storage::BlobType::ShaderBinary,
                      fmt::format("{:#018x}_{}", info.pgm_hash, stage.perm_idx));
            return;
        }
        stage.module = CompileSPV(spv, device);
    });
    // bbport (shader variant list): BB_VARIANT_LIST_OUT=<file> writes every stored variant as a line;
    // BB_VARIANT_LIST=<file> builds each listed variant from the game files and its line alone (code and stage
    // registers from the files, descriptors made from the specialization) and compares the binary with the line's
    // checksum, or with any stored variant of that shader.
    if (const char* out = std::getenv("BB_VARIANT_LIST_OUT")) {
        if (std::FILE* f = std::fopen(out, "w")) {
            u32 n = 0;
            for (const auto& stage : stages) {
                if (stage.parsed) {
                    std::fprintf(f, "%s\n",
                                 VariantLine(stage.program->info.pgm_hash, stage.perm_idx,
                                             stage.checksum, stage.spec)
                                     .c_str());
                    ++n;
                }
            }
            std::fclose(f);
            std::printf("Variant list: %u variants written to %s\n", n, out);
        }
    }
    if (const char* in = std::getenv("BB_VARIANT_LIST")) {
        const double t_list = elapsed_ms();
        std::vector<ListedVariant> listed;
        u32 unparsed = 0;
        if (std::FILE* f = std::fopen(in, "r")) {
            std::string line;
            int c;
            while ((c = std::fgetc(f)) != EOF) {
                if (c != '\n') {
                    line += char(c);
                    continue;
                }
                if (auto v = ParseVariantLine(line)) {
                    listed.push_back(std::move(*v));
                } else if (!line.empty()) {
                    ++unparsed;
                }
                line.clear();
            }
            std::fclose(f);
        }
        std::unordered_map<u64, std::set<u64>> stored_checksums;
        for (const auto& stage : stages) {
            if (stage.parsed && stage.checksum) {
                stored_checksums[stage.program->info.pgm_hash].insert(stage.checksum);
            }
        }
        std::set<u64> wanted;
        for (const auto& v : listed) {
            wanted.insert(v.hash);
        }
        u64 scanned = 0;
        std::unordered_map<u64, std::vector<u32>> registers;
        const auto code = ReadGameShaderCode(wanted, &scanned, &registers);
        std::atomic<u32> same{0}, differ{0}, no_reference{0}, no_code{0}, failed{0};
        std::mutex shown_mutex;
        u32 shown = 0;
        ForListedVariants(listed, threads, [&](size_t i, Shader::Backend::Bindings& end) -> bool {
            const auto& v = listed[i];
            const auto it = code.find(v.hash);
            const auto rit = registers.find(v.hash);
            if (it == code.end() || rit == registers.end()) {
                no_code.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            std::vector<u32> spv;
            std::unique_ptr<Shader::Info> built_info;
            Shader::StageSpecialization built_spec;
            const auto module = BuildListedVariant(v, it->second, rit->second, spv, built_info, built_spec, &end);
            if (!built_info) {
                failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (module) {
                device.destroyShaderModule(module);
            }
            const auto& draw = v.spec.runtime_info;
            const u64 sum = BinaryChecksum(spv);
            // BB_VARIANT_LIST_SUMS=<file>: each built line's binary checksum (offline scoring of predictions).
            static std::FILE* sums = [] {
                const char* path = std::getenv("BB_VARIANT_LIST_SUMS");
                return path ? std::fopen(path, "w") : nullptr;
            }();
            if (sums) {
                std::scoped_lock lk{shown_mutex};
                std::fprintf(sums, "%016llx %u %zu %016llx\n", (unsigned long long)v.hash, u32(v.spec.runtime_info.hw_stage),
                             v.perm, (unsigned long long)sum);
                std::fflush(sums);
            }
            const auto ref = stored_checksums.find(v.hash);
            const bool has_reference = !v.checksums.empty() || ref != stored_checksums.end();
            const bool ok = std::ranges::find(v.checksums, sum) != v.checksums.end() ||
                            (ref != stored_checksums.end() && ref->second.contains(sum));
            if (!has_reference) {
                no_reference.fetch_add(1, std::memory_order_relaxed);
            } else if (ok) {
                same.fetch_add(1, std::memory_order_relaxed);
            } else {
                differ.fetch_add(1, std::memory_order_relaxed);
                std::scoped_lock lk{shown_mutex};
                if (shown++ < 8) {
                    std::printf("Variant list: %016llx (hw %u, perm %zu) differs\n",
                                (unsigned long long)v.hash, u32(draw.hw_stage), v.perm);
                }
            }
            return true;
        });
        std::printf("Variant list: %zu listed (%u unreadable lines); built from the game files: %u identical, %u "
                    "differ, %u without a reference, %u without code, %u faulted (%.0f ms)\n",
                    listed.size(), unparsed, same.load(), differ.load(), no_reference.load(), no_code.load(),
                    failed.load(), elapsed_ms() - t_list);
    }
    // bbport (shader variant list, BB_RUNTIME_CHECK=1): every stored variant's runtime info rebuilt from the stage
    // registers in the game files (plus its per-draw state) and compared with the stored one, byte for byte.
    if (const char* check = std::getenv("BB_RUNTIME_CHECK"); check && check[0] == '1') {
        std::set<u64> wanted;
        for (const auto& stage : stages) {
            if (stage.parsed) {
                wanted.insert(stage.program->info.pgm_hash);
            }
        }
        u64 scanned = 0;
        std::unordered_map<u64, std::vector<u32>> registers;
        ReadGameShaderCode(wanted, &scanned, &registers);
        std::map<std::string, u32> counts;
        u32 shown = 0;
        for (const auto& stage : stages) {
            if (!stage.parsed) {
                continue;
            }
            const auto& info = stage.program->info;
            const char* name = info.hw_stage == Shader::HwStage::Fragment ? "fs"
                               : info.hw_stage == Shader::HwStage::Vertex ? "vs"
                               : info.hw_stage == Shader::HwStage::Compute ? "cs"
                                                                           : "other";
            const auto it = registers.find(info.pgm_hash);
            if (it == registers.end() || it->second.empty()) {
                ++counts[fmt::format("{} no registers", name)];
                continue;
            }
            const auto& stored = stage.spec.runtime_info;
            const auto built = RuntimeInfoFromRegisters(info.hw_stage, info.sw_stage, it->second, stored);
            const bool props = std::memcmp(&built.props, &stored.props, sizeof(stored.props)) == 0;
            const bool hw = std::memcmp(&built.hw, &stored.hw, sizeof(stored.hw)) == 0;
            const bool all = std::memcmp(&built, &stored, sizeof(stored)) == 0;
            ++counts[fmt::format("{} variants", name)];
            counts[fmt::format("{} props same", name)] += props;
            counts[fmt::format("{} hw same", name)] += hw;
            counts[fmt::format("{} whole same", name)] += all;
            if (!all && shown++ < 6) {
                std::printf("Runtime check: %s %016llx differs (props %d, hw %d)\n", name,
                            (unsigned long long)info.pgm_hash, int(props), int(hw));
            }
        }
        std::string line;
        for (const auto& [k, v] : counts) {
            line += fmt::format("{} {}, ", k, v);
        }
        std::printf("Runtime check: %s\n", line.c_str());
    }
    // Everything is read: an archived cache turns writable now (the game saves what it compiles).
    db.FinishPreload();

    const double t_shaders = elapsed_ms();

    // 3. Into the program cache, in order. A permutation lives at its original index.
    std::unordered_map<u64, LoadedStage*> by_hash;
    std::vector<Program*> owners;
    for (size_t i = 0; i < stages.size(); ++i) {
        auto& stage = stages[i];
        if (!stage.parsed) {
            continue;
        }
        by_hash.emplace(stage_hashes[i], &stage);
        const u64 pgm_hash = stage.program->info.pgm_hash;
        const auto [it_pgm, new_program] = program_cache.try_emplace(pgm_hash);
        if (new_program) {
            it_pgm.value() = std::move(stage.program);
            owners.push_back(it_pgm.value().get());
        }
        Program* program = it_pgm.value().get();
        stage.owner = program;
        stage.spec.info = &program->info;
        stage.fetch_shader = stage.spec.fetch_shader_data;
        program->modules.resize(std::max(program->modules.size(), stage.perm_idx + 1));
        if (!stage.module) {
            continue; // slot reserved
        }
        if (!new_program) {
            const auto found =
                std::ranges::find(program->modules, stage.spec, &Program::Module::spec);
            if (found != program->modules.end() && found->module) {
                // The same permutation under another index: a store holding more than one
                // cache generation. Leave this pipeline to compile at runtime.
                device.destroyShaderModule(stage.module);
                stage.module = vk::ShaderModule{};
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {:x} conflicts with index {}, skipping "
                            "preload",
                            stage.perm_idx, pgm_hash,
                            std::distance(program->modules.begin(), found));
                continue;
            }
        }
        program->modules[stage.perm_idx] = {stage.module, stage.spec};
        stage.ready = true;
    }
    const double t_programs = elapsed_ms();

    // 4. The programs are final: selection may start. (A permutation the game needs that is
    // not stored is appended behind the stored ones, as the reserved slots promise.)
    std::array<Shader::RuntimeInfo, MaxShaderStages> no_runtime_infos{};
    ParallelFor(owners.size(), threads, [&](size_t i) {
        owners[i]->info_template = std::make_unique<Shader::Info>(owners[i]->info);
    });
    warm_done.store(true, std::memory_order_release);
    warm_done.notify_all();
    const double t_ready = elapsed_ms();

    // 5. The pipelines themselves, on all threads, while the game already runs: each goes into
    // the map as it is done, unless the game built the same one first. They are built from
    // the programs' template Infos (the game refreshes the programs' own per draw) and then
    // pointed at the programs' Infos.
    std::atomic<u32> num_pipelines{0};
    const auto build_stored = [&](size_t i) {
        auto& entry = stored[i];
        if (!entry.parsed) {
            return;
        }
        if (entry.compute) {
            const auto it = by_hash.find(entry.ckey.value);
            if (it == by_hash.end() || !it->second->ready) {
                return;
            }
            {
                std::shared_lock lock{pipelines_mutex};
                if (compute_pipelines.contains(entry.ckey)) {
                    return;
                }
            }
            auto pipeline = std::make_unique<ComputePipeline>(
                instance, scheduler, desc_heap, profile, *pipeline_cache, entry.ckey,
                *it->second->owner->info_template, it->second->module, entry.csdata, true);
            // bbport: a cached pipeline the driver cannot create (a new driver, or one saved by a
            // build that stopped on it) is left to compile when the game uses it.
            if (!pipeline->IsValid()) {
                return;
            }
            pipeline->RebindStage(&it->second->owner->info);
            std::unique_lock lock{pipelines_mutex};
            num_pipelines += compute_pipelines.try_emplace(entry.ckey, std::move(pipeline)).second;
            return;
        }
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<const Shader::Info*, MaxShaderStages> program_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
        for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
            const auto hash = entry.gkey.stage_hashes[stage];
            if (!hash) {
                continue;
            }
            const auto it = by_hash.find(hash);
            if (it == by_hash.end() || !it->second->ready) {
                return;
            }
            infos[stage] = it->second->owner->info_template.get();
            program_infos[stage] = &it->second->owner->info;
            modules[stage] = it->second->module;
            if (it->second->fetch_shader) {
                fetch_shader = it->second->fetch_shader;
            }
        }
        // The game may need (and build) the same pipeline meanwhile: build it only once.
        const u64 claim_id = std::hash<GraphicsPipelineKey>{}(entry.gkey) ^ PipelineClaimTag;
        AcquireClaim(claim_id);
        {
            std::shared_lock lock{pipelines_mutex};
            if (graphics_pipelines.contains(entry.gkey)) {
                ReleaseClaim(claim_id);
                return;
            }
        }
        auto pipeline = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, entry.gkey, *pipeline_cache, infos,
            no_runtime_infos, fetch_shader, modules, entry.gsdata, true);
        if (!pipeline->IsValid()) {
            ReleaseClaim(claim_id);
            return;
        }
        pipeline->RebindStages(program_infos);
        {
            std::unique_lock lock{pipelines_mutex};
            num_pipelines +=
                graphics_pipelines.try_emplace(entry.gkey, std::move(pipeline)).second;
        }
        ReleaseClaim(claim_id);
    };
    ParallelFor(stored.size(), threads, build_stored);

    // bbport (variant list prototype, BB_VARIANT_SHIP=<lines>): the listed variants the cache does not hold are built
    // from the game files behind the game (BuildListedVariant), put into the program cache (unless the game made the
    // same permutation first) and saved; then the stored pipelines naming them are built.
    std::deque<LoadedStage> shipped;
    if (const char* ship = std::getenv("BB_VARIANT_SHIP")) {
        const double t_ship = elapsed_ms();
        BbOverlay::SetShaderPreparation(1, 0, 0);
        // Behind the game: half the warm-up threads (BB_SHIP_THREADS), so play keeps its share of the CPU.
        u32 ship_threads = std::max<u32>(1, threads / 2);
        if (const char* env = std::getenv("BB_SHIP_THREADS")) {
            ship_threads = std::clamp<u32>(u32(std::atoi(env)), 1, 64);
        }
        std::vector<ListedVariant> listed;
        if (std::FILE* f = std::fopen(ship, "r")) {
            std::string line;
            int c;
            while ((c = std::fgetc(f)) != EOF) {
                if (c != '\n') {
                    line += char(c);
                    continue;
                }
                if (auto v = ParseVariantLine(line)) {
                    if (!by_hash.contains(HashCombine(v->hash, v->perm))) {
                        listed.push_back(std::move(*v));
                    }
                }
                line.clear();
            }
            std::fclose(f);
        }
        std::set<u64> wanted;
        for (const auto& v : listed) {
            wanted.insert(v.hash);
        }
        u64 scanned = 0;
        std::unordered_map<u64, std::vector<u32>> registers;
        const auto code = ReadGameShaderCode(wanted, &scanned, &registers);
        const double t_read = elapsed_ms();
        std::atomic<u32> built{0}, taken{0}, failed{0}, no_code{0}, progress{0};
        std::mutex shipped_mutex;
        BbOverlay::SetShaderPreparation(1, 0, u32(listed.size()));
        std::vector<std::pair<u64, LoadedStage*>> new_stages;
        ForListedVariants(listed, ship_threads, [&](size_t i, Shader::Backend::Bindings& end) -> bool {
            const auto& v = listed[i];
            BbOverlay::SetShaderPreparation(1, progress.fetch_add(1, std::memory_order_relaxed) + 1,
                                            u32(listed.size()));
            const auto it = code.find(v.hash);
            const auto rit = registers.find(v.hash);
            if (it == code.end() || rit == registers.end()) {
                no_code.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            std::vector<u32> spv;
            std::unique_ptr<Shader::Info> info;
            Shader::StageSpecialization spec;
            const auto module = BuildListedVariant(v, it->second, rit->second, spv, info, spec, &end);
            if (!info || !module) {
                failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            built.fetch_add(1, std::memory_order_relaxed);
            Program* program{};
            // The game's own module of this permutation when it was first (its pipelines are still built below).
            vk::ShaderModule existing{};
            Shader::StageSpecialization existing_spec;
            Program* existing_owner{};
            {
                std::unique_lock lk{programs_mutex};
                auto [it_pgm, created] = program_cache.try_emplace(v.hash);
                if (created) {
                    it_pgm.value() = std::make_unique<Program>();
                    it_pgm.value()->info = *info;
                }
                program = it_pgm.value().get();
                if (!program->info_template) {
                    program->info_template = std::make_unique<Shader::Info>(program->info);
                }
                auto& modules = program->modules;
                modules.resize(std::max(modules.size(), v.perm + 1));
                const auto found = std::ranges::find(modules, spec, &Program::Module::spec);
                if (modules[v.perm].module || (found != modules.end() && found->module)) {
                    const auto& theirs = found != modules.end() && found->module ? *found : modules[v.perm];
                    existing = theirs.module;
                    existing_spec = theirs.spec;
                    existing_owner = program;
                    program = nullptr; // the game was first
                } else {
                    spec.info = &program->info;
                    modules[v.perm] = {module, spec};
                }
            }
            if (!program) {
                taken.fetch_add(1, std::memory_order_relaxed);
                device.destroyShaderModule(module);
                // The stored pipelines naming this permutation are built with the game's module.
                if (existing && existing_owner) {
                    std::scoped_lock lk{shipped_mutex};
                    auto& stage = shipped.emplace_back();
                    stage.parsed = true;
                    stage.ready = true;
                    stage.owner = existing_owner;
                    stage.module = existing;
                    stage.spec = existing_spec;
                    stage.perm_idx = v.perm;
                    stage.fetch_shader = existing_spec.fetch_shader_data;
                    new_stages.emplace_back(HashCombine(v.hash, v.perm), &stage);
                }
                return true; // built (its end bindings count)
            }
            // Saved like a shader translated in play: the next launch loads it from the cache.
            const u64 perm_hash = HashCombine(v.hash, v.perm);
            RegisterShaderBinary(std::move(spv), v.hash, v.perm);
            RegisterShaderMeta(*info, spec.fetch_shader_data, spec, perm_hash, v.perm);
            std::scoped_lock lk{shipped_mutex};
            auto& stage = shipped.emplace_back();
            stage.parsed = true;
            stage.ready = true;
            stage.owner = program;
            stage.module = module;
            stage.spec = spec;
            stage.perm_idx = v.perm;
            stage.fetch_shader = spec.fetch_shader_data;
            new_stages.emplace_back(perm_hash, &stage);
            return true;
        });
        for (const auto& [perm_hash, stage] : new_stages) {
            by_hash.emplace(perm_hash, stage);
        }
        const double t_built = elapsed_ms();
        const u32 before = num_pipelines.load();
        BbOverlay::SetShaderPreparation(2, 0, u32(stored.size()));
        std::atomic<u32> pipelines_done{0};
        ParallelFor(stored.size(), ship_threads, [&](size_t i) {
            build_stored(i);
            BbOverlay::SetShaderPreparation(2, pipelines_done.fetch_add(1, std::memory_order_relaxed) + 1,
                                            u32(stored.size()));
        });
        BbOverlay::SetShaderPreparation(0, 0, 0);
        std::printf("Variant ship: %zu listed variants not in the cache; code read in %.0f ms; built %u (%u taken by "
                    "the game first, %u failed, %u without code) in %.0f ms; then %u pipelines in %.0f ms\n",
                    listed.size(), t_read - t_ship, built.load(), taken.load(), failed.load(), no_code.load(),
                    t_built - t_read, num_pipelines.load() - before, elapsed_ms() - t_built);
    }

    // 6. bbport (shader variant list, BB_VARIANT_REPLAY=1): variants with translation inputs but no binary are
    // translated from the code in the player's own game files, with the recorded inputs in place of game memory,
    // and kept only when the result matches the stored checksum bit for bit. The game runs meanwhile (the file
    // scan takes seconds); their pipelines are built after them.
    if (replay_variants) {
        const double t_start = elapsed_ms();
        std::set<u64> wanted;
        u32 to_replay = 0;
        for (const auto& stage : stages) {
            if (stage.replay) {
                wanted.insert(stage.pgm_hash);
                ++to_replay;
            }
        }
        u64 files_scanned = 0;
        const auto code = ReadGameShaderCode(wanted, &files_scanned);
        const double t_scan = elapsed_ms();
        std::atomic<u32> matched{0}, differed{0}, no_code{0}, failed{0}, late{0};
        ParallelFor(stages.size(), threads, [&](size_t i) {
            auto& stage = stages[i];
            if (!stage.replay || !stage.owner) {
                return;
            }
            const auto it = code.find(stage.pgm_hash);
            if (it == code.end()) {
                no_code.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const std::span<const u32> program_code{it->second};
            const Shader::ShaderParams params{
                .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
                    stage.inputs.user_data},
                .code = program_code,
                .hash = stage.pgm_hash,
            };
            Shader::Info info{stage.hw_stage, stage.sw_stage, params};
            const std::vector<u32> synth_flat =
                stage.stored_info
                    ? SynthesizeFlatBuffer(*stage.stored_info, stage.spec, stage.flat.size(),
                                           synth_copy_mtype)
                    : std::vector<u32>{};
            const Shader::TranslationReplay replay{stage.stored_info ? &synth_flat : &stage.flat,
                                                   &stage.inputs.reads,
                                                   &stage.spec.fetch_shader_data};
            auto runtime_info = stage.spec.runtime_info;
            auto binding = stage.spec.start;
            static thread_local Shader::Pools replay_pools;
            std::vector<u32> spv;
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                Shader::t_translation_replay = nullptr;
                failed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            runtime_fault_recover = &recover;
            Shader::t_translation_replay = &replay;
            const auto module = TranslateModule(info, runtime_info, program_code, stage.perm_idx,
                                                binding, replay_pools, spv);
            Shader::t_translation_replay = nullptr;
            runtime_fault_recover = nullptr;
            if (BinaryChecksum(spv) != stage.checksum) {
                if (module) {
                    device.destroyShaderModule(module);
                }
                if (differed.fetch_add(1, std::memory_order_relaxed) < 8) {
                    LOG_WARNING(Render_Vulkan, "Variant replay of {} shader {:#x} ({}) differs",
                                stage.hw_stage, stage.pgm_hash, stage.perm_idx);
                }
                return;
            }
            matched.fetch_add(1, std::memory_order_relaxed);
            // Saved as if translated in play: the next launch loads it like any stored shader.
            RegisterShaderBinary(std::move(spv), stage.pgm_hash, stage.perm_idx);
            // Into the slot reserved for it, unless the game translated the same permutation meanwhile
            // (it appends those behind the stored ones).
            {
                std::unique_lock lk{programs_mutex};
                auto& modules = stage.owner->modules;
                const auto found = std::ranges::find(modules, stage.spec, &Program::Module::spec);
                if (stage.perm_idx < modules.size() && !modules[stage.perm_idx].module &&
                    (found == modules.end() || !found->module)) {
                    modules[stage.perm_idx] = {module, stage.spec};
                    stage.module = module;
                    stage.ready = true;
                    return;
                }
            }
            late.fetch_add(1, std::memory_order_relaxed);
            device.destroyShaderModule(module);
        });
        std::printf("Variant replay: %u variants without a binary; code for %zu of %zu shaders (%llu "
                    "game files scanned, %.0f ms); %u identical, %u differ, %u without code, %u faulted, %u "
                    "translated by the game first (%.0f ms)\n",
                    to_replay, code.size(), wanted.size(), (unsigned long long)files_scanned,
                    t_scan - t_start, matched.load(), differed.load(), no_code.load(), failed.load(),
                    late.load(), elapsed_ms() - t_scan);
        ParallelFor(stored.size(), threads, build_stored);
    }
    const u32 num_total_pipelines = static_cast<u32>(stored.size());
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines.load());
    if (num_total_pipelines > num_pipelines.load()) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines.load());
    }
    std::printf("Pipeline cache: preloaded %u of %u pipelines (%u shaders, %u threads) in %.0f ms "
                "(keys %.0f, shaders %.0f, programs %.0f, pipelines %.0f); the game could select from %.0f ms\n",
                num_pipelines.load(), num_total_pipelines, u32(stages.size()), threads, elapsed_ms(),
                t_keys, t_shaders - t_keys, t_programs - t_shaders, elapsed_ms() - t_programs,
                t_ready);
    // bbport: BB_GPL_STATS=1 (pipeline library study, docs in the local notes): how many stored graphics
    // pipelines share their stages. Pipelines per distinct stage set = how often a library part is reused.
    if (std::getenv("BB_GPL_STATS")) {
        // stage_hashes is indexed by Shader::SwStage: 0 fragment, 1-2 tessellation, 3 vertex, 4 geometry.
        constexpr size_t fragment = static_cast<size_t>(Shader::SwStage::Fragment);
        std::set<std::array<size_t, MaxShaderStages>> stage_sets, pre_raster_sets;
        std::set<size_t> fragment_stages;
        u32 graphics = 0;
        for (const auto& entry : stored) {
            if (!entry.parsed || entry.compute) {
                continue;
            }
            ++graphics;
            stage_sets.insert(entry.gkey.stage_hashes);
            auto pre_raster = entry.gkey.stage_hashes;
            pre_raster[fragment] = 0;
            pre_raster_sets.insert(pre_raster);
            fragment_stages.insert(entry.gkey.stage_hashes[fragment]);
        }
        std::printf("GPL stats: %u graphics pipelines, %zu distinct stage sets (%.1f pipelines each), %zu "
                    "distinct pre-raster sets, %zu distinct fragment stages\n",
                    graphics, stage_sets.size(),
                    stage_sets.empty() ? 0.0 : double(graphics) / double(stage_sets.size()),
                    pre_raster_sets.size(), fragment_stages.size());
    }
    // bbport: BB_GUESS_TEST=1 (first-launch pre-translation study): could each stored permutation have been guessed
    // without running the game? A guess takes, for every draw-time field, the most common value of its kind over
    // all stored permutations (conditioned on what the shader code itself says: a buffer's is_formatted, an
    // image's is_storage). Reported: permutations with no resources (only the runtime info to guess), exact hits
    // of the full guess, and hits if the runtime info were known (resources guessed only). The binding start is
    // left out: with the split layout of pipeline libraries each half starts at 0, so it is known.
    if (std::getenv("BB_GUESS_TEST")) {
        const auto bytes_hash = [](const auto& value) { return XXH3_64bits(&value, sizeof(value)); };
        struct BufferKey {
            u32 stride, is_formatted, swizzle, data_format, num_format, index_stride, element_size;
            AmdGpu::CompMapping dst_select;
            AmdGpu::NumberConversion conversion;
        };
        const auto buffer_key = [&](const Shader::BufferSpecialization& b) {
            BufferKey k{};
            std::memset(&k, 0, sizeof(k));
            k.stride = b.stride;
            k.is_formatted = b.is_formatted;
            k.swizzle = b.swizzle_enable;
            if (b.is_formatted) {
                k.data_format = b.data_format;
                k.num_format = b.num_format;
                k.dst_select = b.dst_select;
                k.conversion = b.num_conversion;
            }
            if (b.swizzle_enable) {
                k.index_stride = b.index_stride;
                k.element_size = b.element_size;
            }
            return bytes_hash(k);
        };
        const auto image_key = [&](const Shader::ImageSpecialization& i) {
            Shader::ImageSpecialization k{};
            std::memset(&k, 0, sizeof(k));
            k = i;
            return bytes_hash(k);
        };
        const auto sampler_key = [](const Shader::SamplerSpecialization& s) {
            return u64(s.force_unnormalized) | u64(s.force_degamma) << 1;
        };
        const auto attrib_key = [&](const Shader::VsAttribSpecialization& a) {
            Shader::VsAttribSpecialization k{};
            std::memset(&k, 0, sizeof(k));
            k = a;
            return bytes_hash(k);
        };
        // Pass 1: the value counts per kind (and per known condition).
        std::unordered_map<u64, std::unordered_map<u64, u32>> counts;
        const auto kind = [](u64 type, u64 condition) { return type << 8 | condition; };
        std::unique_lock lock{programs_mutex};
        for (const auto& [hash, program] : program_cache) {
            const u64 stage = static_cast<u64>(program->info.sw_stage);
            for (const auto& module : program->modules) {
                const auto& spec = module.spec;
                ++counts[kind(1, stage)][bytes_hash(spec.runtime_info)];
                for (const auto& b : spec.buffers) {
                    ++counts[kind(2, b.is_formatted)][buffer_key(b)];
                }
                for (const auto& i : spec.images) {
                    ++counts[kind(3, i.is_storage)][image_key(i)];
                }
                for (const auto& s : spec.samplers) {
                    ++counts[kind(4, 0)][sampler_key(s)];
                }
                for (const auto& a : spec.vs_attribs) {
                    ++counts[kind(5, 0)][attrib_key(a)];
                }
                ++counts[kind(6, 0)][spec.fmasks.size()];
            }
        }
        std::unordered_map<u64, u64> mode;
        for (const auto& [k, values] : counts) {
            u32 best = 0;
            for (const auto& [value, n] : values) {
                if (n > best) {
                    best = n;
                    mode[k] = value;
                }
            }
        }
        // Pass 2: hits per stage.
        struct Tally {
            u32 total = 0, resource_free = 0, free_hit = 0, full_hit = 0, resources_hit = 0;
        };
        std::map<u64, Tally> tally;
        for (const auto& [hash, program] : program_cache) {
            const u64 stage = static_cast<u64>(program->info.sw_stage);
            for (const auto& module : program->modules) {
                const auto& spec = module.spec;
                auto& t = tally[stage];
                ++t.total;
                const bool runtime_hit = bytes_hash(spec.runtime_info) == mode[kind(1, stage)];
                bool resources = spec.fmasks.empty();
                for (const auto& b : spec.buffers) {
                    resources &= buffer_key(b) == mode[kind(2, b.is_formatted)];
                }
                for (const auto& i : spec.images) {
                    resources &= image_key(i) == mode[kind(3, i.is_storage)];
                }
                for (const auto& s : spec.samplers) {
                    resources &= sampler_key(s) == mode[kind(4, 0)];
                }
                for (const auto& a : spec.vs_attribs) {
                    resources &= attrib_key(a) == mode[kind(5, 0)];
                }
                const bool no_resources = spec.buffers.empty() && spec.images.empty() &&
                                          spec.samplers.empty() && spec.vs_attribs.empty() &&
                                          spec.fmasks.empty();
                t.resource_free += no_resources;
                t.free_hit += no_resources && runtime_hit;
                t.full_hit += runtime_hit && resources;
                t.resources_hit += resources;
            }
        }
        static constexpr const char* stage_names[] = {"fragment", "tess control", "tess eval",
                                                      "vertex", "geometry", "compute"};
        for (const auto& [stage, t] : tally) {
            std::printf("Guess test: %-12s %5u permutations: %u with no resources (%u of them guessed), full guess "
                        "%u (%.1f%%), resources only %u (%.1f%%)\n",
                        stage < 6 ? stage_names[stage] : "?", t.total, t.resource_free, t.free_hit,
                        t.full_hit, 100.0 * t.full_hit / std::max(1u, t.total), t.resources_hit,
                        100.0 * t.resources_hit / std::max(1u, t.total));
        }
    }
    if (const u32 n = mismatched_binaries.load()) {
        std::printf("Pipeline cache: %u shader binaries did not match their meta: removed, "
                    "translated again when used\n", n);
    }
    // The driver's cache gets the entries this preload had to compile.
    if (driver_cache_loaded_size != DriverCacheSize()) {
        driver_cache_dirty.fetch_add(1);
    }
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}
} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
