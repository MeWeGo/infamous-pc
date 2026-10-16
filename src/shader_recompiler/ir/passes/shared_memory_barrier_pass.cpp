// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_set>
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

static void EmitBarrier(IR::IREmitter& ir, bool use_subgroup_barrier) {
    if (use_subgroup_barrier) {
        ir.SubgroupBarrier();
    } else {
        ir.Barrier();
    }
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block, bool use_subgroup_barrier) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::Barrier) {
            action = BarrierAction::None;
            continue;
        }
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitBarrier(ir, use_subgroup_barrier);
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitBarrier(ir, use_subgroup_barrier);
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        const auto last = --block->end();
        if (last->GetOpcode() != IR::Opcode::Barrier) {
            IR::IREmitter ir{*block, last};
            EmitBarrier(ir, use_subgroup_barrier);
        }
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block, bool use_subgroup_barrier) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    if (insert_point != block->end() && insert_point->GetOpcode() == IR::Opcode::Barrier) {
        return;
    }
    IR::IREmitter ir{*block, insert_point};
    EmitBarrier(ir, use_subgroup_barrier);
}

static bool SearchDivergence(const IR::Value& root, const NodeSet& uniform_read_lanes) {
    // BreadthFirstSearch visits each instruction once across all operand paths. A ReadFirstLane
    // or a uniform-block ReadLane with immediate lane index produces a wave-uniform value (after
    // LowerWave64BallotPass lowers the uniform ReadLane to a cross-half broadcast), so we return
    // false to prune its value operand instead of traversing into a lane-varying source.
    return IR::BreadthFirstSearch(root, [&](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
                   return false;
               }
               if (inst->GetOpcode() == IR::Opcode::ReadLane && inst->Arg(1).IsImmediate() &&
                   uniform_read_lanes.contains(inst->GetParent())) {
                   return false;
               }
               if (IsLaneVaryingSource(*inst)) {
                   return true;
               }
               if (IsLoadShared(*inst)) {
                   // Treat shared memory loads as potentially lane-varying if their offset depends
                   // on lane ID; the BFS will naturally traverse into inst->Arg(0) (the offset)
                   // when we return nullopt here.
                   return std::nullopt;
               }
               return std::nullopt;
           }) == true;
}

static bool IsDivergent(const IR::U1& cond, const NodeSet& uniform_blocks) {
    return SearchDivergence(cond, uniform_blocks);
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth,
                                    const NodeSet& uniform_blocks, bool use_subgroup_barrier) {
    const IR::U1 cond = data.if_node.cond;
    if (IsDivergent(cond, uniform_blocks)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge, use_subgroup_barrier);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list,
                                  const NodeSet& uniform_blocks) {
    using Type = IR::AbstractSyntaxNode::Type;
    NodeSet divergent_loops;
    u32 cond_divergence_depth{};
    std::vector<bool> cond_stack;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool div = IsDivergent(node.data.if_node.cond, uniform_blocks);
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
        case Type::Repeat:
            if (IsDivergent(node.data.repeat.cond, uniform_blocks) || cond_divergence_depth > 0) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case Type::Break:
            // A Break inside a divergent If is divergent even when its own cond is immediate true.
            if (IsDivergent(node.data.break_node.cond, uniform_blocks) ||
                cond_divergence_depth > 0) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// When the guest workgroup size is > 64, GCN shaders must emit explicit s_barrier between waves,
// UNLESS the shader uses each wave64's slice of LDS completely independently (no inter-wave LDS
// sharing). On a wave32 host each guest wave64 is split into two subgroups that still share LDS
// without an explicit s_barrier, so we must insert barriers even when threadgroup_size > 64.
//
// When each guest wave fits entirely within a single host subgroup (e.g. threadgroup_size <= 32 on
// a wave32 host, or profile.subgroup_size == 64), there is no cross-subgroup split of a guest
// wave; only a SubgroupBarrier is needed to prevent the host compiler from reordering a subgroup's
// own shared memory writes and reads across lanes. Using a workgroup-wide OpControlBarrier in a
// multi-wave shader that never had s_barrier is unsafe if control flow diverges between waves (for
// example via a per-wave atomic work queue or per-wave buffer load).
static bool GuestWavesFitHostSubgroups(u32 threadgroup_size, const Profile& profile) {
    return profile.subgroup_size >= GcnSubgroupSize || threadgroup_size <= profile.subgroup_size;
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (shared_memory_size == 0 || !profile.needs_lds_barriers) {
        return;
    }
    const bool use_subgroup_barrier = GuestWavesFitHostSubgroups(threadgroup_size, profile);
    const auto uniform_vec = FindWave64UniformBlocks(program);
    const NodeSet uniform_blocks(uniform_vec.begin(), uniform_vec.end());
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list, uniform_blocks);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth, uniform_blocks,
                                    use_subgroup_barrier);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge, use_subgroup_barrier);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block, use_subgroup_barrier);
        }
    }
}

} // namespace Shader::Optimization
