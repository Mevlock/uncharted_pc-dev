// SPDX-License-Identifier: GPL-2.0-or-later
// bbport diagnosis (BB_MOTION_STATIC_LOG=1): which object motion draws have inputs that change from frame to frame.
//
// A vertex shader's positions follow from its buffers: the vertex streams and the constant buffers (camera, the
// object's transform, bone palettes). The survey pairs each motion draw with the same draw of the previous frame (by
// shader, stream sizes, indices and occurrence) and counts, every 600 frames: draws whose streams sit at the same
// addresses and were not written by the GPU; per constant buffer size, how often the content and the address
// repeat (the address also against two frames back: buffers written into two copies that take turns); and the share
// of draws a content key (streams, indices and every constant buffer but the excluded sizes) finds in the frame
// before: the draws a static gate would take.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Vulkan::Motion {

class Survey {
public:
    static bool On() {
        static const bool on = [] {
            const char* env = std::getenv("BB_MOTION_STATIC_LOG");
            return env && env[0] == '1';
        }();
        return on;
    }

    struct Buffer {
        uint32_t size{}, stride{};
        uint64_t address{};
        uint64_t content{}; ///< constant buffers (stride 16 or 0): hash of the bytes (0: not readable)
        bool gpu_written{}; ///< the buffer cache holds GPU writes on its pages
    };
    static constexpr size_t MaxBuffers = 12;
    struct Draw {
        uint64_t shader{}, geometry{}, indices{};
        uint32_t index_count{}, instances{}, first_instance{};
        uint32_t num_buffers{};
        bool images{}; ///< the vertex shader also reads images
        std::array<Buffer, MaxBuffers> buffers{};
    };

    Survey() {
        // BB_MOTION_STATIC_EXCLUDE=864,416: constant buffer sizes left out of the content key (the camera's).
        if (const char* env = std::getenv("BB_MOTION_STATIC_EXCLUDE")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 10);
                if (end == p) {
                    break;
                }
                excluded.push_back(uint32_t(v));
                p = *end ? end + 1 : end;
            }
        }
    }

    void OnDraw(const Draw& d) {
        ++w.draws;
        uint64_t id = Mix(Mix(Mix(d.shader, d.geometry), d.indices),
                          uint64_t(d.index_count) << 32 | d.instances) ^ d.first_instance;
        id = Mix(id, occurrences[id]++);
        // The content key: everything the positions follow from but the excluded constants.
        uint64_t key = Mix(Mix(d.shader, d.indices), uint64_t(d.index_count) << 32 | d.instances) ^ d.first_instance;
        bool gpu = false, unreadable = false;
        for (uint32_t i = 0; i < d.num_buffers; ++i) {
            const Buffer& b = d.buffers[i];
            if (IsStream(b)) {
                key = Mix(key, b.address ^ uint64_t(b.size) << 40);
                gpu |= b.gpu_written;
            } else if (!Excluded(b.size)) {
                key = Mix(key, b.content);
                unreadable |= b.content == 0;
                gpu |= b.gpu_written;
            }
        }
        const bool key_ok = !gpu && !unreadable && !d.images;
        w.gpu_inputs += gpu;
        w.images += d.images;
        if (key_ok) {
            Seen& seen = key_seen[key];
            if (seen.last == frame) {
                ++w.key_twice; // the same content twice in a frame: as the first
            } else {
                seen.streak = seen.last + 1 == frame ? seen.streak + 1 : 0;
                seen.last = frame;
            }
            w.key_static += seen.streak >= 1;
            w.streak8 += seen.streak >= 8;
            w.streak60 += seen.streak >= 60;
        }
        Record& rec = records[id];
        const bool paired = rec.frame + 1 == frame && rec.num_buffers == d.num_buffers;
        if (paired) {
            ++w.paired;
            bool streams_same = true, constants_same = true;
            std::string changed;
            for (uint32_t i = 0; i < d.num_buffers; ++i) {
                const Buffer& b = d.buffers[i];
                const auto& old = rec.buffers[i];
                if (IsStream(b)) {
                    if (old.address != b.address) {
                        streams_same = false;
                    }
                    continue;
                }
                auto& cls = w.classes[b.size];
                ++cls.n;
                const bool same = old.content == b.content && b.content != 0;
                cls.content_same += same;
                cls.address_same += old.address == b.address;
                cls.address_turns += old.address != b.address && old.address2 == b.address;
                cls.gpu += b.gpu_written;
                if (!same) {
                    constants_same = false;
                    changed += (changed.empty() ? "" : "+") + std::to_string(b.size);
                }
            }
            w.streams_same += streams_same;
            w.all_same += streams_same && constants_same && !gpu;
            if (!streams_same) {
                changed += changed.empty() ? "streams" : "+streams";
            }
            ++w.patterns[changed.empty() ? "none" : changed];
        }
        for (uint32_t i = 0; i < d.num_buffers; ++i) {
            rec.buffers[i].address2 = paired ? rec.buffers[i].address : 0;
            rec.buffers[i].address = d.buffers[i].address;
            rec.buffers[i].content = d.buffers[i].content;
        }
        rec.num_buffers = d.num_buffers;
        rec.frame = frame;
        if (layouts.size() < 60 && layouts.insert(d.shader).second) {
            std::string text;
            for (uint32_t i = 0; i < d.num_buffers; ++i) {
                char part[64];
                std::snprintf(part, sizeof(part), " %u/%u@%llx%s", d.buffers[i].size, d.buffers[i].stride,
                              (unsigned long long)d.buffers[i].address, d.buffers[i].gpu_written ? "g" : "");
                text += part;
            }
            std::printf("Motion survey layout: vs %08x indices %u instances %u%s buffers (size/stride@address):%s\n",
                        uint32_t(d.shader), d.index_count, d.instances, d.images ? " images" : "", text.c_str());
        }
    }

    void NextFrame() {
        ++frame;
        occurrences.clear();
        if (frame % 600 != 0) {
            return;
        }
        const double f = 600.0;
        const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
        std::printf("Motion survey (per frame, 600 frames): %.0f motion draws, %.0f paired with the frame before; of "
                    "the paired: streams at the same addresses %.1f%%, every input the same %.1f%%; content key (of "
                    "all draws): static %.1f%% (8+ frames %.1f%%, 60+ frames %.1f%%), GPU-written inputs %.1f%%, "
                    "images %.1f%%, same content twice a frame %.1f%%\n",
                    w.draws / f, w.paired / f, pct(w.streams_same, w.paired), pct(w.all_same, w.paired),
                    pct(w.key_static, w.draws), pct(w.streak8, w.draws), pct(w.streak60, w.draws),
                    pct(w.gpu_inputs, w.draws), pct(w.images, w.draws), pct(w.key_twice, w.draws));
        std::string classes;
        for (const auto& [size, c] : w.classes) {
            char part[200];
            std::snprintf(part, sizeof(part),
                          " | %u: %.0f a frame, content same %.1f%%, address same %.1f%%, takes turns %.1f%%, other "
                          "%.1f%%, GPU %.0f%%",
                          size, c.n / f, pct(c.content_same, c.n), pct(c.address_same, c.n),
                          pct(c.address_turns, c.n), pct(c.n - c.address_same - c.address_turns, c.n),
                          pct(c.gpu, c.n));
            classes += part;
        }
        std::printf("Motion survey constants (by size)%s\n", classes.c_str());
        std::vector<std::pair<uint64_t, std::string>> top;
        for (const auto& [name, n] : w.patterns) {
            top.emplace_back(n, name);
        }
        std::sort(top.rbegin(), top.rend());
        std::string patterns;
        for (size_t i = 0; i < top.size() && i < 14; ++i) {
            char part[200];
            std::snprintf(part, sizeof(part), " | %s: %.1f%%", top[i].second.c_str(), pct(top[i].first, w.paired));
            patterns += part;
        }
        std::printf("Motion survey changed inputs (of the paired draws)%s\n", patterns.c_str());
        w = {};
        // Forget what has not been drawn for a while.
        std::erase_if(records, [this](const auto& r) { return r.second.frame + 600 < frame; });
        std::erase_if(key_seen, [this](const auto& r) { return r.second.last + 600 < frame; });
    }

private:
    static uint64_t Mix(uint64_t h, uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdULL;
        return h ^ (h >> 32);
    }
    static bool IsStream(const Buffer& b) {
        return b.stride != 16 && b.stride != 0;
    }
    bool Excluded(uint32_t size) const {
        return std::find(excluded.begin(), excluded.end(), size) != excluded.end();
    }
    struct Record {
        struct Old {
            uint64_t address{}, address2{}, content{};
        };
        std::array<Old, MaxBuffers> buffers{};
        uint32_t num_buffers{};
        uint64_t frame{};
    };
    struct Seen {
        uint64_t last{};
        uint32_t streak{};
    };
    struct Class {
        uint64_t n{}, content_same{}, address_same{}, address_turns{}, gpu{};
    };
    struct Window {
        uint64_t draws{}, paired{}, streams_same{}, all_same{}, key_static{}, key_twice{}, streak8{}, streak60{};
        uint64_t gpu_inputs{}, images{};
        std::map<uint32_t, Class> classes;
        std::map<std::string, uint64_t> patterns;
    } w;
    uint64_t frame = 2;
    std::vector<uint32_t> excluded;
    std::unordered_map<uint64_t, uint32_t> occurrences;
    std::unordered_map<uint64_t, Record> records;
    std::unordered_map<uint64_t, Seen> key_seen;
    std::unordered_set<uint64_t> layouts;
};

} // namespace Vulkan::Motion
