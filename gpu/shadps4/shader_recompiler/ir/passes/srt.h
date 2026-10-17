// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#ifdef _WIN32
#include <intrin.h>
#endif
#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

// bbport: a ReadConst whose offset varies per lane (a constant block's pointer plus a per-pixel
// offset, as addr64 buffer loads compute it) reads a window of the block's first dwords that the
// walker copies to the flat buffer; its flags are the window's start with this bit.
constexpr u32 ReadConstWindowFlag = 1u << 16;
constexpr u32 ReadConstWindowDwords = 256;

// bbport: the walker reads the resource tables in guest memory, where data the GPU wrote (and
// has not copied back) is stale: character constants among them (screen-size triangles for
// single frames in U3). Each table it reads, with the bytes it reads there, goes into the log
// of the calling thread (TEB ArbitraryUserPointer, gs:[0x28]) while one is set.
constexpr u32 SrtReadLogCapacity = 64;
struct SrtReadLog {
    u32 count;
    u32 pad;
    struct Read {
        u64 address;
        u64 bytes;
    } reads[SrtReadLogCapacity];
};
static_assert(offsetof(SrtReadLog, reads) == 8 && sizeof(SrtReadLog::Read) == 16);
/// Called after a logged walk; true when it refreshed guest memory (the walk is repeated).
inline bool (*g_srt_read_check)(const SrtReadLog&) = nullptr;

inline SrtReadLog* SwapSrtReadLog(SrtReadLog* log) {
#ifdef _WIN32
    auto* previous = reinterpret_cast<SrtReadLog*>(__readgsqword(0x28));
    __writegsqword(0x28, reinterpret_cast<unsigned long long>(log));
    return previous;
#else
    return nullptr;
#endif
}

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
