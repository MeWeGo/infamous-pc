// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace {

struct Wave64TestFixture : testing::Test {
    Common::ObjectPool<Shader::IR::Inst> inst_pool{256};
    Common::ObjectPool<Shader::IR::Block> block_pool{16};
    Shader::IR::Block block{inst_pool};
    Shader::IR::IREmitter ir{block};
};

TEST_F(Wave64TestFixture, ImmediateIsUniform) {
    const Shader::IR::U32 imm = ir.Imm32(42u);
    EXPECT_FALSE(Shader::Optimization::ReachesLaneVaryingSource(imm, [](const Shader::IR::Inst&) {
        return false;
    }));
}

TEST_F(Wave64TestFixture, LaneIdIsVarying) {
    const Shader::IR::U32 lane = ir.LaneId();
    EXPECT_TRUE(Shader::Optimization::ReachesLaneVaryingSource(lane, [](const Shader::IR::Inst&) {
        return false;
    }));
}

TEST_F(Wave64TestFixture, ReadFirstLanePruning) {
    const Shader::IR::U32 lane = ir.LaneId();
    const Shader::IR::U32 rfl = ir.ReadFirstLane(lane);

    // Without treating ReadFirstLane as uniform boundary, lane is reached -> varying
    EXPECT_TRUE(Shader::Optimization::ReachesLaneVaryingSource(rfl, [](const Shader::IR::Inst&) {
        return false;
    }));

    // With ReadFirstLane as leaf boundary, search stops and returns false (uniform)
    EXPECT_FALSE(Shader::Optimization::ReachesLaneVaryingSource(rfl, [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::ReadFirstLane;
    }));
}

TEST_F(Wave64TestFixture, OrderIndependenceExhaustiveSearch) {
    // Both expressions contain one ReadFirstLane and one LaneId.
    // The previous BFS stopped at the first visited leaf (rightmost operand first),
    // which caused the uniformity check to depend on operand order.
    // ReachesLaneVaryingSource must report true (varying) for both orders!
    const Shader::IR::U32 lane = ir.LaneId();
    const Shader::IR::U32 rfl = ir.ReadFirstLane(lane);

    const Shader::IR::U32 add_rfl_first = ir.IAdd(rfl, lane);
    const Shader::IR::U32 add_lane_first = ir.IAdd(lane, rfl);

    auto is_uniform_leaf = [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::ReadFirstLane;
    };

    EXPECT_TRUE(Shader::Optimization::ReachesLaneVaryingSource(add_rfl_first, is_uniform_leaf));
    EXPECT_TRUE(Shader::Optimization::ReachesLaneVaryingSource(add_lane_first, is_uniform_leaf));
}

TEST_F(Wave64TestFixture, ReadFirstLaneWithImmediateIsUniform) {
    const Shader::IR::U32 lane = ir.LaneId();
    const Shader::IR::U32 rfl = ir.ReadFirstLane(lane);
    const Shader::IR::U32 uniform_sum = ir.IAdd(rfl, ir.Imm32(100u));

    auto is_uniform_leaf = [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::ReadFirstLane;
    };

    EXPECT_FALSE(Shader::Optimization::ReachesLaneVaryingSource(uniform_sum, is_uniform_leaf));
}

TEST_F(Wave64TestFixture, ReductionIdentityFoldsInactiveLane) {
    Shader::Info info{};
    info.hw_stage = Shader::HwStage::Compute;
    Shader::IR::Program program{info};
    program.blocks.push_back(&block);

    // ReadLane reading lane 40 on a <=32-thread workgroup
    const Shader::IR::U32 val = ir.LaneId();
    const Shader::IR::U32 read_lane = ir.ReadLane(val, ir.Imm32(40u));
    // Consumed by IAdd32
    const Shader::IR::U32 sum = ir.IAdd(read_lane, ir.Imm32(10u));

    Shader::RuntimeInfo runtime_info{};
    runtime_info.hw.cs.workgroup_size = {32, 1, 1};

    Shader::Profile profile{};
    profile.subgroup_size = 32;

    Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile);

    // After the pass, the ReadLane instruction should be removed and folded to Imm32(0) (identity for IAdd32)
    // The sum should now use Imm32(0)
    EXPECT_TRUE(sum.Inst()->Arg(0).IsImmediate());
    EXPECT_EQ(sum.Inst()->Arg(0).U32(), 0u);
}

TEST_F(Wave64TestFixture, SharedMemoryBarrierSubgroupVsWorkgroup) {
    // Test 1: threadgroup_size <= 32 on a 32-lane host produces SubgroupBarrier
    {
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Compute;

        Shader::IR::Block test_block{inst_pool};
        Shader::IR::IREmitter test_ir{test_block};

        // Shared write followed by shared read in the same block
        test_ir.WriteShared(32, test_ir.Imm32(1u), test_ir.Imm32(0u));
        const auto load_res = test_ir.LoadShared(32, false, test_ir.Imm32(0u));
        (void)load_res;

        Shader::IR::Program program{info};
        program.blocks.push_back(&test_block);
        Shader::IR::AbstractSyntaxNode node{};
        node.type = Shader::IR::AbstractSyntaxNode::Type::Block;
        node.data.block = &test_block;
        program.syntax_list.push_back(node);

        Shader::RuntimeInfo runtime_info{};
        runtime_info.hw.cs.workgroup_size = {32, 1, 1};
        runtime_info.hw.cs.shared_memory_size = 1024;

        Shader::Profile profile{};
        profile.subgroup_size = 32;
        profile.needs_lds_barriers = true;

        Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);

        bool found_subgroup_barrier = false;
        bool found_workgroup_barrier = false;
        for (const auto& inst : test_block.Instructions()) {
            if (inst.GetOpcode() == Shader::IR::Opcode::SubgroupBarrier) {
                found_subgroup_barrier = true;
            }
            if (inst.GetOpcode() == Shader::IR::Opcode::Barrier) {
                found_workgroup_barrier = true;
            }
        }
        EXPECT_TRUE(found_subgroup_barrier);
        EXPECT_FALSE(found_workgroup_barrier);
    }

    // Test 2: threadgroup_size = 128 on a 32-lane host (guest wave64 split across 2 host subgroups)
    // must produce a workgroup Barrier
    {
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Compute;

        Shader::IR::Block test_block{inst_pool};
        Shader::IR::IREmitter test_ir{test_block};

        test_ir.WriteShared(32, test_ir.Imm32(1u), test_ir.Imm32(0u));
        const auto load_res = test_ir.LoadShared(32, false, test_ir.Imm32(0u));
        (void)load_res;

        Shader::IR::Program program{info};
        program.blocks.push_back(&test_block);
        Shader::IR::AbstractSyntaxNode node{};
        node.type = Shader::IR::AbstractSyntaxNode::Type::Block;
        node.data.block = &test_block;
        program.syntax_list.push_back(node);

        Shader::RuntimeInfo runtime_info{};
        runtime_info.hw.cs.workgroup_size = {128, 1, 1};
        runtime_info.hw.cs.shared_memory_size = 1024;

        Shader::Profile profile{};
        profile.subgroup_size = 32;
        profile.needs_lds_barriers = true;

        Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);

        bool found_subgroup_barrier = false;
        bool found_workgroup_barrier = false;
        for (const auto& inst : test_block.Instructions()) {
            if (inst.GetOpcode() == Shader::IR::Opcode::SubgroupBarrier) {
                found_subgroup_barrier = true;
            }
            if (inst.GetOpcode() == Shader::IR::Opcode::Barrier) {
                found_workgroup_barrier = true;
            }
        }
        EXPECT_FALSE(found_subgroup_barrier);
        EXPECT_TRUE(found_workgroup_barrier);
    }
}

} // namespace
