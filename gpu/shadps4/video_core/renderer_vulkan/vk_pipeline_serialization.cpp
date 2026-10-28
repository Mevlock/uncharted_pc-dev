// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <map>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <xxhash.h>
#include "bbport_threads.h"
#include "bbport_toggles.h"
#include "common/serdes.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/cta_split.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "bbport_focus_fix.h"
#include "bbport_nan_guard.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 21u; // bbport: shadPS4 #5447, #5427, #5364, #5365 (CFG fallthrough), #5429 (LDS barriers); page table watch bit
static constexpr u32 ShaderMetaVersion = 14u; // bbport: translation inputs (shader variant list)
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
};

/// bbport (shader variant list): the code of the given shaders, read from the game's own files (BB_APP0_DIR). The
/// shaders sit there uncompressed, each with its OrbShdr binary info block (shader_hash = the port's program hash,
/// length = code bytes) behind its code, or reached from a jump at its start (AmdGpu::SearchBinaryInfo).
static std::unordered_map<u64, std::vector<u32>> ReadGameShaderCode(const std::set<u64>& wanted,
                                                                    u64* files_scanned) {
    std::unordered_map<u64, std::vector<u32>> found;
    const char* app0 = std::getenv("BB_APP0_DIR");
    if (!app0 || wanted.empty()) {
        return found;
    }
    static constexpr char sig[] = "OrbShdr";
    static constexpr size_t chunk = 64ull << 20;
    std::vector<char> buf(chunk + 64);
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(app0, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (found.size() == wanted.size()) {
            break;
        }
        if (!it->is_regular_file(ec) || it->file_size(ec) < 64) {
            continue;
        }
        std::FILE* f = _wfopen(it->path().c_str(), L"rb");
        if (!f) {
            continue;
        }
        ++*files_scanned;
        // Info blocks of the wanted shaders in this file.
        struct Hit {
            u64 at, hash;
            u32 length;
        };
        std::vector<Hit> hits;
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
                if (std::memcmp(p, sig, 7) != 0) {
                    continue;
                }
                u32 bits;
                u64 hash;
                std::memcpy(&bits, p + 8, 4);
                std::memcpy(&hash, p + 16, 8);
                const u32 length = bits >> 8;
                if (wanted.contains(hash) && !found.contains(hash) && length >= 8 &&
                    length < (1u << 22)) {
                    hits.push_back({base + u64(p - buf.data()), hash, length});
                }
            }
            if (!got) {
                break;
            }
            carry = have - scan_end;
            std::memmove(buf.data(), buf.data() + scan_end, carry);
            base += scan_end;
        }
        for (const auto& hit : hits) {
            if (found.contains(hit.hash)) {
                continue;
            }
            // The code starts with s_mov_b32 vcc_hi, n where the info block is (n + 1) * 2 dwords on, or ends at the
            // info block.
            const u64 window = std::min<u64>(hit.at, u64(hit.length) + 0x10000);
            std::vector<u8> bytes(window + 28);
            if (_fseeki64(f, s64(hit.at - window), SEEK_SET) != 0 ||
                std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
                continue;
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
            if (start < 0 && hit.length <= window) {
                start = s64(window - hit.length);
            }
            if (start < 0 || u64(start) + hit.length > bytes.size()) {
                continue;
            }
            std::vector<u32> code(hit.length / 4);
            std::memcpy(code.data(), bytes.data() + start, code.size() * 4);
            found.emplace(hit.hash, std::move(code));
        }
        std::fclose(f);
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

void PipelineCache::WarmUpThread() {
    Common::SetCurrentThreadName("bb:PipelineWarm");
    BbThreads::MakeBelowNormal();
    auto& db = Storage::DataBase::Instance();
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };

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
        Serialization::Archive ar{std::move(data)};
        Serialization::Reader pldata{ar};
        u32 version{};
        pldata.Read(version);
        if (version != Serialization::PipelineKeyVersion) {
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
        } else {
            entry.gkey.Deserialize(ar);
            entry.gsdata.Deserialize(ar);
        }
        entry.parsed = true;
    });
    const double t_keys = elapsed_ms();

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
    // Everything is read: an archived cache turns writable now (the game saves what it compiles).
    db.FinishPreload();

    // bbport (shader variant list, BB_VARIANT_REPLAY=1): variants with translation inputs but no binary are
    // translated from the code in the player's own game files, with the recorded inputs in place of game memory,
    // and kept only when the result matches the stored checksum bit for bit.
    if (replay_variants) {
        const double t_start = elapsed_ms();
        std::set<u64> wanted;
        u32 to_replay = 0;
        for (const auto& stage : stages) {
            if (stage.replay) {
                wanted.insert(stage.program->info.pgm_hash);
                ++to_replay;
            }
        }
        u64 files_scanned = 0;
        const auto code = ReadGameShaderCode(wanted, &files_scanned);
        const double t_scan = elapsed_ms();
        std::atomic<u32> matched{0}, differed{0}, no_code{0}, failed{0};
        ParallelFor(stages.size(), threads, [&](size_t i) {
            auto& stage = stages[i];
            if (!stage.replay) {
                return;
            }
            const auto& stored = stage.program->info;
            const auto it = code.find(stored.pgm_hash);
            if (it == code.end()) {
                no_code.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const std::span<const u32> program_code{it->second};
            const Shader::ShaderParams params{
                .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
                    stage.inputs.user_data},
                .code = program_code,
                .hash = stored.pgm_hash,
            };
            Shader::Info info{stored.hw_stage, stored.sw_stage, params};
            const Shader::TranslationReplay replay{&stored.flattened_ud_buf, &stage.inputs.reads,
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
                                stored.hw_stage, stored.pgm_hash, stage.perm_idx);
                }
                return;
            }
            matched.fetch_add(1, std::memory_order_relaxed);
            stage.module = module;
            // Saved as if translated in play: the next launch loads it like any stored shader.
            RegisterShaderBinary(std::move(spv), stored.pgm_hash, stage.perm_idx);
        });
        std::printf(
                    "Variant replay: %u variants without a binary; code for %zu of %zu "
                    "shaders from %llu game files (%.0f ms); %u identical, %u differ, %u without "
                    "code, %u faulted (%.0f ms)\n",
                    to_replay, code.size(), wanted.size(), (unsigned long long)files_scanned,
                     t_scan - t_start, matched.load(), differed.load(), no_code.load(),
                     failed.load(), elapsed_ms() - t_scan);
    }
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
    ParallelFor(stored.size(), threads, [&](size_t i) {
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
    });
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
