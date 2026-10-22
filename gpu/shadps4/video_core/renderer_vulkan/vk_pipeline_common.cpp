// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <vector>
#include <x86intrin.h>
#include <xxhash.h>

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }
    {
        u64 buffers = 0, images = 0, samplers = 0;
        for (const auto& write : set_writes) {
            if (write.pBufferInfo) {
                buffers += write.descriptorCount;
            } else if (write.descriptorType == vk::DescriptorType::eSampler) {
                samplers += write.descriptorCount;
            } else {
                images += write.descriptorCount;
            }
        }
        descriptor_counts[0].fetch_add(1, std::memory_order_relaxed);
        descriptor_counts[1].fetch_add(buffers, std::memory_order_relaxed);
        descriptor_counts[2].fetch_add(images, std::memory_order_relaxed);
        descriptor_counts[3].fetch_add(samplers, std::memory_order_relaxed);
        // Statistics: how often a set's contents were written before (recently), exactly and
        // apart from the buffer offsets.
        static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
        if (stats) {
            static std::vector<u64> words;
            static std::vector<size_t> offsets;
            static std::array<u64, 1u << 16> seen_exact{}, seen_handles{};
            words.clear();
            offsets.clear();
            words.push_back(std::bit_cast<u64>(*desc_layout));
            for (const auto& write : set_writes) {
                words.push_back(u64{write.dstBinding} << 32 | u32(write.descriptorType));
                for (u32 i = 0; i < write.descriptorCount; ++i) {
                    if (write.pBufferInfo) {
                        words.push_back(std::bit_cast<u64>(write.pBufferInfo[i].buffer));
                        words.push_back(write.pBufferInfo[i].range);
                        offsets.push_back(words.size());
                        words.push_back(write.pBufferInfo[i].offset);
                    } else if (write.pImageInfo) {
                        words.push_back(std::bit_cast<u64>(write.pImageInfo[i].imageView));
                        words.push_back(std::bit_cast<u64>(write.pImageInfo[i].sampler));
                    }
                }
            }
            const u64 exact = XXH3_64bits(words.data(), words.size() * sizeof(u64)) | 1;
            for (const size_t at : offsets) {
                words[at] = 0;
            }
            const u64 handles = XXH3_64bits(words.data(), words.size() * sizeof(u64)) | 1;
            u64& slot_exact = seen_exact[exact & 0xffff];
            u64& slot_handles = seen_handles[handles & 0xffff];
            descriptor_repeats[0].fetch_add(slot_exact == exact, std::memory_order_relaxed);
            descriptor_repeats[1].fetch_add(slot_handles == handles, std::memory_order_relaxed);
            slot_exact = exact;
            slot_handles = handles;
        }
    }

    // bbport: without push descriptors (Dozen) a set is allocated and written per draw, which
    // costs as much as the rest of the draw. With threaded recording that work goes with the
    // command to the recording thread (toggle DescriptorThread: on this thread, as before).
    const bool descriptor_thread = !BbToggle::Disabled(BbToggle::DescriptorThread);
    if (uses_push_descriptors || (descriptor_thread && scheduler.IsRecordingDeferred())) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        if (uses_push_descriptors) {
            scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
            });
            return;
        }
        // The heap is locked: the toggle can send the next set to this thread while the recording
        // thread still allocates earlier ones.
        scheduler.Record([bind_point, layout, writes, set_layout = *desc_layout, heap = &desc_heap,
                          device = instance.GetDevice()](vk::CommandBuffer cmdbuf) {
            const u64 start = __rdtsc();
            const auto desc_set = heap->Commit(set_layout);
            auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
            for (size_t i = 0; i < writes.size(); ++i) {
                patched[i].dstSet = desc_set;
            }
            device.updateDescriptorSets(writes, {});
            cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
            descriptor_cycles[1].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const u64 start = __rdtsc();
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    descriptor_cycles[0].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
