// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#ifdef _WIN32
#include <intrin.h>
#endif
#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

// bbport: a ReadConst whose offset varies per lane (a constant block's pointer plus a per-pixel
// offset, as addr64 buffer loads compute it) reads a window of the block's first dwords that the
// walker copies to the flat buffer; its flags are the window's start with this bit, and the
// window's dwords - 1 from bit ReadConstWindowSizeShift up (at most ReadConstMaxWindowDwords).
constexpr u32 ReadConstWindowFlag = 1u << 16;
constexpr u32 ReadConstWindowSizeShift = 17;
constexpr u32 ReadConstMaxWindowDwords = 32768;
/// bbport: the window of a read whose offset range is not known, in graphics stages (and where
/// the flat buffer has no room for more).
constexpr u32 ReadConstSmallWindowDwords = 256;

/// bbport: a window is as long as the offsets of its reads can reach, where their arithmetic
/// bounds them (a mask, a bit field, a minimum: flatten_extended_userdata_pass.cpp,
/// OffsetUpperBound): no read is clamped then. Before, every window had the fixed sizes below
/// and a read past it got the window's last dword: the games' floating dust and snow (vertex
/// shader 6d41b681, all three games: each mote follows one of 32 path segments, 515 dwords of
/// its block; with 256 dwords the motes on segments 11 to 31 all read one constant: dust stood
/// still in clumps, snow fell in straight lines). The fixed sizes remain for offsets without a
/// bound (one log line per such shader when it is translated).
/// BB_READCONST_RANGE=0: fixed sizes for all, as before (another shader cache version).
inline bool ReadConstRange() {
    static const bool on = [] {
        const char* env = std::getenv("BB_READCONST_RANGE");
        return !(env && env[0] == '0');
    }();
    return on;
}

/// bbport: dwords of a ReadConst window whose reads have no known range, in compute shaders.
/// 256 held ~4 lights of U2's tiled lighting compute shader (56 dwords each, up to 128 lights): the others read the window's last dword, passed
/// culling everywhere and lit every pixel (too bright, and ~26 ms a frame at a lamp-heavy view
/// instead of ~2). 8192 dwords (32 KiB) hold them all; few shaders use windows (7 binds a frame
/// in U2). BB_READCONST_WINDOW_DWORDS=N sets it (256: as before); the shader cache is rebuilt
/// when it changes (ShaderBinaryVersion).
inline u32 ReadConstWindowDwords() {
    static const u32 dwords = [] {
        const char* env = std::getenv("BB_READCONST_WINDOW_DWORDS");
        const u32 n = env ? u32(std::strtoul(env, nullptr, 10)) : 8192u;
        return n < ReadConstSmallWindowDwords ? ReadConstSmallWindowDwords
                                              : std::min<u32>(n, 32768u);
    }();
    return dwords;
}

/// bbport diagnosis (BB_FRAME_STATS): resource table walks and the dwords they filled (each
/// shader stage's flat buffer, windows included), for the "Constant tables" line.
inline const bool g_flatbuf_stats = std::getenv("BB_FRAME_STATS") != nullptr;
inline std::atomic<u64> g_flatbuf_walks{0}, g_flatbuf_dwords{0};

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
/// bbport (U3 chapter 14 red deck trim, 2026-11-13): called before a logged walk, so g_srt_read_check can tell writes
/// that completed during the walk (after its reads) from ones done before it.
inline void (*g_srt_walk_begin)() = nullptr;
/// bbport: called before a descriptor is read through a user data table pointer outside the SRT walker
/// (Info::ReadUdRegDirect: vertex attribute V#s and other GetSharp reads), with the guest address and size.
inline void (*g_ud_table_read)(u64 address, u64 size) = nullptr;

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

    /// bbport: the walker's size, the buffer size and the walker's code; not the walker's address in
    /// this session (Deserialize registers the code anew).
    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
    /// Metas before version 17: the struct as raw memory (its pointer and padding included).
    bool DeserializeRaw(Serialization::Archive& ar);
};

} // namespace Shader
