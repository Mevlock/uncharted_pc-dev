// SPDX-FileCopyrightText: Copyright DXVK Project
// SPDX-License-Identifier: Zlib

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"

namespace Vulkan {

namespace {
// BB_BARRIER_STATS=1 (diagnosis): how the block index is used, every 5 s.
struct BlockStats {
    u64 inserts, insert_blocks, insert_scanned, contained, finds, find_blocks, find_scanned,
        large_scanned, all_scans, clears, items_at_clear, large_at_clear, heads_at_clear;
};
BlockStats g_stats{};
const bool g_stats_on = [] {
    const char* env = std::getenv("BB_BARRIER_STATS");
    return env && env[0] == '1';
}();
void ReportStats() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    last = now;
    auto& s = g_stats;
    const auto per = [](u64 a, u64 b) { return b ? double(a) / double(b) : 0.0; };
    std::printf("Barrier stats (5 s): %llu inserts (%.2f blocks, %.2f items scanned, %.0f%% "
                "contained), %llu finds (%.2f blocks, %.2f items scanned, %.2f large scanned, %llu "
                "full scans); %llu clears: %.0f items, %.1f large, %.0f heads each\n",
                (unsigned long long)s.inserts, per(s.insert_blocks, s.inserts),
                per(s.insert_scanned, s.insert_blocks), 100.0 * per(s.contained, s.insert_blocks),
                (unsigned long long)s.finds, per(s.find_blocks, s.finds),
                per(s.find_scanned, s.finds), per(s.large_scanned, s.finds),
                (unsigned long long)s.all_scans, (unsigned long long)s.clears,
                per(s.items_at_clear, s.clears), per(s.large_at_clear, s.clears),
                per(s.heads_at_clear, s.clears));
    s = {};
}
bool Overlap(u64 a_start, u64 a_end, const AddressRange& b) {
    return a_end >= b.range_start && a_start <= b.range_end;
}
} // namespace

BarrierTracker::BarrierTracker() {
    const char* env = std::getenv("BB_BARRIER_TRACKER");
    block_mode = !(env && std::string_view{env} == "tree");
    check_mode = env && std::string_view{env} == "check";
    if (block_mode) {
        heads.resize(HeadCount);
        items.reserve(8192);
        items.push_back({}); // index 0: end of a list
    }
    // Having an accessible 0 node makes certain things easier to
    // implement and allows us to use 0 as an invalid node index.
    nodes.emplace_back();

    // Pre-allocate root nodes for the implicit hash table
    for (u32 i = 0; i < 2u * HashTableSize; i++) {
        AllocateNode();
    }
}

BarrierTracker::BlockHead* BarrierTracker::FindHead(u64 resource, u64 block, u32 access,
                                                    bool create) {
    u64 hash = resource * 0x9E3779B97F4A7C15ull ^ block * 0xC2B2AE3D27D4EB4Full ^ access;
    hash ^= hash >> 29;
    hash *= 0xBF58476D1CE4E5B9ull;
    hash ^= hash >> 32;
    for (u32 probe = 0; probe < HeadCount; ++probe) {
        auto& head = heads[(hash + probe) & (HeadCount - 1)];
        if (head.epoch != epoch) {
            // Free slot: the key is not in the table. Kept under three quarters full so that
            // misses stay short; past that the caller lists the range in `large`.
            if (!create || heads_used >= HeadCount / 4 * 3) {
                return nullptr;
            }
            ++heads_used;
            head = {resource, block, epoch, 0, access};
            return &head;
        }
        if (head.resource == resource && head.block == block && head.access == access) {
            return &head;
        }
    }
    return nullptr;
}

bool BarrierTracker::BlockFind(const AddressRange& range, Access access) const {
    if (g_stats_on) {
        ++g_stats.finds;
        g_stats.large_scanned += large.size();
    }
    for (const u32 index : large) {
        const auto& item = items[index];
        if (item.resource == range.resource && item.access == u32(access) &&
            Overlap(item.start, item.end, range)) {
            return true;
        }
    }
    const u64 first = range.range_start >> BlockShift;
    const u64 last = range.range_end >> BlockShift;
    if (last - first >= MaxBlocksPerRange) {
        if (g_stats_on) {
            ++g_stats.all_scans;
        }
        for (size_t index = 1; index < items.size(); ++index) {
            const auto& item = items[index];
            if (item.resource == range.resource && item.access == u32(access) &&
                Overlap(item.start, item.end, range)) {
                return true;
            }
        }
        return false;
    }
    auto& self = const_cast<BarrierTracker&>(*this);
    for (u64 block = first; block <= last; ++block) {
        const BlockHead* head = self.FindHead(range.resource, block, u32(access), false);
        if (g_stats_on) {
            ++g_stats.find_blocks;
        }
        for (u32 index = head ? head->first : 0; index; index = items[index].next) {
            if (g_stats_on) {
                ++g_stats.find_scanned;
            }
            if (Overlap(items[index].start, items[index].end, range)) {
                return true;
            }
        }
    }
    return false;
}

void BarrierTracker::BlockInsert(const AddressRange& range, Access access) {
    if (g_stats_on) {
        ++g_stats.inserts;
        ReportStats();
    }
    const u64 first = range.range_start >> BlockShift;
    const u64 last = range.range_end >> BlockShift;
    const auto add_large = [&] {
        large.push_back(static_cast<u32>(items.size()));
        items.push_back({range.resource, range.range_start, range.range_end, 0, u32(access)});
    };
    if (last - first >= MaxBlocksPerRange) {
        add_large();
        return;
    }
    for (u64 block = first; block <= last; ++block) {
        BlockHead* head = FindHead(range.resource, block, u32(access), true);
        if (!head) {
            add_large();
            return;
        }
        bool contained = false;
        for (u32 index = head->first; index && !contained; index = items[index].next) {
            auto& item = items[index];
            if (g_stats_on) {
                ++g_stats.insert_scanned;
            }
            contained = item.start <= range.range_start && item.end >= range.range_end;
            // An item overlapping or touching the range grows to cover it when that keeps it
            // within the blocks it is listed in: the same overlaps, fewer items to scan (reads
            // come in neighbouring 4 KiB pages, writes in runs).
            if (!contained && item.end + 1 >= range.range_start &&
                item.start <= range.range_end + 1) {
                const u64 start = std::min(item.start, range.range_start);
                const u64 end = std::max(item.end, range.range_end);
                if (start >> BlockShift == item.start >> BlockShift &&
                    end >> BlockShift == item.end >> BlockShift) {
                    item.start = start;
                    item.end = end;
                    contained = true;
                }
            }
        }
        if (g_stats_on) {
            ++g_stats.insert_blocks;
            g_stats.contained += contained;
        }
        if (contained) {
            continue;
        }
        const u32 index = static_cast<u32>(items.size());
        items.push_back({range.resource, range.range_start, range.range_end, head->first,
                         u32(access)});
        head->first = index;
    }
}

bool BarrierTracker::FindRange(const AddressRange& range, Access access_type) const {
    if (block_mode) {
        const bool found = BlockFind(range, access_type);
        if (check_mode) {
            const u32 root_index = ComputeRootIndex(range, access_type);
            const bool tree = FindNode(range, root_index) != 0;
            static u32 prints = 0;
            if (found != tree && prints++ < 50) {
                std::printf("Barrier tracker check: %s range %#llx %#llx-%#llx: blocks %d, tree %d\n",
                            access_type == Access::Write ? "write" : "read",
                            (unsigned long long)range.resource,
                            (unsigned long long)range.range_start,
                            (unsigned long long)range.range_end, int(found), int(tree));
            }
            static u64 checks = 0;
            if ((++checks & ((1u << 22) - 1)) == 0) {
                std::printf("Barrier tracker check: %llu lookups compared, %u differed\n",
                            (unsigned long long)checks, prints);
            }
        }
        return found;
    }
    u32 root_index = ComputeRootIndex(range, access_type);
    u32 node_index = FindNode(range, root_index);
    return node_index;
}

void BarrierTracker::InsertRange(const AddressRange& range, Access access_type) {
    if (block_mode) {
        BlockInsert(range, access_type);
        if (!check_mode) {
            return;
        }
    }
    // If we can just insert the node with no conflicts, we don't have to do anything.
    u32 root_index = ComputeRootIndex(range, access_type);
    u32 node_index = InsertNode(range, root_index);

    if (!node_index) [[likely]] {
        return;
    }

    // If there's an existing node and it contains the entire range we want to add already, also
    // don't do anything. If there are conflicting access ops, reset it.
    auto& node = nodes[node_index];
    if (node.address_range.Contains(range)) {
        return;
    }

    // Otherwise, check if there are any other overlapping ranges.
    // If that is not the case, simply update the range we found.
    bool has_overlap = false;

    if (range.range_start < node.address_range.range_start) {
        AddressRange test_range;
        test_range.resource = range.resource;
        test_range.range_start = range.range_start;
        test_range.range_end = node.address_range.range_start - 1;
        has_overlap = FindNode(test_range, root_index);
    }

    if (range.range_end > node.address_range.range_end && !has_overlap) {
        AddressRange test_range;
        test_range.resource = range.resource;
        test_range.range_start = node.address_range.range_end + 1;
        test_range.range_end = range.range_end;
        has_overlap = FindNode(test_range, root_index);
    }

    if (!has_overlap) {
        node.address_range.range_start =
            std::min(node.address_range.range_start, range.range_start);
        node.address_range.range_end = std::max(node.address_range.range_end, range.range_end);
        return;
    }

    // If there are multiple ranges overlapping the one being inserted, remove them all and insert
    // the merged range.
    AddressRange merged_range = range;
    while (node_index) {
        auto& node = nodes[node_index];
        merged_range.range_start =
            std::min(merged_range.range_start, node.address_range.range_start);
        merged_range.range_end = std::max(merged_range.range_end, node.address_range.range_end);

        RemoveNode(node_index, root_index);
        node_index = FindNode(range, root_index);
    }
    InsertNode(merged_range, root_index);
}

void BarrierTracker::Clear() {
    if (block_mode) {
        if (g_stats_on) {
            ++g_stats.clears;
            g_stats.items_at_clear += items.size() - 1;
            g_stats.large_at_clear += large.size();
            g_stats.heads_at_clear += heads_used;
        }
        if (++epoch == 0) {
            // Wrapped: no slot may still carry the new epoch.
            for (auto& head : heads) {
                head.epoch = 0;
            }
            epoch = 1;
        }
        heads_used = 0;
        items.resize(1);
        large.clear();
        if (!check_mode) {
            return;
        }
    }
    root_mask_valid = 0;
    while (root_mask_subtree) {
        // Free subtrees if any, but keep the root node intact
        const u32 root_index = std::countr_zero(root_mask_subtree) + 1;
        auto& root = nodes[root_index];
        if (root.header) {
            FreeNode(root.Child(0));
            FreeNode(root.Child(1));
            root.header = 0;
        }
        root_mask_subtree &= root_mask_subtree - 1;
    }
}

u32 BarrierTracker::AllocateNode() {
    if (!free.empty()) {
        const u32 node_index = free.back();
        free.pop_back();

        // Free any subtree that the node might still have
        auto& node = nodes[node_index];
        FreeNode(node.Child(0));
        FreeNode(node.Child(1));

        node.header = 0;
        return node_index;
    } else {
        // Allocate entirely new node in the array
        const u32 node_index = nodes.size();
        nodes.emplace_back();
        return node_index;
    }
}

void BarrierTracker::FreeNode(u32 node) {
    if (node) {
        free.push_back(node);
    }
}

u32 BarrierTracker::FindNode(const AddressRange& range, u32 root_index) const {
    // Check if the given root is valid at all
    const u64 root_bit = 1ULL << (root_index - 1);
    if (!(root_mask_valid & root_bit)) {
        return false;
    }

    // Traverse search tree normally
    u32 node_index = root_index;
    while (node_index) {
        auto& node = nodes[node_index];
        if (node.address_range.Overlaps(range)) {
            return node_index;
        }
        node_index = node.Child(node.address_range < range);
    }
    return 0;
}

u32 BarrierTracker::InsertNode(const AddressRange& range, u32 root_index) {
    // Check if the given root is valid at all
    const u64 root_bit = 1ULL << (root_index - 1);

    if (!(root_mask_valid & root_bit)) {
        // Update root node as necessary. Also reset its red-ness if we set it during deletion.
        root_mask_valid |= root_bit;
        auto& node = nodes[root_index];
        node.header = 0;
        node.address_range = range;
        return 0;
    }

    // Traverse tree and abort if we find any range overlapping the one we're trying to insert.
    u32 parent_index = root_index;
    u32 child_index = 0;

    while (true) {
        auto& parent = nodes[parent_index];
        if (parent.address_range.Overlaps(range)) {
            return parent_index;
        }
        child_index = parent.address_range < range;
        if (!parent.Child(child_index)) {
            break;
        }
        parent_index = parent.Child(child_index);
    }

    // Create and insert new node into the tree
    const u32 node_index = AllocateNode();

    auto& parent = nodes[parent_index];
    parent.SetChild(child_index, node_index);

    auto& node = nodes[node_index];
    node.SetRed(true);
    node.SetParent(parent_index);
    node.address_range = range;

    // Only do the fixup to maintain red-black properties if
    // we haven't marked the root node as red in a deletion.
    if (parent_index != root_index && !nodes[root_index].IsRed()) {
        RebalancePostInsert(node_index, root_index);
    }

    root_mask_subtree |= root_bit;
    return 0;
}

void BarrierTracker::RemoveNode(u32 node_index, u32 root_index) {
    auto& node = nodes[node_index];

    u32 l = node.Child(0);
    u32 r = node.Child(1);

    if (l && r) {
        // Both children are valid. Take the payload from the smallest
        // node in the right subtree and delete that node instead.
        u32 child_index = r;
        while (nodes[child_index].Child(0)) {
            child_index = nodes[child_index].Child(0);
        }

        node.address_range = nodes[child_index].address_range;
        RemoveNode(child_index, root_index);
        return;
    }

    // Deletion is expected to be exceptionally rare, to the point of
    // being irrelevant in practice since it can only ever happen if an
    // app reads multiple disjoint blocks of a resource and then reads
    // another range covering multiple of those blocks again. Instead
    // of implementing a complex post-delete fixup, mark the root as
    // red and allow the tree to go unbalanced until the next reset.
    if (!node.IsRed() && (node_index != root_index)) {
        nodes[root_index].SetRed(true);
    }

    // We're deleting the a node with one or no children. To avoid
    // special-casing the root node, copy the child node to it and
    // update links as necessary.
    u32 child_index = std::max(l, r);
    u32 parent_index = node.Parent();

    if (child_index) {
        auto& child = nodes[child_index];

        u32 cl = child.Child(0);
        u32 cr = child.Child(1);

        node.SetChild(0, cl);
        node.SetChild(1, cr);

        if (node_index != root_index)
            node.SetRed(child.IsRed());

        node.address_range = child.address_range;

        if (cl) {
            nodes[cl].SetParent(node_index);
        }
        if (cr) {
            nodes[cr].SetParent(node_index);
        }

        child.header = 0;
        FreeNode(child_index);
    } else if (node_index != root_index) {
        // Removing leaf node, update parent link and move on.
        auto& parent = nodes[parent_index];

        const u32 which = parent.Child(1) == node_index;
        parent.SetChild(which, 0);

        node.header = 0;
        FreeNode(node_index);
    } else {
        // Removing root with no children, mark tree as invalid
        const u64 root_bit = 1ULL << (root_index - 1u);
        root_mask_subtree &= ~root_bit;
        root_mask_valid &= ~root_bit;
    }
}

void BarrierTracker::RebalancePostInsert(u32 node_index, u32 root_index) {
    while (node_index != root_index) {
        auto& node = nodes[node_index];
        auto& p = nodes[node.Parent()];

        if (!p.IsRed()) {
            break;
        }

        auto& g = nodes[p.Parent()];

        if (g.Child(1) == node.Parent()) {
            auto& u = nodes[g.Child(0)];

            if (g.Child(0) && u.IsRed()) {
                g.SetRed(true);
                u.SetRed(false);
                p.SetRed(false);
                node_index = p.Parent();
            } else {
                if (p.Child(0) == node_index) {
                    RotateRight(node.Parent(), root_index);
                }
                p.SetRed(false);
                g.SetRed(true);
                RotateLeft(p.Parent(), root_index);
            }
        } else {
            auto& u = nodes[g.Child(1)];
            if (g.Child(1) && u.IsRed()) {
                g.SetRed(true);
                u.SetRed(false);
                p.SetRed(false);
                node_index = p.Parent();
            } else {
                if (p.Child(1) == node_index) {
                    RotateLeft(node.Parent(), root_index);
                }
                p.SetRed(false);
                g.SetRed(true);
                RotateRight(p.Parent(), root_index);
            }
        }
    }
    nodes[root_index].SetRed(false);
}

void BarrierTracker::RotateLeft(u32 node_index, u32 root_index) {
    // This implements rotations in such a way that the node to
    // rotate around does not move. This is important to avoid
    // having a special case for the root node, and avoids having
    // to access the parent or special-case the root node.
    auto& node = nodes[node_index];

    u32 l = node.Child(0);
    u32 r = node.Child(1);
    u32 rl = nodes[r].Child(0);
    u32 rr = nodes[r].Child(1);

    nodes[l].SetParent(r);

    const bool is_red = nodes[r].IsRed();
    nodes[r].SetRed(node.IsRed());
    nodes[r].SetChild(0, l);
    nodes[r].SetChild(1, rl);

    nodes[rr].SetParent(node_index);

    node.SetRed(is_red && node_index != root_index);
    node.SetChild(0, r);
    node.SetChild(1, rr);

    std::swap(node.address_range, nodes[r].address_range);
}

void BarrierTracker::RotateRight(u32 node_index, u32 root_index) {
    auto& node = nodes[node_index];

    u32 l = node.Child(0);
    u32 r = node.Child(1);
    u32 ll = nodes[l].Child(0);
    u32 lr = nodes[l].Child(1);

    nodes[r].SetParent(l);

    const bool is_red = nodes[l].IsRed();
    nodes[l].SetRed(node.IsRed());
    nodes[l].SetChild(0, lr);
    nodes[l].SetChild(1, r);

    nodes[ll].SetParent(node_index);

    node.SetRed(is_red && node_index != root_index);
    node.SetChild(0, ll);
    node.SetChild(1, l);

    std::swap(node.address_range, nodes[l].address_range);
}

} // namespace Vulkan
