// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/info.h"
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace BbPrecompile {
using ConstantResolver = std::function<std::optional<u32>(u32)>;
// Resolve all template constants before the module reaches the Vulkan driver.
// This patches already compiled SPIR-V; it never invokes the shader translator.
// binding_offset: the first descriptor binding at the stage's place in the pipeline.
bool FreezeTemplate(std::span<const u32> input, const ConstantResolver &resolve,
                    std::vector<u32> &output, u32 binding_offset = 0);
std::optional<u32> DescriptorConstant(u32 slot, const Shader::Info &info);
} // namespace BbPrecompile
