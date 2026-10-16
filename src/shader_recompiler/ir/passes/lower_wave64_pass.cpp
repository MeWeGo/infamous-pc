// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <unordered_set>
#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

bool IsLaneVaryingSource(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LaneId:
    case IR::Opcode::MaskedBitCount32:
        return true;
    case IR::Opcode::GetAttributeU32: {
        const auto attr = inst.Arg(0).Attribute();
        return attr == IR::Attribute::LocalInvocationId ||
               attr == IR::Attribute::LocalInvocationIndex ||
               attr == IR::Attribute::SubgroupLtMask;
    }
    default: {
        // Atomic ops returning the old value produce a distinct result per participating lane.
        const auto op = inst.GetOpcode();
        if ((op >= IR::Opcode::SharedAtomicIAdd32 && op <= IR::Opcode::SharedAtomicCmpSwap64) ||
            (op >= IR::Opcode::BufferAtomicIAdd32 && op <= IR::Opcode::BufferAtomicFCmpSwap32) ||
            (op >= IR::Opcode::ImageAtomicIAdd32 && op <= IR::Opcode::ImageAtomicCmpSwap32)) {
            return inst.Flags<bool>();
        }
        return false;
    }
    }
}

bool ReachesLaneVaryingSource(const IR::Value& root,
                              const std::function<bool(const IR::Inst&)>& is_uniform_leaf) {
    if (root.IsImmediate()) {
        return false;
    }
    std::unordered_set<const IR::Inst*> visited;
    std::vector<const IR::Inst*> stack{root.Inst()};
    while (!stack.empty()) {
        const IR::Inst* const inst = stack.back();
        stack.pop_back();
        if (!visited.insert(inst).second) {
            continue;
        }
        if (is_uniform_leaf(*inst)) {
            continue;
        }
        if (IsLaneVaryingSource(*inst)) {
            return true;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            const IR::Value arg = inst->Arg(i);
            if (!arg.IsImmediate()) {
                stack.push_back(arg.Inst());
            }
        }
    }
    return false;
}

static bool IsProvablyWaveUniform(const IR::Value& root) {
    if (root.IsImmediate()) {
        return true;
    }
    return !ReachesLaneVaryingSource(root, [](const IR::Inst& inst) {
        return inst.GetOpcode() == IR::Opcode::ReadFirstLane ||
               (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate());
    });
}

static bool IsDivergentCondition(const IR::U1& condition) {
    return !IsProvablyWaveUniform(condition);
}

// A loop is divergent when any exit condition (Repeat or Break) can vary across lanes, meaning
// different lanes may execute different numbers of iterations.
static std::unordered_set<const IR::Block*> FindDivergentLoops(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    std::unordered_set<const IR::Block*> divergent;
    std::vector<const IR::Block*> loop_stack;
    u32 cond_divergence_depth{};
    std::vector<bool> cond_stack;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::Loop:
            loop_stack.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (IsDivergentCondition(node.data.repeat.cond) || cond_divergence_depth > 0) {
                divergent.insert(node.data.repeat.merge);
            }
            if (!loop_stack.empty() && loop_stack.back() == node.data.repeat.merge) {
                loop_stack.pop_back();
            }
            break;
        case Type::Break:
            // A Break inside a divergent If is divergent even if its own cond is true, because
            // only the lanes that entered the If reach the Break.
            if (IsDivergentCondition(node.data.break_node.cond) || cond_divergence_depth > 0) {
                divergent.insert(node.data.break_node.merge);
            }
            break;
        case Type::If: {
            const bool div = IsDivergentCondition(node.data.if_node.cond);
            cond_stack.push_back(div);
            cond_divergence_depth += static_cast<u32>(div);
            break;
        }
        case Type::EndIf:
            if (!cond_stack.empty()) {
                cond_divergence_depth -= static_cast<u32>(cond_stack.back());
                cond_stack.pop_back();
            }
            break;
        default:
            break;
        }
    }
    return divergent;
}

std::vector<IR::Block*> FindWave64UniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct Scope {
        const IR::Block* merge;
        bool divergent;
    };

    const auto divergent_loops = FindDivergentLoops(program);

    std::vector<IR::Block*> blocks;
    std::vector<Scope> conditionals;
    std::vector<Scope> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop: {
            const bool divergent = divergent_loops.contains(node.data.loop.merge);
            loops.push_back({node.data.loop.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::Repeat:
            if (loops.empty() || loops.back().merge != node.data.repeat.merge) {
                return {};
            }
            divergence_depth -= static_cast<u32>(loops.back().divergent);
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

// When a workgroup has fewer threads than a GCN wave64 and ReadLane reads a lane >= host subgroup
// size, that lane is permanently inactive on GCN. Look at the consuming reduction op and fold the
// ReadLane to its identity element so a bogus lane0/undefined read doesn't corrupt the reduction.
static std::optional<u32> ReductionIdentity(const IR::Inst& read_lane) {
    if (!read_lane.HasUses()) {
        return 0u;
    }
    std::optional<u32> identity;
    for (const IR::Use& use : read_lane.Uses()) {
        u32 id{};
        switch (use.user->GetOpcode()) {
        case IR::Opcode::IAdd32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
        case IR::Opcode::UMax32:
            id = 0u;
            break;
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::UMin32:
            id = ~0u;
            break;
        case IR::Opcode::SMax32:
            id = 0x8000'0000u;
            break;
        case IR::Opcode::SMin32:
            id = 0x7fff'ffffu;
            break;
        case IR::Opcode::FPAdd32:
            id = std::bit_cast<u32>(0.0f);
            break;
        case IR::Opcode::FPMax32:
            id = std::bit_cast<u32>(-std::numeric_limits<float>::infinity());
            break;
        case IR::Opcode::FPMin32:
            id = std::bit_cast<u32>(std::numeric_limits<float>::infinity());
            break;
        default:
            return std::nullopt;
        }
        if (identity && *identity != id) {
            return std::nullopt;
        }
        identity = id;
    }
    return identity;
}

static IR::Inst* FindBallotForMaskedBitCount(const IR::Inst& mbcnt) {
    IR::Value value = mbcnt.Arg(0);
    if (value.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* inst = value.Inst();
    if (inst->GetOpcode() != IR::Opcode::CompositeExtractU32x2 || inst->Arg(0).IsImmediate()) {
        return nullptr;
    }
    inst = inst->Arg(0).Inst();
    if (inst->GetOpcode() != IR::Opcode::UnpackUint2x32 || inst->Arg(0).IsImmediate()) {
        return nullptr;
    }
    inst = inst->Arg(0).Inst();
    return inst->GetOpcode() == IR::Opcode::Ballot ? inst : nullptr;
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        const u32 host_lanes = std::min<u32>(profile.subgroup_size, 32);
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate() &&
                    inst.Arg(1).U32() >= host_lanes) {
                    if (const auto id = ReductionIdentity(inst)) {
                        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                        inst.ReplaceUsesWithAndRemove(ir.Imm32(*id));
                    }
                }
            }
        }
        return;
    }

    // Only lower ReadFirstLane when the shader also computes wave64-wide lane masks/indices
    // (Ballot or v_mbcnt) or the input is not provably wave-uniform.
    bool has_lane_math = false;
    for (IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            if (op == IR::Opcode::Ballot || op == IR::Opcode::MaskedBitCount32 ||
                (op == IR::Opcode::GetAttributeU32 &&
                 inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask)) {
                has_lane_math = true;
                break;
            }
        }
        if (has_lane_math) {
            break;
        }
    }

    std::vector<IR::Inst*> worklist;
    std::vector<IR::Inst*> divergent_ballots;
    const auto uniform_blocks = FindWave64UniformBlocks(program);
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                LOG_WARNING(Render_Recompiler, "{} instruction in non uniform control flow",
                            inst.GetOpcode());
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::ReadFirstLane && is_uniform &&
                       (has_lane_math || !IsProvablyWaveUniform(inst.Arg(0)))) {
                worklist.push_back(&inst);
            } else if (inst.GetOpcode() == IR::Opcode::Ballot) {
                const auto is_unpack = [](const IR::Use& use) {
                    return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
                };
                if (std::ranges::any_of(inst.Uses(), is_unpack)) {
                    if (is_uniform) {
                        worklist.push_back(&inst);
                    } else {
                        divergent_ballots.push_back(&inst);
                    }
                }
            } else if (inst.GetOpcode() == IR::Opcode::MaskedBitCount32) {
                IR::Inst* const ballot = FindBallotForMaskedBitCount(inst);
                if (ballot == nullptr ||
                    std::ranges::contains(uniform_blocks, ballot->GetParent()) ||
                    std::ranges::contains(divergent_ballots, ballot)) {
                    worklist.push_back(&inst);
                }
            }
        }
    }

    // For a Ballot in non-uniform control flow we can't use an LDS barrier (not all lanes reach
    // it), so cross-half merging is impossible. At least place the host 32-bit ballot in the half
    // of the 64-bit mask corresponding to this subgroup's half of the wave64, so a subsequent
    // MaskedBitCount32 (which indexes bits 0..63 by LocalInvocationIndex & 63) sees the upper
    // subgroup's ballot bits in the upper word instead of the lower word.
    for (IR::Inst* inst : divergent_ballots) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 is_hi =
            ir.BitwiseAnd(ir.ShiftRightLogical(invocation_index, ir.Imm32(5u)), ir.Imm32(1u));
        const IR::U1 is_hi_cond = ir.INotEqual(is_hi, ir.Imm32(0u));
        const IR::U32 mask32 =
            IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
        const IR::U32 zero = ir.Imm32(0u);
        const IR::U32 lo = IR::U32{ir.Select(is_hi_cond, zero, mask32)};
        const IR::U32 hi = IR::U32{ir.Select(is_hi_cond, mask32, zero)};
        const IR::U64 mask64 = ir.PackUint2x32(ir.CompositeConstruct(lo, hi));
        inst->ReplaceUsesWithAndRemove(mask64);
    }

    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            const IR::U64 mask = IR::U64{ir.LoadShared(64, false, offset)};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            // In uniform control flow EXEC has bit 0 set, so GCN v_readfirstlane_b32 reads wave64
            // lane 0 (the lower wave32 subgroup's lane 0).
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadFirstLane(IR::U32{inst->Arg(0)}), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false, ir.BitwiseAnd(offset, ir.Imm32(~7u)))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            const IR::U32 thread_mask{
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(2).U1() ? 1u : 0u)};
            const IR::U32 masked_value{
                ir.BitCount(ir.BitwiseAnd(IR::U32{inst->Arg(0)}, thread_mask))};
            inst->ReplaceUsesWithAndRemove(ir.IAdd(masked_value, IR::U32{inst->Arg(1)}));
        }
    }
}

} // namespace Shader::Optimization
