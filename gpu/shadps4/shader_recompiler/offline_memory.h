// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include "video_core/amdgpu/pixel_format.h"
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace Shader {
// A bounded descriptor address space owned by an asset compiler. Addresses are
// synthetic identifiers, never pointers into a running or previous guest.
class OfflineMemory {
  public:
    virtual ~OfflineMemory() = default;
    virtual u32 Read32(u64 address) const = 0;
    virtual bool Parametric() const { return false; }
};

namespace OfflineSlot {
constexpr u32 Normalize = 0x1000;
constexpr u32 Gamma = 0x2000; // sampler * 64 + image
constexpr u32 Stride = 0x3000;
constexpr u32 Swizzle = 0x4000; // vertex attribute * 4 + component
constexpr u32 BufferPresent = 0x5000;
constexpr u32 ImagePresent = 0x6000;
constexpr u32 BufferBinding = 0x7000; // buffer * 4 + part: push-data member, component, bit offset
constexpr u32 UserData = 0x7400;      // register index * 2 + part: push-data member, component
constexpr u32 BufferFormat = 0x7800;  // buffer: its TemplateBufferFormat
}

// The formats a prepared formatted buffer load (Uncharted's vertex streams) is ready for; the
// template takes the descriptor's when it is frozen. None of them needs a number conversion.
inline std::optional<u32> TemplateBufferFormat(AmdGpu::DataFormat data, AmdGpu::NumberFormat number) {
    using enum AmdGpu::DataFormat;
    using enum AmdGpu::NumberFormat;
    constexpr std::array<std::pair<AmdGpu::DataFormat, AmdGpu::NumberFormat>, 12> formats{{
        {Format32, Float}, {Format32_32, Float}, {Format32_32_32, Float},
        {Format32_32_32_32, Float}, {Format16_16, Float}, {Format16_16_16_16, Float},
        {Format8_8_8_8, Unorm}, {Format16_16, Unorm}, {Format8_8_8_8, Snorm},
        {Format16_16, Snorm}, {Format16_16_16_16, Snorm}, {Format8_8_8_8, Uint},
    }};
    for (const auto& [d, n] : formats)
        if (d == data && n == number)
            return u32(data) | u32(number) << 8;
    return std::nullopt;
}
template <class Visit> void ForEachTemplateBufferFormat(Visit&& visit) {
    using enum AmdGpu::DataFormat;
    using enum AmdGpu::NumberFormat;
    for (const auto data : {Format32, Format32_32, Format32_32_32, Format32_32_32_32, Format16_16,
                            Format16_16_16_16, Format8_8_8_8})
        for (const auto number : {Float, Unorm, Snorm, Uint})
            if (const auto code = TemplateBufferFormat(data, number))
                visit(data, number, *code);
}

struct SrtRead {
    // First offset is a user-data register pair; following offsets follow
    // pointers in descriptor tables. The leaf offset reads one data dword.
    std::vector<u32> pointers;
    u32 offset = 0;
    u32 destination = 0;
};
using SrtReadPlan = std::vector<SrtRead>;
struct PersistentSrtInfo;
// Register a validated asset-derived read layout against current guest tables.
// Called only for shaders actually used; compilation never dereferences a guest.
PersistentSrtInfo RegisterSrtReadPlan(const SrtReadPlan& plan, u32 flat_words, u32 user_words);
} // namespace Shader
