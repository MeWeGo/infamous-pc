// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <utility>

#include "common/types.h"

namespace VideoCore {

constexpr u64 PAGES_PER_WORD = 64;
constexpr u64 BYTES_PER_PAGE = 4_KB;
constexpr u64 BYTES_PER_WORD = PAGES_PER_WORD * BYTES_PER_PAGE;

constexpr u64 HIGHER_PAGE_BITS = 24;
constexpr u64 HIGHER_PAGE_SIZE = 1ULL << HIGHER_PAGE_BITS;
constexpr u64 HIGHER_PAGE_MASK = HIGHER_PAGE_SIZE - 1ULL;

constexpr u64 NUM_REGION_PAGES = HIGHER_PAGE_SIZE / BYTES_PER_PAGE;
constexpr u64 NUM_REGION_WORDS = HIGHER_PAGE_SIZE / BYTES_PER_WORD;

enum class Type : u8 {
    CPU = 1 << 0,
    GPU = 1 << 1,
};

enum class StateOp : u8 {
    None = 0,
    Set = 1,
    Clear = 2,
};

constexpr bool operator&(Type a, Type b) noexcept {
    return std::to_underlying(a) & std::to_underlying(b);
}

constexpr Type operator|(Type a, Type b) noexcept {
    return static_cast<Type>(std::to_underlying(a) | std::to_underlying(b));
}

struct Bounds {
    u64 start_word;
    u64 start_page;
    u64 end_word;
    u64 end_page;
};

/// 4096-page bitmap (64 x 64-bit words) covering one 16 MB region.
/// Shared instances in RegionManager (cpu, gpu, rewritten_now, rewritten_before) are mutated
/// under RegionManager::mutex and read lock-free by the GPU thread via Load()/Store() using
/// relaxed std::atomic_ref<u64> to avoid C++ data-race UB and TSan reports.
struct RegionBits {
    constexpr void Fill(u64 value) noexcept {
        if consteval {
            data.fill(value);
        } else {
            for (u64& word : data) {
                std::atomic_ref<u64>{word}.store(value, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] u64 Load(u64 index) const noexcept {
        return std::atomic_ref<u64>{data[index]}.load(std::memory_order_relaxed);
    }

    void Store(u64 index, u64 value) noexcept {
        std::atomic_ref<u64>{data[index]}.store(value, std::memory_order_relaxed);
    }

    constexpr bool GetPage(u64 page) const noexcept {
        if consteval {
            return data[page / PAGES_PER_WORD] & (1ULL << (page % PAGES_PER_WORD));
        } else {
            return Load(page / PAGES_PER_WORD) & (1ULL << (page % PAGES_PER_WORD));
        }
    }

    constexpr u64& operator[](u64 index) noexcept {
        return data[index];
    }

    constexpr u64 operator[](u64 index) const noexcept {
        return data[index];
    }

private:
    alignas(64) mutable std::array<u64, NUM_REGION_WORDS> data;
};

} // namespace VideoCore
