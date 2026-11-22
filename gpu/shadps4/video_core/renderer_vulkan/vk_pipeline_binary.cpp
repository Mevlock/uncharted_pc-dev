// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <vector>
#include <miniz.h>

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_binary.h"

namespace Vulkan {
/// Held shared while a pipeline is created, alone while the driver's cache data is read (vk_pipeline_cache.cpp).
std::shared_mutex& PipelineCacheUse();
} // namespace Vulkan

namespace Vulkan::PipelineBinaries {
namespace {

using Storage::BlobType;

// The blob of one pipeline: "BBPB", format version, binary count, then per binary its key (size, bytes), flags
// (bit 0: the data is zlib-compressed), the data's size and its stored size, the stored bytes.
constexpr u32 BlobMagic = 0x42504242; // "BBPB"
constexpr u32 BlobVersion = 1;
constexpr u32 FlagCompressed = 1;
// The tag: "BBPBTAG1", vendor, device, driver version, the pipeline cache UUID, the global key (size, 32 bytes), then
// the test salt (BB_PIPELINE_BINARY_TAG_SALT, for a driver-change check without changing the driver).
constexpr char TagMagic[8] = {'B', 'B', 'P', 'B', 'T', 'A', 'G', '1'};
constexpr const char* TagName = "tag";

bool active = false;      ///< Setup found the extension on and the cache open
bool load_stored = false; ///< the stored binaries are of this driver (the tag matched)
bool can_save = false;    ///< this game writes the pack (else: the plain path for anything not stored)
u64 stored_at_start = 0;

std::atomic<u64> used{0}, captured{0}, captured_bytes{0}, captured_raw_bytes{0}, refused{0}, capture_failed{0},
    no_key{0}, cache_misses{0};

std::string Hex(const u8* bytes, u32 size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(size_t(size) * 2, '0');
    for (u32 i = 0; i < size; ++i) {
        text[2 * i] = digits[bytes[i] >> 4];
        text[2 * i + 1] = digits[bytes[i] & 15];
    }
    return text;
}

template <typename T>
void Put(std::vector<u8>& out, const T& value) {
    const auto* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool Take(const std::vector<u8>& in, size_t& at, T& value) {
    if (at > in.size() || in.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

/// The binaries of one blob, ready for vkCreatePipelineBinariesKHR (data points into the blob or into `owned`).
struct Parsed {
    std::vector<VkPipelineBinaryKeyKHR> keys;
    std::vector<VkPipelineBinaryDataKHR> data;
    std::vector<std::vector<u8>> owned;
};

bool Parse(std::vector<u8>& blob, Parsed& out) {
    size_t at = 0;
    u32 magic = 0, version = 0, count = 0;
    if (!Take(blob, at, magic) || !Take(blob, at, version) || !Take(blob, at, count) || magic != BlobMagic ||
        version != BlobVersion || count == 0 || count > 64) {
        return false;
    }
    out.keys.resize(count);
    out.data.resize(count);
    out.owned.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u32 key_size = 0, flags = 0;
        u64 raw_size = 0, stored_size = 0;
        if (!Take(blob, at, key_size) || key_size == 0 || key_size > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR ||
            blob.size() - at < key_size) {
            return false;
        }
        auto& key = out.keys[i];
        key = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        key.keySize = key_size;
        std::memcpy(key.key, blob.data() + at, key_size);
        at += key_size;
        if (!Take(blob, at, flags) || !Take(blob, at, raw_size) || !Take(blob, at, stored_size) || raw_size == 0 ||
            stored_size == 0 || blob.size() - at < stored_size || raw_size > (u64(256) << 20)) {
            return false;
        }
        u8* stored = blob.data() + at;
        at += size_t(stored_size);
        if (flags & FlagCompressed) {
            auto& raw = out.owned.emplace_back(size_t(raw_size));
            mz_ulong length = mz_ulong(raw_size);
            if (mz_uncompress(raw.data(), &length, stored, mz_ulong(stored_size)) != MZ_OK || length != raw_size) {
                return false;
            }
            out.data[i] = {size_t(raw_size), raw.data()};
        } else {
            if (stored_size != raw_size) {
                return false;
            }
            out.data[i] = {size_t(raw_size), stored};
        }
    }
    return at == blob.size();
}

/// One binary appended to a blob (compressed when that saves an eighth or more).
void Append(std::vector<u8>& blob, const VkPipelineBinaryKeyKHR& key, const std::vector<u8>& raw) {
    Put(blob, u32(key.keySize));
    blob.insert(blob.end(), key.key, key.key + key.keySize);
    std::vector<u8> packed(mz_compressBound(mz_ulong(raw.size())));
    mz_ulong packed_size = mz_ulong(packed.size());
    const bool compress = mz_compress2(packed.data(), &packed_size, raw.data(), mz_ulong(raw.size()), 1) == MZ_OK &&
                          packed_size <= raw.size() - raw.size() / 8;
    Put(blob, u32(compress ? FlagCompressed : 0));
    Put(blob, u64(raw.size()));
    if (compress) {
        Put(blob, u64(packed_size));
        blob.insert(blob.end(), packed.data(), packed.data() + packed_size);
    } else {
        Put(blob, u64(raw.size()));
        blob.insert(blob.end(), raw.begin(), raw.end());
    }
}

template <typename Info>
Created CreateIn(vk::Device device, vk::PipelineCache cache, const Info& info) {
    std::shared_lock use{PipelineCacheUse()};
    if constexpr (std::is_same_v<Info, vk::GraphicsPipelineCreateInfo>) {
        auto [result, pipeline] = device.createGraphicsPipelineUnique(cache, info);
        return {result, std::move(pipeline)};
    } else {
        auto [result, pipeline] = device.createComputePipelineUnique(cache, info);
        return {result, std::move(pipeline)};
    }
}

/// As before: in the VkPipelineCache. The creation feedback tells whether the driver found it there (a miss changes
/// the driver's cache data, which is then saved: vk_pipeline_serialization.cpp, the end of the preload).
template <typename Info>
Created CreatePlain(vk::Device device, vk::PipelineCache cache, const Info& info) {
    VkPipelineCreationFeedback feedback{};
    const VkPipelineCreationFeedbackCreateInfo feedback_info{
        VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, info.pNext, &feedback, 0, nullptr};
    Info with = info;
    with.pNext = &feedback_info;
    auto created = CreateIn(device, cache, with);
    if (created.result == vk::Result::eSuccess && cache &&
        (!(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ||
         !(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))) {
        cache_misses.fetch_add(1, std::memory_order_relaxed);
    }
    return created;
}

/// The pipeline from its stored binaries; an empty pipeline when the driver refuses them.
template <typename Info>
Created FromBinaries(vk::Device device, const Info& info, std::vector<u8>& blob) {
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    Parsed parsed;
    if (!Parse(blob, parsed)) {
        return {vk::Result::eErrorFormatNotSupported, {}};
    }
    const u32 count = u32(parsed.keys.size());
    const VkPipelineBinaryKeysAndDataKHR keys_and_data{count, parsed.keys.data(), parsed.data.data()};
    const VkPipelineBinaryCreateInfoKHR create_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, nullptr,
                                                    &keys_and_data, VK_NULL_HANDLE, nullptr};
    std::vector<VkPipelineBinaryKHR> binaries(count, VK_NULL_HANDLE);
    VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, nullptr, count,
                                           binaries.data()};
    const VkResult result = d.vkCreatePipelineBinariesKHR(VkDevice(device), &create_info, nullptr, &handles);
    Created created{vk::Result(result), {}};
    if (result == VK_SUCCESS && handles.pipelineBinaryCount == count) {
        const VkPipelineBinaryInfoKHR binary_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, info.pNext, count,
                                                  binaries.data()};
        Info from = info;
        from.pNext = &binary_info;
        created = CreateIn(device, VK_NULL_HANDLE, from); // binaries: no pipeline cache (VUID-...-pNext-09616)
    } else if (result == VK_SUCCESS) {
        created.result = vk::Result::eIncomplete;
    }
    for (const auto binary : binaries) {
        if (binary != VK_NULL_HANDLE) {
            d.vkDestroyPipelineBinaryKHR(VkDevice(device), binary, nullptr);
        }
    }
    return created;
}

/// The binaries of a pipeline created with the capture flag, saved under `name`; the captured data is released.
void Capture(vk::Device device, VkPipeline pipeline, const std::string& name) {
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const VkPipelineBinaryCreateInfoKHR create_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, nullptr,
                                                    nullptr, pipeline, nullptr};
    VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, nullptr, 0, nullptr};
    VkResult result = d.vkCreatePipelineBinariesKHR(VkDevice(device), &create_info, nullptr, &handles);
    std::vector<VkPipelineBinaryKHR> binaries;
    if (result == VK_SUCCESS && handles.pipelineBinaryCount > 0 && handles.pipelineBinaryCount <= 64) {
        binaries.assign(handles.pipelineBinaryCount, VK_NULL_HANDLE);
        handles.pPipelineBinaries = binaries.data();
        result = d.vkCreatePipelineBinariesKHR(VkDevice(device), &create_info, nullptr, &handles);
    }
    bool ok = result == VK_SUCCESS && !binaries.empty() && handles.pipelineBinaryCount == binaries.size();
    std::vector<u8> blob;
    u64 raw_bytes = 0;
    if (ok) {
        Put(blob, BlobMagic);
        Put(blob, BlobVersion);
        Put(blob, u32(binaries.size()));
    }
    std::vector<u8> raw;
    for (size_t i = 0; ok && i < binaries.size(); ++i) {
        const VkPipelineBinaryDataInfoKHR data_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, nullptr,
                                                    binaries[i]};
        VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        size_t size = 0;
        ok = d.vkGetPipelineBinaryDataKHR(VkDevice(device), &data_info, &key, &size, nullptr) == VK_SUCCESS &&
             size > 0 && size <= (size_t(256) << 20);
        if (ok) {
            raw.resize(size);
            ok = d.vkGetPipelineBinaryDataKHR(VkDevice(device), &data_info, &key, &size, raw.data()) == VK_SUCCESS &&
                 size == raw.size() && key.keySize > 0 && key.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
        }
        if (ok) {
            Append(blob, key, raw);
            raw_bytes += raw.size();
        }
    }
    const VkReleaseCapturedPipelineDataInfoKHR release{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR,
                                                       nullptr, pipeline};
    d.vkReleaseCapturedPipelineDataKHR(VkDevice(device), &release, nullptr);
    for (const auto binary : binaries) {
        if (binary != VK_NULL_HANDLE) {
            d.vkDestroyPipelineBinaryKHR(VkDevice(device), binary, nullptr);
        }
    }
    if (!ok) {
        if (capture_failed.fetch_add(1, std::memory_order_relaxed) < 3) {
            std::printf("Pipeline binaries: the driver gave no binaries for pipeline %s (%s): compiled again at the "
                        "next start\n",
                        name.c_str(), vk::to_string(vk::Result(result)).c_str());
        }
        return;
    }
    const u64 bytes = blob.size();
    if (Storage::DataBase::Instance().Save(BlobType::PipelineBinary, name, std::move(blob))) {
        captured.fetch_add(1, std::memory_order_relaxed);
        captured_bytes.fetch_add(bytes, std::memory_order_relaxed);
        captured_raw_bytes.fetch_add(raw_bytes, std::memory_order_relaxed);
    }
}

template <typename Info>
Created Create(const Instance& instance, vk::PipelineCache cache, const Info& info) {
    const auto device = instance.GetDevice();
    if (!active) {
        return CreatePlain(device, cache, info);
    }
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    // The create info as it is compiled: with the capture flag (VkPipelineCreateFlags2 replaces `flags`).
    const VkPipelineCreateFlags2CreateInfoKHR flags2{
        VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, info.pNext,
        VkPipelineCreateFlags2KHR(static_cast<VkPipelineCreateFlags>(info.flags)) |
            VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    Info capture = info;
    capture.pNext = &flags2;
    const VkPipelineCreateInfoKHR key_info{VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, &capture};
    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (d.vkGetPipelineKeyKHR(VkDevice(device), &key_info, &key) != VK_SUCCESS || key.keySize == 0 ||
        key.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
        no_key.fetch_add(1, std::memory_order_relaxed);
        return CreatePlain(device, cache, info);
    }
    const std::string name = Hex(key.key, key.keySize);
    auto& db = Storage::DataBase::Instance();
    if (load_stored) {
        std::vector<u8> blob;
        db.Load(BlobType::PipelineBinary, name, blob);
        if (!blob.empty()) {
            auto created = FromBinaries(device, info, blob);
            if (created.result == vk::Result::eSuccess && created.value) {
                used.fetch_add(1, std::memory_order_relaxed);
                return created;
            }
            // Refused (another driver build behind the same tag, damaged data): removed, compiled and captured again.
            if (refused.fetch_add(1, std::memory_order_relaxed) < 3) {
                std::printf("Pipeline binaries: the driver refused the stored binaries of pipeline %s (%s): removed, "
                            "compiled again\n",
                            name.c_str(), vk::to_string(created.result).c_str());
            }
            if (can_save) {
                db.Remove(BlobType::PipelineBinary, name);
            }
        }
    }
    if (!can_save) {
        return CreatePlain(device, cache, info); // nothing could be saved: as before
    }
    // Compiled with the capture flag: no pipeline cache (VUID-...-pNext-09617).
    auto created = CreateIn(device, VK_NULL_HANDLE, capture);
    if (created.result != vk::Result::eSuccess || !created.value) {
        // The driver did not build it this way: the VkPipelineCache, as before (the caller's own retry follows).
        if (capture_failed.fetch_add(1, std::memory_order_relaxed) < 3) {
            std::printf("Pipeline binaries: pipeline %s was refused with the capture flag (%s): built as before\n",
                        name.c_str(), vk::to_string(created.result).c_str());
        }
        return CreatePlain(device, cache, info);
    }
    Capture(device, VkPipeline(*created.value), name);
    return created;
}

} // namespace

void Setup(const Instance& instance) {
    active = false;
    load_stored = false;
    auto& db = Storage::DataBase::Instance();
    if (!instance.IsPipelineBinaryEnabled()) {
        return;
    }
    if (!db.IsOpened()) {
        std::printf("Pipeline binaries: off (the shader cache is off)\n");
        return;
    }
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (!d.vkGetPipelineKeyKHR || !d.vkCreatePipelineBinariesKHR || !d.vkGetPipelineBinaryDataKHR ||
        !d.vkReleaseCapturedPipelineDataKHR || !d.vkDestroyPipelineBinaryKHR) {
        std::printf("Pipeline binaries: off (the driver's functions are missing)\n");
        return;
    }
    const auto device = instance.GetDevice();
    VkPipelineBinaryKeyKHR global{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (const VkResult result = d.vkGetPipelineKeyKHR(VkDevice(device), nullptr, &global);
        result != VK_SUCCESS || global.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
        std::printf("Pipeline binaries: off (the driver gave no global key: %s)\n",
                    vk::to_string(vk::Result(result)).c_str());
        return;
    }
    const auto& props = instance.GetPhysicalDevice().getProperties();
    std::vector<u8> tag(TagMagic, TagMagic + sizeof(TagMagic));
    Put(tag, u32(props.vendorID));
    Put(tag, u32(props.deviceID));
    Put(tag, u32(props.driverVersion));
    tag.insert(tag.end(), props.pipelineCacheUUID.begin(), props.pipelineCacheUUID.end());
    Put(tag, u32(global.keySize));
    tag.insert(tag.end(), global.key, global.key + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
    if (const char* salt = std::getenv("BB_PIPELINE_BINARY_TAG_SALT")) { // test only: a driver change
        tag.insert(tag.end(), salt, salt + std::strlen(salt));
        std::printf("Test: BB_PIPELINE_BINARY_TAG_SALT=%s\n", salt);
    }
    can_save = db.CanSave();
    std::vector<u8> stored_tag;
    db.Load(BlobType::PipelineBinary, TagName, stored_tag);
    auto names = db.ListBlobNames(BlobType::PipelineBinary);
    std::erase(names, std::string{TagName});
    if (stored_tag == tag) {
        load_stored = true;
        stored_at_start = names.size();
        std::printf("Pipeline binaries: %zu stored for this driver\n", names.size());
    } else {
        if (!names.empty()) {
            u32 old_version = 0;
            if (stored_tag.size() >= 20 && std::memcmp(stored_tag.data(), TagMagic, sizeof(TagMagic)) == 0) {
                std::memcpy(&old_version, stored_tag.data() + 16, sizeof(old_version));
            }
            std::printf("Pipeline binaries: the driver changed (driver version %#x, was %#x, or another graphics "
                        "card): %zu stored binaries %s, built again\n",
                        props.driverVersion, old_version, names.size(),
                        can_save ? "dropped" : "not used (this game only reads the cache)");
            if (can_save) {
                for (const auto& name : names) {
                    db.Remove(BlobType::PipelineBinary, name);
                }
            }
        } else {
            std::printf("Pipeline binaries: none stored yet: the pipelines this game builds are kept\n");
        }
        if (can_save) {
            db.Save(BlobType::PipelineBinary, TagName, std::move(tag));
        }
    }
    active = true;
}

bool Active() {
    return active;
}

Created CreateGraphics(const Instance& instance, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info) {
    return Create(instance, cache, info);
}

Created CreateCompute(const Instance& instance, vk::PipelineCache cache, const vk::ComputePipelineCreateInfo& info) {
    return Create(instance, cache, info);
}

u64 DriverCacheMisses() {
    return cache_misses.load(std::memory_order_relaxed);
}

void PrintStats(const char* when) {
    if (!active) {
        return;
    }
    std::printf("Pipeline binaries (%s): %llu used, %llu captured (%.1f MB, %.1f MB before compression), %llu refused "
                "by the driver, %llu capture failed, %llu without a key; %llu were stored at the start\n",
                when, static_cast<unsigned long long>(used.load()), static_cast<unsigned long long>(captured.load()),
                double(captured_bytes.load()) / (1024.0 * 1024.0),
                double(captured_raw_bytes.load()) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(refused.load()),
                static_cast<unsigned long long>(capture_failed.load()), static_cast<unsigned long long>(no_key.load()),
                static_cast<unsigned long long>(stored_at_start));
    std::fflush(stdout);
}

} // namespace Vulkan::PipelineBinaries
