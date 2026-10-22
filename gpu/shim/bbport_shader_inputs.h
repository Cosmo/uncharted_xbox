// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/offline_memory.h"
#include "video_core/amdgpu/resource.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>

namespace BbPrecompile {
// Shader resource tables (usage 0x0a, Uncharted) sit in user-data registers (the usage's top
// byte is their size in dwords, minus one) and point to further tables. Their layout is
// inferred from how the program uses what it reads: a field used as the base of a load points
// to a further table, one used as an image, sampler or buffer descriptor holds a default
// descriptor of that kind, and the rest reads as zero. Runtime lookup still checks every
// descriptor, so a wrong guess only costs the prepared shader.
struct SrtLayout {
    enum class Kind : u8 { Pointer, Image, Sampler, Buffer };
    struct Field {
        Kind kind;
        u32 size;
        u32 child;
        bool array = false; // images read as arrays
    };
    // tables[0]: the user-data registers, by register (16 and up: the extended user data).
    std::deque<std::map<u32, Field>> tables;
};

inline SrtLayout InferSrtLayout(std::span<const u32> code, std::span<const u32> usages) {
    using namespace Shader::Gcn;
    using Kind = SrtLayout::Kind;
    SrtLayout layout;
    layout.tables.emplace_back();
    // What a scalar register holds: a dword of a table (Field), or the extended user-data
    // pointer, through which table registers from 16 up are read.
    struct Source {
        enum : u8 { None, Field, Extended } type = None;
        u32 table = 0, offset = 0;
    };
    std::array<Source, 128> sgprs{};
    std::map<u32, Source> lanes; // scalar registers saved in vector-register lanes (v * 64 + lane)
    std::array<bool, 16 + 256> in_table{};
    bool any = false;
    for (u32 usage : usages) {
        const u32 type = usage & 255, reg = (usage >> 16) & 255, size = (usage >> 24) + 1;
        if (type == 0x1b && reg < 16)
            sgprs[reg] = {Source::Extended};
        if (type != 0x0a)
            continue;
        any = true;
        for (u32 i = reg; i < reg + size && i < in_table.size(); ++i) {
            in_table[i] = true;
            if (i < 16)
                sgprs[i] = {Source::Field, 0, i};
        }
    }
    if (!any || code.empty())
        return layout;
    // Classifies the dword a register holds; for a pointer, returns the table it points to.
    auto use = [&](u32 sgpr, Kind kind, u32 size, bool array = false) -> std::optional<u32> {
        if (sgpr >= sgprs.size() || sgprs[sgpr].type != Source::Field)
            return std::nullopt;
        const Source source = sgprs[sgpr];
        auto& fields = layout.tables[source.table];
        const auto [it, added] = fields.try_emplace(source.offset, SrtLayout::Field{kind, size, 0});
        if (it->second.kind == kind)
            it->second.array |= array;
        if (it->second.kind != kind || kind != Kind::Pointer)
            return std::nullopt;
        if (added) {
            it->second.child = layout.tables.size();
            layout.tables.emplace_back();
        }
        return it->second.child;
    };
    auto clear = [&](const InstOperand& operand) {
        if (operand.field != OperandField::ScalarGPR || operand.code >= sgprs.size())
            return;
        sgprs[operand.code] = {};
        const bool wide = operand.type == ScalarType::Uint64 || operand.type == ScalarType::Sint64 ||
                          operand.type == ScalarType::Float64;
        if (wide && operand.code + 1 < sgprs.size())
            sgprs[operand.code + 1] = {};
    };
    auto constant = [](const InstOperand& operand) -> std::optional<u32> {
        const u32 field = u32(operand.field);
        if (field >= u32(OperandField::ConstZero) && field < u32(OperandField::SignedConstIntNeg))
            return field - u32(OperandField::ConstZero);
        if (operand.field == OperandField::LiteralConst)
            return operand.code;
        return std::nullopt;
    };
    // Effect of one instruction on what the registers hold.
    auto step = [&](const GcnInst& inst) {
        switch (inst.inst_class) {
        case InstClass::ScalarMemRd: {
            const auto& smrd = inst.control.smrd;
            const u32 base = inst.src[0].code * 2, count = smrd.count;
            std::optional<u32> offset;
            if (smrd.imm)
                offset = smrd.offset;
            else if (smrd.offset == 0xff)
                offset = inst.src[1].code;
            const bool buffer = inst.opcode >= Opcode::S_BUFFER_LOAD_DWORD &&
                                inst.opcode <= Opcode::S_BUFFER_LOAD_DWORDX16;
            const bool extended =
                !buffer && base < sgprs.size() && sgprs[base].type == Source::Extended;
            std::optional<u32> table;
            if (buffer)
                use(base, Kind::Buffer, 4);
            else if (!extended)
                table = use(base, Kind::Pointer, 2);
            for (u32 i = 0; i < count && inst.dst[0].code + i < sgprs.size(); ++i) {
                Source loaded{};
                if (extended && offset) {
                    if (16 + *offset + i < in_table.size() && in_table[16 + *offset + i])
                        loaded = {Source::Field, 0, 16 + *offset + i};
                } else if (table && offset && *offset + i < 16000) {
                    loaded = {Source::Field, *table, *offset + i};
                }
                sgprs[inst.dst[0].code + i] = loaded;
            }
            break;
        }
        case InstClass::VectorMemBufNoFmt:
        case InstClass::VectorMemBufFmt:
        case InstClass::VectorMemBufAtomic:
            use(inst.src[2].code * 4, Kind::Buffer, 4);
            break;
        case InstClass::VectorMemImgSmp:
            use(inst.src[3].code * 4, Kind::Sampler, 4);
            [[fallthrough]];
        case InstClass::VectorMemImgNoSmp:
        case InstClass::VectorMemImgUt:
            use(inst.src[2].code * 4, Kind::Image, inst.control.mimg.r128 ? 4 : 8,
                inst.control.mimg.da);
            break;
        default:
            if (inst.opcode == Opcode::V_WRITELANE_B32 || inst.opcode == Opcode::V_READLANE_B32) {
                const bool write = inst.opcode == Opcode::V_WRITELANE_B32;
                const auto& vector = write ? inst.dst[0] : inst.src[0];
                const auto& scalar = write ? inst.src[0] : inst.dst[0];
                const auto lane = constant(inst.src[1]);
                if (vector.field != OperandField::VectorGPR || scalar.field != OperandField::ScalarGPR ||
                    scalar.code >= sgprs.size()) {
                    for (u32 i = 0; i < inst.dst_count && i < inst.dst.size(); ++i)
                        clear(inst.dst[i]);
                } else if (write && lane && sgprs[scalar.code].type != Source::None) {
                    lanes[vector.code * 64 + (*lane & 63)] = sgprs[scalar.code];
                } else if (write) {
                    if (lane)
                        lanes.erase(vector.code * 64 + (*lane & 63));
                    else
                        lanes.erase(lanes.lower_bound(vector.code * 64),
                                    lanes.lower_bound(vector.code * 64 + 64));
                } else {
                    const auto saved = lane ? lanes.find(vector.code * 64 + (*lane & 63)) : lanes.end();
                    sgprs[scalar.code] = saved != lanes.end() ? saved->second : Source{};
                }
                break;
            }
            if ((inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOV_B64) &&
                inst.src[0].field == OperandField::ScalarGPR) {
                const u32 words = inst.opcode == Opcode::S_MOV_B64 ? 2 : 1;
                for (u32 i = 0; i < words && inst.dst[0].code + i < sgprs.size(); ++i)
                    sgprs[inst.dst[0].code + i] =
                        inst.src[0].code + i < sgprs.size() ? sgprs[inst.src[0].code + i] : Source{};
                break;
            }
            for (u32 i = 0; i < inst.dst_count && i < inst.dst.size(); ++i) {
                clear(inst.dst[i]);
                if (inst.dst[i].field == OperandField::VectorGPR && !lanes.empty())
                    lanes.erase(lanes.lower_bound(inst.dst[i].code * 64),
                                lanes.lower_bound(inst.dst[i].code * 64 + 64));
            }
            break;
        }
    };
    // Follow the control flow: a block starts from what all its predecessors agree on, so a
    // register reused on one path keeps its meaning on the others.
    struct Decoded {
        GcnInst inst;
        u32 pc;
    };
    std::vector<Decoded> program;
    {
        GcnCodeSlice slice(code.data(), code.data() + code.size());
        GcnDecodeContext decoder;
        for (u32 pc = 0; !slice.atEnd();) {
            program.push_back({decoder.decodeInstruction(slice), pc});
            pc += program.back().inst.length;
        }
    }
    std::map<u32, size_t> blocks; // first pc -> block index
    blocks[0] = 0;
    for (const auto& [inst, pc] : program) {
        if (inst.opcode == Opcode::S_BRANCH || inst.IsConditionalBranch())
            blocks.try_emplace(inst.BranchTarget(pc), 0);
        if (inst.IsTerminateInstruction())
            blocks.try_emplace(pc + inst.length, 0);
    }
    std::vector<u32> starts;
    for (auto& [pc, index] : blocks) {
        index = starts.size();
        starts.push_back(pc);
    }
    std::vector<std::vector<size_t>> successors(starts.size());
    std::vector<std::pair<size_t, size_t>> ranges(starts.size()); // instruction indices
    for (size_t i = 0, block = 0; i < program.size(); ++i) {
        const auto& [inst, pc] = program[i];
        if (const auto it = blocks.find(pc); it != blocks.end()) {
            block = it->second;
            ranges[block].first = i;
        }
        ranges[block].second = i + 1;
        const bool last = i + 1 == program.size() || blocks.contains(program[i + 1].pc);
        if (!last)
            continue;
        const auto add = [&](u32 target) {
            if (const auto it = blocks.find(target); it != blocks.end())
                successors[block].push_back(it->second);
        };
        if (inst.opcode == Opcode::S_BRANCH || inst.IsConditionalBranch())
            add(inst.BranchTarget(pc));
        if (!inst.IsUnconditionalBranch() && inst.opcode != Opcode::S_ENDPGM)
            add(pc + inst.length);
    }
    struct State {
        std::array<Source, 128> sgprs;
        std::map<u32, Source> lanes;
    };
    std::vector<std::optional<State>> entry(starts.size()), exit(starts.size());
    entry[0] = State{sgprs, lanes};
    const auto same = [](const Source& a, const Source& b) {
        return a.type == b.type && a.table == b.table && a.offset == b.offset;
    };
    const auto same_lane = [&](const auto& a, const auto& b) {
        return a.first == b.first && same(a.second, b.second);
    };
    // Blocks run in program order; a loop's back edge only narrows later passes.
    for (u32 pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (size_t block = 0; block < starts.size(); ++block) {
            if (!entry[block] || ranges[block].second <= ranges[block].first)
                continue;
            sgprs = entry[block]->sgprs;
            lanes = entry[block]->lanes;
            for (size_t i = ranges[block].first; i < ranges[block].second; ++i)
                step(program[i].inst);
            if (exit[block] && std::ranges::equal(exit[block]->sgprs, sgprs, same) &&
                std::ranges::equal(exit[block]->lanes, lanes, same_lane))
                continue;
            exit[block] = State{sgprs, lanes};
            for (size_t next : successors[block]) {
                if (!entry[next]) {
                    entry[next] = State{sgprs, lanes};
                    changed = true;
                    continue;
                }
                auto& in = *entry[next];
                for (size_t r = 0; r < sgprs.size(); ++r)
                    if (!same(in.sgprs[r], sgprs[r]) && in.sgprs[r].type != Source::None) {
                        in.sgprs[r] = {};
                        changed = true;
                    }
                for (auto it = in.lanes.begin(); it != in.lanes.end();) {
                    const auto found = lanes.find(it->first);
                    if (found != lanes.end() && same(found->second, it->second)) {
                        ++it;
                    } else {
                        it = in.lanes.erase(it);
                        changed = true;
                    }
                }
            }
        }
        if (!changed)
            break;
    }
    return layout;
}

// Explicit descriptor variant: ordinary float 2D images, normalized samplers,
// and unformatted constant buffers. Runtime lookup must check specialization
// before using an output prepared for this variant.
class AssetInputs final : public Shader::OfflineMemory {
  public:
    explicit AssetInputs(std::span<const u32> usages, u32 sampler_flags = 0, bool parametric = false,
                         std::span<const u32> code = {})
        : parametric(parametric), srt(InferSrtLayout(code, usages)) {
        AmdGpu::Image image = AmdGpu::Image::Null(true);
        image.base_address = 1;
        AmdGpu::Buffer buffer = AmdGpu::Buffer::Placeholder(65536);
        buffer.data_format = u32(AmdGpu::DataFormat::Format32);
        buffer.stride = 16;
        buffer.num_format = u32(AmdGpu::NumberFormat::Float);
        buffer.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buffer.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        buffer.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);
        for (size_t at = 0; at < resources.size(); at += 8)
            std::memcpy(resources.data() + at, &image, sizeof(image));
        AmdGpu::Sampler sampler{};
        sampler.force_unnormalized.Assign(sampler_flags & 1);
        sampler.force_degamma.Assign((sampler_flags >> 1) & 1);
        for (size_t at = 0; at < samplers.size(); at += 4)
            std::memcpy(samplers.data() + at, &sampler, sizeof(sampler));
        std::memcpy(image_words.data(), &image, sizeof(image));
        auto array = image;
        array.type = u64(AmdGpu::ImageType::Color2DArray);
        std::memcpy(array_words.data(), &array, sizeof(array));
        std::memcpy(sampler_words.data(), &sampler, sizeof(sampler));
        std::memcpy(buffer_words.data(), &buffer, sizeof(buffer));
        for (u32 usage : usages) {
            const u32 type = usage & 255, reg = (usage >> 16) & 255, flags = usage >> 24;
            switch (type) {
            case 0: // immediate V# or T#
            case 4: // read-write V# or T# (Uncharted)
                if (flags & 2)
                    Put(reg, image);
                else
                    Put(reg, buffer);
                break;
            case 7:
                break; // GDS counter range (Uncharted): an offset, no descriptor
            case 1:
                Put(reg, sampler);
                break;
            case 2:
                Put(reg, buffer);
                break;
            case 0x13:
                Put(reg, ResourceBase);
                break;
            case 0x15:
                Put(reg, SamplerBase);
                break;
            case 0x1b:
                Put(reg, ExtendedBase);
                break;
            case 0x0a: // shader resource table (Uncharted): its registers follow the loop
                if (srt.tables[0].empty())
                    throw std::runtime_error("Shader resource table layout unknown");
                break;
            case 0x12:
                break; // Fetch supplied explicitly from the asset semantics.
            case 0x17:
                Put(reg, VertexBase);
                break;
            default:
                throw std::runtime_error("Unsupported asset resource usage type");
            }
        }
        for (const auto& [reg, field] : srt.tables[0]) {
            std::array<u32, 8> words{};
            for (u32 i = 0; i < field.size; ++i)
                words[i] = SrtWord(field, i);
            PutWords(reg, std::span<const u32>(words).first(field.size));
        }
    }
    std::span<const u32> UserData() const { return user; }
    bool Parametric() const override { return parametric; }
    u32 Read32(u64 address) const override {
        if (address % 4)
            throw std::runtime_error("Unaligned offline descriptor read");
        auto read = [&](u64 base, std::span<const u32> words) -> const u32* {
            if (address >= base && (address - base) / 4 < words.size())
                return &words[(address - base) / 4];
            return nullptr;
        };
        if (auto p = read(ExtendedBase, extended))
            return *p;
        if (auto p = read(ResourceBase, resources))
            return *p;
        if (auto p = read(SamplerBase, samplers))
            return *p;
        if (auto p = read(VertexBase, vertices))
            return *p;
        if (address >= SrtBase + SrtStride && (address - SrtBase) / SrtStride < srt.tables.size())
            return ReadSrt(srt.tables[(address - SrtBase) / SrtStride],
                           u32((address - SrtBase) % SrtStride / 4));
        throw std::runtime_error("Offline descriptor read outside supplied tables");
    }

  private:
    bool parametric;
    SrtLayout srt;
    static constexpr u64 ExtendedBase = 0x10000, ResourceBase = 0x20000, SamplerBase = 0x30000,
                         VertexBase = 0x40000, SrtBase = 0x1000000, SrtStride = 0x10000;
    std::array<u32, 8> image_words{}, array_words{};
    std::array<u32, 4> sampler_words{}, buffer_words{};
    std::array<u32, 16> user{};
    std::array<u32, 256> extended{};
    std::array<u32, 2048> resources{};
    std::array<u32, 1024> samplers{};
    std::array<u32, 1024> vertices{};
    u32 SrtWord(const SrtLayout::Field& field, u32 at) const {
        switch (field.kind) {
        case SrtLayout::Kind::Pointer:
            return at ? 0 : u32(SrtBase + u64(field.child) * SrtStride);
        case SrtLayout::Kind::Image:
            return (field.array ? array_words : image_words)[at];
        case SrtLayout::Kind::Sampler:
            return sampler_words[at];
        case SrtLayout::Kind::Buffer:
            return buffer_words[at];
        }
        return 0;
    }
    u32 ReadSrt(const std::map<u32, SrtLayout::Field>& fields, u32 dword) const {
        auto it = fields.upper_bound(dword);
        if (it == fields.begin())
            return 0;
        --it;
        const u32 at = dword - it->first;
        return at < it->second.size ? SrtWord(it->second, at) : 0;
    }
    void PutWords(u32 reg, std::span<const u32> value) {
        auto words = reg < 16 ? std::span<u32>(user) : std::span<u32>(extended);
        const u32 offset = reg < 16 ? reg : reg - 16;
        if (offset + value.size() > words.size())
            throw std::runtime_error("Asset descriptor crosses register bounds");
        std::memcpy(words.data() + offset, value.data(), value.size_bytes());
    }
    template <class T> void Put(u32 reg, const T& value) {
        static_assert(sizeof(T) % 4 == 0);
        std::array<u32, sizeof(T) / 4> words;
        std::memcpy(words.data(), &value, sizeof(value));
        PutWords(reg, words);
    }
};

inline Shader::Gcn::FetchShaderSnapshot AssetFetch(std::span<const u32> semantics,
                                                 std::span<const u32> usages, bool skinned = false,
                                                 bool unsigned_indices = false) {
    Shader::Gcn::FetchShaderSnapshot result;
    result.fetch.emplace();
    result.fetch->size = semantics.size() * 12 + 12;
    u32 table_register = 0;
    bool found = false;
    for (u32 usage : usages)
        if ((usage & 255) == 0x17) {
            table_register = (usage >> 16) & 255;
            found = true;
        }
    if (!found)
        throw std::runtime_error("Vertex asset is missing its buffer-table register");
    for (u32 packed : semantics) {
        const u32 semantic = packed & 255, vgpr = (packed >> 8) & 255,
                  components = (packed >> 16) & 255;
        if (packed >> 24 || !components || components > 4 || semantic >= 64 || vgpr > 252)
            throw std::runtime_error("Unsupported vertex input semantic");
        Shader::Gcn::VertexAttribute attribute{};
        attribute.semantic = result.fetch->attributes.size();
        attribute.dest_vgpr = vgpr;
        attribute.num_elements = components;
        attribute.sgpr_base = table_register;
        attribute.dword_offset = semantic * 4;
        result.fetch->attributes.push_back(attribute);
        auto buffer = AmdGpu::Buffer::Placeholder(65536);
        buffer.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        buffer.num_format = u32(AmdGpu::NumberFormat::Float);
        buffer.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buffer.dst_sel_y =
            u32(components >= 2 ? AmdGpu::CompSwizzle::Green : AmdGpu::CompSwizzle::Zero);
        buffer.dst_sel_z =
            u32(components >= 3 ? AmdGpu::CompSwizzle::Blue : AmdGpu::CompSwizzle::Zero);
        buffer.dst_sel_w =
            u32(components == 4 ? AmdGpu::CompSwizzle::Alpha : AmdGpu::CompSwizzle::One);
        // Explicit common mesh-layout variant. Other packed layouts remain separate.
        if (semantic == 0 && components == 4)
            buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::One);
        if (semantic >= 14 && !skinned) {
            buffer.dst_sel_x = buffer.dst_sel_y = buffer.dst_sel_z = u32(AmdGpu::CompSwizzle::Zero);
            buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::One);
        }
        if(semantic==15&&skinned) buffer.num_format=u32(unsigned_indices ? AmdGpu::NumberFormat::Uint : AmdGpu::NumberFormat::Sint);
        result.buffers.push_back(buffer);
    }
    return result;
}
} // namespace BbPrecompile
