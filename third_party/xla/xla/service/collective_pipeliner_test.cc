/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/collective_pipeliner.h"

#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_matchers.h"
#include "xla/service/hlo_parser.h"
#include "xla/service/hlo_pass_pipeline.h"
#include "xla/statusor.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/util.h"

namespace xla {
namespace {

using ::testing::_;
namespace op = xla::testing::opcode_matchers;

class CollectivePipelinerTest : public HloTestBase {
 public:
  CollectivePipelinerTest() {
    const int64_t kNumReplicas = 4;
    const int64_t kNumComputations = 2;
    config_ = GetModuleConfigForTest(/*replica_count=*/kNumReplicas,
                                     /*num_partitions=*/kNumComputations);
  }

 protected:
  const HloPredicate IsAllGather = HloPredicateIsOp<HloOpcode::kAllGather>;
  HloModuleConfig config_;
};

StatusOr<bool> RunOptimizer(
    HloModule* module, bool last_run, int64_t level_to_operate_on = 0,
    bool pipeline_use_tree = false, bool process_different_sized_ops = true,
    CollectivePipeliner::PipeliningDirection direction =
        CollectivePipeliner::PipeliningDirection::kForward,
    HloPredicate should_process = HloPredicateIsOp<HloOpcode::kAllReduce>) {
  CollectivePipeliner::Config config = {
      /*level_to_operate_on=*/level_to_operate_on,
      /*max_pipelining_per_loop=*/INT64_MAX,
      /*last_run=*/last_run,
      /*pipeline_use_tree=*/pipeline_use_tree,
      /*process_different_sized_ops=*/process_different_sized_ops,
      /*direction=*/
      direction,
      /*should_process=*/should_process,
  };
  HloPassPipeline pass("optimizer");
  pass.AddPass<HloVerifier>(/*layout_sensitive=*/false,
                            /*allow_mixed_precision=*/false);
  pass.AddPass<CollectivePipeliner>(config);
  pass.AddPass<HloVerifier>(/*layout_sensitive=*/false,
                            /*allow_mixed_precision=*/false);
  return pass.Run(module);
}

TEST_F(CollectivePipelinerTest, TransformIncrementIndexByOne) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true).value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(root, op::DynamicUpdateSlice(_, op::AllReduce(), _, _, _));
  const HloInstruction* sliced = root->operand(1)->operand(0);
  EXPECT_EQ(sliced->opcode(), HloOpcode::kDynamicSlice);
  const HloInstruction* index = sliced->operand(1);
  EXPECT_EQ(index->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(index->tuple_index(), 3);
  const HloInstruction* while_inst = index->operand(0);
  EXPECT_EQ(while_inst->opcode(), HloOpcode::kWhile);
  const HloInstruction* while_root =
      while_inst->while_body()->root_instruction();
  EXPECT_EQ(while_root->opcode(), HloOpcode::kTuple);
  const HloInstruction* dyn_upd = while_root->operand(1);
  EXPECT_EQ(dyn_upd->opcode(), HloOpcode::kDynamicUpdateSlice);
  const HloInstruction* dyn_upd2 = dyn_upd->operand(0);
  EXPECT_EQ(dyn_upd2->opcode(), HloOpcode::kDynamicUpdateSlice);
  const HloInstruction* prev_ar = dyn_upd2->operand(1);
  EXPECT_EQ(prev_ar->opcode(), HloOpcode::kAllReduce);
  const HloInstruction* dyn_slice_top = prev_ar->operand(0);
  EXPECT_EQ(dyn_slice_top->opcode(), HloOpcode::kDynamicSlice);
  const HloInstruction* get_tuple_value = dyn_slice_top->operand(0);
  const HloInstruction* get_tuple_index = dyn_slice_top->operand(1);
  EXPECT_EQ(get_tuple_value->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(get_tuple_index->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(get_tuple_value->tuple_index(), 1);
  EXPECT_EQ(get_tuple_index->tuple_index(), 3);
}

TEST_F(CollectivePipelinerTest, TransformIncrementIndexByOneNotFirstIdx) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[8,3,128], bf16[8,3,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[8,3,128], bf16[8,3,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[8,3,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[8,3,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[8,1,128] dynamic-slice(get-tuple-element.5, constant.2561, select.1348, constant.2561), dynamic_slice_sizes={8,1,128}
  mul = bf16[8,1,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[8,1,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[8,3,128] dynamic-update-slice(get-tuple-element.395, ar.1, constant.2561, select.1348, constant.2561)
  ROOT tuple = (s32[], bf16[8,3,128], bf16[8,3,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[8,3,128] parameter(0)
  tuple = (s32[], bf16[8,3,128], bf16[8,3,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[8,3,128], bf16[8,3,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[8,3,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true).value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(root, op::DynamicUpdateSlice(_, op::AllReduce(), _, _, _));
  const HloInstruction* sliced = root->operand(1)->operand(0);
  EXPECT_EQ(sliced->opcode(), HloOpcode::kDynamicSlice);
  const HloInstruction* index = sliced->operand(2);
  EXPECT_EQ(index->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(index->tuple_index(), 3);
  const HloInstruction* while_inst = index->operand(0);
  EXPECT_EQ(while_inst->opcode(), HloOpcode::kWhile);
  const HloInstruction* while_root =
      while_inst->while_body()->root_instruction();
  EXPECT_EQ(while_root->opcode(), HloOpcode::kTuple);
  const HloInstruction* dyn_upd = while_root->operand(1);
  EXPECT_EQ(dyn_upd->opcode(), HloOpcode::kDynamicUpdateSlice);
  const HloInstruction* dyn_upd2 = dyn_upd->operand(0);
  EXPECT_EQ(dyn_upd2->opcode(), HloOpcode::kDynamicUpdateSlice);
  const HloInstruction* prev_ar = dyn_upd2->operand(1);
  EXPECT_EQ(prev_ar->opcode(), HloOpcode::kAllReduce);
  const HloInstruction* dyn_slice_top = prev_ar->operand(0);
  EXPECT_EQ(dyn_slice_top->opcode(), HloOpcode::kDynamicSlice);
  const HloInstruction* get_tuple_value = dyn_slice_top->operand(0);
  const HloInstruction* get_tuple_index = dyn_slice_top->operand(2);
  EXPECT_EQ(get_tuple_value->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(get_tuple_index->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(get_tuple_value->tuple_index(), 1);
  EXPECT_EQ(get_tuple_index->tuple_index(), 3);
}

TEST_F(CollectivePipelinerTest, TransformIncrementByTwo) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(2)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)

  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true).value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(root, op::DynamicUpdateSlice(_, op::AllReduce(), _, _, _));
  const HloInstruction* sliced = root->operand(1)->operand(0);
  EXPECT_EQ(sliced->opcode(), HloOpcode::kDynamicSlice);
  const HloInstruction* index = sliced->operand(1);
  EXPECT_EQ(index->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(index->tuple_index(), 3);
  const HloInstruction* while_inst = index->operand(0);
  EXPECT_EQ(while_inst->opcode(), HloOpcode::kWhile);
  const HloInstruction* while_root =
      while_inst->while_body()->root_instruction();
  EXPECT_EQ(while_root->opcode(), HloOpcode::kTuple);
  const HloInstruction* dyn_upd = while_root->operand(1);
  EXPECT_EQ(dyn_upd->opcode(), HloOpcode::kDynamicUpdateSlice);
  const HloInstruction* dyn_upd2 = dyn_upd->operand(0);
  EXPECT_EQ(dyn_upd2->opcode(), HloOpcode::kDynamicUpdateSlice);
  const HloInstruction* prev_ar = dyn_upd2->operand(1);
  EXPECT_EQ(prev_ar->opcode(), HloOpcode::kAllReduce);
  const HloInstruction* dyn_slice_top = prev_ar->operand(0);
  EXPECT_EQ(dyn_slice_top->opcode(), HloOpcode::kDynamicSlice);
  const HloInstruction* get_tuple_value = dyn_slice_top->operand(0);
  const HloInstruction* get_tuple_index = dyn_slice_top->operand(1);
  EXPECT_EQ(get_tuple_value->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(get_tuple_index->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(get_tuple_value->tuple_index(), 1);
  EXPECT_EQ(get_tuple_index->tuple_index(), 3);
}

TEST_F(CollectivePipelinerTest, NoTransformCantProveIndexDoesntWrap) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(4)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(-1)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_FALSE(RunOptimizer(module.get(), /*last_run=*/true).value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest, TransformNegativeIndexIterationToZero) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(0)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(-3)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/false).value());
  XLA_VLOG_LINES(1, module->ToString());
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(root, op::DynamicUpdateSlice(
                        _,
                        op::CustomCall(op::AllReduce(op::DynamicSlice(
                                           op::GetTupleElement(op::While()),
                                           op::GetTupleElement(),
                                           op::Constant(), op::Constant())),
                                       op::Constant()),
                        op::GetTupleElement(), op::Constant(), op::Constant()));
}

TEST_F(CollectivePipelinerTest, EscapedInputNoTransform) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[1,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(0)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[1,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=3
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.911 = bf16[1,8,128] dynamic-slice(get-tuple-element.395, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.395, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[1,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, dynamic-slice.911, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(-3)
  p0 = bf16[3,8,128] parameter(0)
  cc = bf16[] constant(0)
  c1 = bf16[1,8,128] broadcast(cc), dimensions={}
  c2 = bf16[3,8,128] broadcast(cc), dimensions={}
  tuple = (s32[], bf16[3,8,128], bf16[1,8,128], bf16[3,8,128]) tuple(c0, p0, c1, c2)
  while = (s32[], bf16[3,8,128], bf16[1,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  XLA_VLOG_LINES(1, module->ToString());
  EXPECT_FALSE(RunOptimizer(module.get(), /*last_run=*/true).value());
}

TEST_F(CollectivePipelinerTest, TransformWithAg) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(0)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  rs.1 = bf16[1,1,128] reduce-scatter(mul), replica_groups={}, to_apply=add, channel_id=1, dimensions={1}
  ag.1 = bf16[1,8,128] all-gather(rs.1), replica_groups={}, channel_id=2, dimensions={1}
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ag.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(-3)
  p0 = bf16[3,8,128] parameter(0)
  cc = bf16[] constant(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(root, op::DynamicUpdateSlice(
                        _, op::AllGather(op::GetTupleElement(op::While())),
                        op::GetTupleElement(), op::Constant(), op::Constant()));
}

TEST_F(CollectivePipelinerTest, TransformWithAgWithFormatting) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,9,128], bf16[3,9,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(0)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,9,128], bf16[3,9,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,9,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,9,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,9,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,9,128}
  mul = bf16[1,9,128] multiply(dynamic-slice.99, dynamic-slice.99)
  cpd = bf16[] constant(0)
  %pd = bf16[1,16,128] pad(mul, cpd), padding=0_0x0_7x0_0
  rs.1 = bf16[1,2,128] reduce-scatter(pd), replica_groups={}, to_apply=add, channel_id=1, dimensions={1}
  ag.1 = bf16[1,16,128] all-gather(rs.1), replica_groups={}, channel_id=2, dimensions={1}
  slc = bf16[1,9,128] slice(ag.1), slice={[0:1], [0:9], [0:128]}
  dynamic-update-slice.35 = bf16[3,9,128] dynamic-update-slice(get-tuple-element.395, slc, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,9,128], bf16[3,9,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(-3)
  p0 = bf16[3,9,128] parameter(0)
  cc = bf16[] constant(0)
  tuple = (s32[], bf16[3,9,128], bf16[3,9,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,9,128], bf16[3,9,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,9,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(root,
              op::DynamicUpdateSlice(
                  _, op::Slice(op::AllGather(op::GetTupleElement(op::While()))),
                  op::GetTupleElement(), op::Constant(), op::Constant()));
}

TEST_F(CollectivePipelinerTest, TransformWithAgInsertCustomCall) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(0)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  constant.2561 = s32[] constant(0)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, get-tuple-element.394, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  rs.1 = bf16[1,1,128] reduce-scatter(mul), replica_groups={}, to_apply=add, channel_id=1, dimensions={1}
  ag.1 = bf16[1,8,128] all-gather(rs.1), replica_groups={}, channel_id=2, dimensions={1}
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ag.1, get-tuple-element.394, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(-8)
  p0 = bf16[3,8,128] parameter(0)
  cc = bf16[] constant(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/false, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  RunOptimizer(module.get(), /*last_run=*/true, 1).value();
  XLA_VLOG_LINES(1, module->ToString());
  auto* root = module->entry_computation()->root_instruction();
  // Matching the pattern we expect for the output of the loop when an
  // all-gather is pipelined through the loop. We dynamic-slice the stacked
  // data, perform the all-gather and then put it in the stacked data again.
  EXPECT_THAT(root, op::DynamicUpdateSlice(
                        _, op::AllGather(op::GetTupleElement(op::While())),
                        op::GetTupleElement(), op::Constant(), op::Constant()));
}

TEST_F(CollectivePipelinerTest, PushAgOver) {
  constexpr absl::string_view hlo_string = R"(
HloModule module, entry_computation_layout={(bf16[3,8,128]{2,1,0})->bf16[3,8,128]{2,1,0}}

%add (lhs: bf16[], rhs: bf16[]) -> bf16[] {
  %lhs = bf16[] parameter(0)
  %rhs = bf16[] parameter(1)
  ROOT %add = bf16[] add(bf16[] %lhs, bf16[] %rhs)
}

%while_body.clone (loop_peel_param: (s32[], bf16[3,8,128], s32[])) -> (s32[], bf16[3,8,128], s32[]) {
  %loop_peel_param = (s32[], bf16[3,8,128]{2,1,0}, s32[]) parameter(0)
  %get-tuple-element.2 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_param), index=0
  %constant.7 = s32[] constant(1)
  %add.4 = s32[] add(s32[] %get-tuple-element.2, s32[] %constant.7)
  %get-tuple-element.3 = bf16[3,8,128]{2,1,0} get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_param), index=1
  %get-tuple-element.4 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_param), index=2
  %constant.12 = s64[] constant(1)
  %custom-call = s32[] custom-call(s32[] %get-tuple-element.4, s64[] %constant.12), custom_call_target="InsertedByPreviousStep"
  %constant.13 = s32[] constant(0)
  %constant.10 = s32[] constant(0)
  %dynamic-slice.2 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.3, s32[] %custom-call, s32[] %constant.13, s32[] %constant.13), dynamic_slice_sizes={1,8,128}
  %ar.2 = bf16[1,1,128]{2,1,0} reduce-scatter(bf16[1,8,128]{2,1,0} %dynamic-slice.2), channel_id=2, replica_groups={}, to_apply=%add, dimensions={1}
  %ag.2 = bf16[1,8,128]{2,1,0} all-gather(bf16[1,1,128]{2,1,0} %ar.2), channel_id=32, replica_groups={}, dimensions={1}
  %dynamic-update-slice.2 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.3, bf16[1,8,128]{2,1,0} %ag.2, s32[] %custom-call, s32[] %constant.13, s32[] %constant.13)
  %dynamic-slice.1 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.3, s32[] %get-tuple-element.2, s32[] %constant.10, s32[] %constant.10), dynamic_slice_sizes={1,8,128}
  %mul.2 = bf16[1,8,128]{2,1,0} multiply(bf16[1,8,128]{2,1,0} %dynamic-slice.1, bf16[1,8,128]{2,1,0} %dynamic-slice.1)
  %constant.15 = s32[] constant(0)
  %dynamic-update-slice.4 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %dynamic-update-slice.2, bf16[1,8,128]{2,1,0} %mul.2, s32[] %get-tuple-element.2, s32[] %constant.15, s32[] %constant.15)
  ROOT %tuple.3 = (s32[], bf16[3,8,128]{2,1,0}, s32[]) tuple(s32[] %add.4, bf16[3,8,128]{2,1,0} %dynamic-update-slice.4, s32[] %get-tuple-element.2)
}

%while_cond.clone (loop_peel_cond_param: (s32[], bf16[3,8,128], s32[])) -> pred[] {
  %loop_peel_cond_param = (s32[], bf16[3,8,128]{2,1,0}, s32[]) parameter(0)
  %gte.1 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_cond_param), index=0
  %constant.6 = s32[] constant(0)
  ROOT %cmp.1 = pred[] compare(s32[] %gte.1, s32[] %constant.6), direction=LT
}

ENTRY %entry (p0: bf16[3,8,128]) -> bf16[3,8,128] {
  %c0 = s32[] constant(-3)
  %p0 = bf16[3,8,128]{2,1,0} parameter(0)
  %tuple.1 = (s32[], bf16[3,8,128]{2,1,0}) tuple(s32[] %c0, bf16[3,8,128]{2,1,0} %p0)
  %get-tuple-element.0 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}) %tuple.1), index=0
  %constant.0 = s32[] constant(1)
  %constant.4 = s32[] constant(0)
  %add.1 = s32[] add(s32[] %get-tuple-element.0, s32[] %constant.0)
  %get-tuple-element.1 = bf16[3,8,128]{2,1,0} get-tuple-element((s32[], bf16[3,8,128]{2,1,0}) %tuple.1), index=1
  %dynamic-slice.0 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.1, s32[] %get-tuple-element.0, s32[] %constant.4, s32[] %constant.4), dynamic_slice_sizes={1,8,128}
  %mul.1 = bf16[1,8,128]{2,1,0} multiply(bf16[1,8,128]{2,1,0} %dynamic-slice.0, bf16[1,8,128]{2,1,0} %dynamic-slice.0)
  %dynamic-update-slice.0 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.1, bf16[1,8,128]{2,1,0} %mul.1, s32[] %get-tuple-element.0, s32[] %constant.4, s32[] %constant.4)
  %tuple.4 = (s32[], bf16[3,8,128]{2,1,0}, s32[]) tuple(s32[] %add.1, bf16[3,8,128]{2,1,0} %dynamic-update-slice.0, s32[] %get-tuple-element.0)
  %while.1 = (s32[], bf16[3,8,128]{2,1,0}, s32[]) while((s32[], bf16[3,8,128]{2,1,0}, s32[]) %tuple.4), condition=%while_cond.clone, body=%while_body.clone
  %get-tuple-element.6 = bf16[3,8,128]{2,1,0} get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %while.1), index=1
  %get-tuple-element.5 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %while.1), index=2
  %constant.14 = s32[] constant(0)
  %dynamic-slice.3 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.6, s32[] %get-tuple-element.5, s32[] %constant.14, s32[] %constant.14), dynamic_slice_sizes={1,8,128}
  %ar.3 = bf16[1,8,128]{2,1,0} all-reduce(bf16[1,8,128]{2,1,0} %dynamic-slice.3), channel_id=3, replica_groups={}, to_apply=%add
  ROOT %dynamic-update-slice.3 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.6, bf16[1,8,128]{2,1,0} %ar.3, s32[] %get-tuple-element.5, s32[] %constant.14, s32[] %constant.14)
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 1,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  auto* root = module->entry_computation()->root_instruction();
  // Check that the all-gather can be pipelined after we had already a previous
  // round of pipelining performed previously for another op. (in this case
  // AllReduce).
  EXPECT_THAT(
      root,
      op::DynamicUpdateSlice(
          op::DynamicUpdateSlice(_, op::AllGather(), _, _, _),
          op::AllReduce(op::DynamicSlice(op::DynamicUpdateSlice(), _, _, _)),
          op::GetTupleElement(), op::Constant(), op::Constant()));
}

TEST_F(CollectivePipelinerTest, NoPushAgOverBecauseDifferentSize) {
  constexpr absl::string_view hlo_string = R"(
HloModule module, entry_computation_layout={(bf16[3,8,128]{2,1,0})->bf16[3,8,128]{2,1,0}}

%add (lhs: bf16[], rhs: bf16[]) -> bf16[] {
  %lhs = bf16[] parameter(0)
  %rhs = bf16[] parameter(1)
  ROOT %add = bf16[] add(bf16[] %lhs, bf16[] %rhs)
}

%while_body.clone (loop_peel_param: (s32[], bf16[3,8,128], s32[])) -> (s32[], bf16[3,8,128], s32[]) {
  %loop_peel_param = (s32[], bf16[3,8,128]{2,1,0}, s32[]) parameter(0)
  %get-tuple-element.2 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_param), index=0
  %constant.7 = s32[] constant(1)
  %add.4 = s32[] add(s32[] %get-tuple-element.2, s32[] %constant.7)
  %get-tuple-element.3 = bf16[3,8,128]{2,1,0} get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_param), index=1
  %get-tuple-element.4 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_param), index=2
  %constant.12 = s64[] constant(1)
  %custom-call = s32[] custom-call(s32[] %get-tuple-element.4, s64[] %constant.12), custom_call_target="InsertedByPreviousStep"
  %constant.13 = s32[] constant(0)
  %constant.10 = s32[] constant(0)
  %dynamic-slice.2 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.3, s32[] %custom-call, s32[] %constant.13, s32[] %constant.13), dynamic_slice_sizes={1,8,128}
  %ar.2 = bf16[1,1,128]{2,1,0} reduce-scatter(bf16[1,8,128]{2,1,0} %dynamic-slice.2), channel_id=2, replica_groups={}, to_apply=%add, dimensions={1}
  %ag.2 = bf16[1,8,128]{2,1,0} all-gather(bf16[1,1,128]{2,1,0} %ar.2), channel_id=32, replica_groups={}, dimensions={1}
  %dynamic-update-slice.2 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.3, bf16[1,8,128]{2,1,0} %ag.2, s32[] %custom-call, s32[] %constant.13, s32[] %constant.13)
  %dynamic-slice.1 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.3, s32[] %get-tuple-element.2, s32[] %constant.10, s32[] %constant.10), dynamic_slice_sizes={1,8,128}
  %mul.2 = bf16[1,8,128]{2,1,0} multiply(bf16[1,8,128]{2,1,0} %dynamic-slice.1, bf16[1,8,128]{2,1,0} %dynamic-slice.1)
  %constant.15 = s32[] constant(0)
  %dynamic-update-slice.4 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %dynamic-update-slice.2, bf16[1,8,128]{2,1,0} %mul.2, s32[] %get-tuple-element.2, s32[] %constant.15, s32[] %constant.15)
  ROOT %tuple.3 = (s32[], bf16[3,8,128]{2,1,0}, s32[]) tuple(s32[] %add.4, bf16[3,8,128]{2,1,0} %dynamic-update-slice.4, s32[] %get-tuple-element.2)
}

%while_cond.clone (loop_peel_cond_param: (s32[], bf16[3,8,128], s32[])) -> pred[] {
  %loop_peel_cond_param = (s32[], bf16[3,8,128]{2,1,0}, s32[]) parameter(0)
  %gte.1 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %loop_peel_cond_param), index=0
  %constant.6 = s32[] constant(0)
  ROOT %cmp.1 = pred[] compare(s32[] %gte.1, s32[] %constant.6), direction=LT
}

ENTRY %entry (p0: bf16[3,8,128]) -> bf16[3,8,128] {
  %c0 = s32[] constant(-3)
  %p0 = bf16[3,8,128]{2,1,0} parameter(0)
  %tuple.1 = (s32[], bf16[3,8,128]{2,1,0}) tuple(s32[] %c0, bf16[3,8,128]{2,1,0} %p0)
  %get-tuple-element.0 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}) %tuple.1), index=0
  %constant.0 = s32[] constant(1)
  %constant.4 = s32[] constant(0)
  %add.1 = s32[] add(s32[] %get-tuple-element.0, s32[] %constant.0)
  %get-tuple-element.1 = bf16[3,8,128]{2,1,0} get-tuple-element((s32[], bf16[3,8,128]{2,1,0}) %tuple.1), index=1
  %dynamic-slice.0 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.1, s32[] %get-tuple-element.0, s32[] %constant.4, s32[] %constant.4), dynamic_slice_sizes={1,8,128}
  %mul.1 = bf16[1,8,128]{2,1,0} multiply(bf16[1,8,128]{2,1,0} %dynamic-slice.0, bf16[1,8,128]{2,1,0} %dynamic-slice.0)
  %dynamic-update-slice.0 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.1, bf16[1,8,128]{2,1,0} %mul.1, s32[] %get-tuple-element.0, s32[] %constant.4, s32[] %constant.4)
  %tuple.4 = (s32[], bf16[3,8,128]{2,1,0}, s32[]) tuple(s32[] %add.1, bf16[3,8,128]{2,1,0} %dynamic-update-slice.0, s32[] %get-tuple-element.0)
  %while.1 = (s32[], bf16[3,8,128]{2,1,0}, s32[]) while((s32[], bf16[3,8,128]{2,1,0}, s32[]) %tuple.4), condition=%while_cond.clone, body=%while_body.clone
  %get-tuple-element.6 = bf16[3,8,128]{2,1,0} get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %while.1), index=1
  %get-tuple-element.5 = s32[] get-tuple-element((s32[], bf16[3,8,128]{2,1,0}, s32[]) %while.1), index=2
  %constant.14 = s32[] constant(0)
  %dynamic-slice.3 = bf16[1,8,128]{2,1,0} dynamic-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.6, s32[] %get-tuple-element.5, s32[] %constant.14, s32[] %constant.14), dynamic_slice_sizes={1,8,128}
  %ar.3 = bf16[1,8,128]{2,1,0} all-reduce(bf16[1,8,128]{2,1,0} %dynamic-slice.3), channel_id=3, replica_groups={}, to_apply=%add
  ROOT %dynamic-update-slice.3 = bf16[3,8,128]{2,1,0} dynamic-update-slice(bf16[3,8,128]{2,1,0} %get-tuple-element.6, bf16[1,8,128]{2,1,0} %ar.3, s32[] %get-tuple-element.5, s32[] %constant.14, s32[] %constant.14)
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_FALSE(RunOptimizer(module.get(), /*last_run=*/false, 1,
                            /*pipeline_use_tree=*/false,
                            /*process_different_sized_ops=*/false,
                            CollectivePipeliner::PipeliningDirection::kForward,
                            IsAllGather)
                   .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest, TransformIncrementByTwoFormat) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,16,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,16,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.396 = bf16[3,16,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(2)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,16,128] dynamic-slice(get-tuple-element.396, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,16,128}
  mul = bf16[1,16,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,16,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  ds.1 = bf16[1,8,128] dynamic-slice(ar.1, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ds.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,16,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.396)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,16,128] parameter(0)
  c1 = bf16[] constant(0)
  b1 = bf16[3,8,128] broadcast(c1), dimensions={}
  tuple = (s32[], bf16[3,8,128], bf16[3,16,128]) tuple(c0, b1, p0)
  while = (s32[], bf16[3,8,128], bf16[3,16,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true).value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(
      root,
      op::DynamicUpdateSlice(
          _, op::DynamicSlice(op::AllReduce(op::GetTupleElement()), _, _, _), _,
          _, _));
}

TEST_F(CollectivePipelinerTest, TransformIncrementByTwoFormatTranspose) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,16,128], bf16[3,16,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,16,128], bf16[3,16,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,16,128] get-tuple-element(param), index=1
  get-tuple-element.396 = bf16[3,16,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(2)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,16,128] dynamic-slice(get-tuple-element.396, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,16,128}
  mul = bf16[1,16,128] multiply(dynamic-slice.99, dynamic-slice.99)
  reshape.1 = bf16[2,16,64] reshape(mul)
  ar.1 = bf16[2,16,64] all-reduce(reshape.1), replica_groups={}, to_apply=add, channel_id=1
  transpose.1 = bf16[64,2,16] transpose(ar.1), dimensions={2,0,1}
  reshape.2 = bf16[1,16,128] reshape(transpose.1)
  dynamic-update-slice.35 = bf16[3,16,128] dynamic-update-slice(get-tuple-element.395, reshape.2, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,16,128], bf16[3,16,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.396)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,16,128] parameter(0)
  c1 = bf16[] constant(0)
  b1 = bf16[3,16,128] broadcast(c1), dimensions={}
  tuple.1 = (s32[], bf16[3,16,128], bf16[3,16,128]) tuple(c0, b1, p0)
  while = (s32[], bf16[3,16,128], bf16[3,16,128]) while(tuple.1), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,16,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true).value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(
      root,
      op::DynamicUpdateSlice(
          _, op::Reshape(op::Transpose(op::AllReduce(op::GetTupleElement()))),
          _, _, _));
}

TEST_F(CollectivePipelinerTest, TransformIncrementIndexByOneBackwards) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.k = bf16[3,1,2,128] get-tuple-element(param), index=2
  constant.2561 = s32[] constant(0)
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.k = bf16[1,1,2,128] dynamic-slice(get-tuple-element.k, select.1348, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,1,2,128}
  r = bf16[1,2,128] reshape(dynamic-slice.k)
  a = bf16[1,2,128] add(r, r), control-predecessors={constant.2559}
  ag = bf16[1,8,128] all-gather(a), dimensions={1}, replica_groups={}
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.395, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, ag)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.k), control-predecessors={a}
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  p1 = bf16[3,1,2,128] parameter(1)
  tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128]) tuple(c0, p0, p1)
  while = (s32[], bf16[3,8,128], bf16[3,1,2,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/false,
                           CollectivePipeliner::PipeliningDirection::kBackward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  const int64_t while_count = absl::c_count_if(
      module->entry_computation()->instructions(),
      [](const HloInstruction* instruction) {
        return HloPredicateIsOp<HloOpcode::kWhile>(instruction);
      });
  EXPECT_EQ(while_count, 1);
  const HloInstruction* while_instr =
      FindInstruction(module.get(), HloOpcode::kWhile);
  const HloInstruction* tuple = while_instr->operand(0);
  EXPECT_TRUE(tuple->HasControlDependencies());
  EXPECT_EQ(tuple->control_predecessors().size(), 1);
  const HloInstruction* add_instr = tuple->control_predecessors()[0];
  EXPECT_EQ(add_instr->opcode(), HloOpcode::kAdd);
  const HloComputation* comp = while_instr->while_body();
  const HloInstruction* root_loop = comp->root_instruction();
  EXPECT_TRUE(root_loop->HasControlDependencies());
  EXPECT_EQ(root_loop->control_predecessors().size(), 1);
  const HloInstruction* add_instr_loop = root_loop->control_predecessors()[0];
  EXPECT_EQ(add_instr_loop->opcode(), HloOpcode::kAdd);
}

TEST_F(CollectivePipelinerTest,
       TransformIncrementIndexByOneBackwardsModifyOut) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.k = bf16[3,1,2,128] get-tuple-element(param), index=2
  constant.2561 = s32[] constant(0)
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.k = bf16[1,1,2,128] dynamic-slice(get-tuple-element.k, select.1348, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,1,2,128}
  r = bf16[1,2,128] reshape(dynamic-slice.k)
  a = bf16[1,2,128] add(r, r)
  ag = bf16[1,8,128] all-gather(a), dimensions={1}, replica_groups={}
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.395, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, ag)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  constant.10 = bf16[] constant(0)
  b = bf16[3,1,2,128] broadcast(constant.10), dimensions={}
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128]) tuple(add.230, dynamic-update-slice.35, b)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  p1 = bf16[3,1,2,128] parameter(1)
  tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128]) tuple(c0, p0, p1)
  while = (s32[], bf16[3,8,128], bf16[3,1,2,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_FALSE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                            /*pipeline_use_tree=*/false,
                            /*process_different_sized_ops=*/false,
                            CollectivePipeliner::PipeliningDirection::kBackward,
                            IsAllGather)
                   .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest,
       TransformIncrementIndexByOneBackwardsPlusForward) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.k = bf16[3,1,2,128] get-tuple-element(param), index=2
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=3
  constant.2561 = s32[] constant(0)
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.k = bf16[1,1,2,128] dynamic-slice(get-tuple-element.k, select.1348, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,1,2,128}
  r = bf16[1,2,128] reshape(dynamic-slice.k)
  a = bf16[1,2,128] add(r, r)
  ag = bf16[1,8,128] all-gather(a), dimensions={1}, replica_groups={}
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, ag)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.k, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  p1 = bf16[3,1,2,128] parameter(1)
  tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128], bf16[3,8,128]) tuple(c0, p0, p1, p0)
  while = (s32[], bf16[3,8,128], bf16[3,1,2,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/false, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kBackward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest,
       TransformIncrementIndexByOneBackwardsPlusForwardConvertOutput) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], f32[3,8,128], bf16[3,1,2,128], f32[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], f32[3,8,128], bf16[3,1,2,128], f32[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = f32[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.k = bf16[3,1,2,128] get-tuple-element(param), index=2
  get-tuple-element.5 = f32[3,8,128] get-tuple-element(param), index=3
  constant.2561 = s32[] constant(0)
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.k = bf16[1,1,2,128] dynamic-slice(get-tuple-element.k, select.1348, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,1,2,128}
  r = bf16[1,2,128] reshape(dynamic-slice.k)
  a = bf16[1,2,128] add(r, r)
  ag = bf16[1,8,128] all-gather(a), dimensions={1}, replica_groups={}
  dynamic-slice.99 = f32[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  cvt.0 = bf16[1,8,128] convert(dynamic-slice.99)
  mul = bf16[1,8,128] multiply(cvt.0, ag)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  cvt.1 = f32[1,8,128] convert(ar.1)
  dynamic-update-slice.35 = f32[3,8,128] dynamic-update-slice(get-tuple-element.395, cvt.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], f32[3,8,128], bf16[3,1,2,128], f32[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.k, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = f32[3,8,128] parameter(0)
  p1 = bf16[3,1,2,128] parameter(1)
  tuple = (s32[], f32[3,8,128], bf16[3,1,2,128], f32[3,8,128]) tuple(c0, p0, p1, p0)
  while = (s32[], f32[3,8,128], bf16[3,1,2,128], f32[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = f32[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/false, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kBackward,
                           IsAllGather)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest, MultiUsesElementwise) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  c2 = bf16[] constant(2.0)
  bc = bf16[1,8,128] broadcast(c2)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  mul2 = bf16[1,8,128] multiply(ar.1, bc)
  mul3 = bf16[1,8,128] multiply(mul2, ar.1)
  mul4 = bf16[1,8,128] multiply(mul3, mul)
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, mul4, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/true,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest, ElementWiseUser) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  mul2 = bf16[1,8,128] multiply(ar.1, mul)
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, mul2, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/true,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest, TransformIncrementIndexByOneNotFirstIdxSink) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.35 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.35, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  %c = bf16[] custom-call(), custom_call_target="Boh"
  %b = bf16[1,8,128] broadcast(c), dimensions={}
  %a = bf16[1,8,128] add(ar.1, b)
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, a, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.35), control-predecessors={select.1348}
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true,
                           /*level_to_operate_on=*/0,
                           /*pipeline_use_tree=*/true,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::kForwardSink)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* while_instr =
      FindInstruction(module.get(), HloOpcode::kWhile);
  const HloComputation* comp = while_instr->while_body();
  const HloInstruction* root_loop = comp->root_instruction();
  EXPECT_TRUE(root_loop->HasControlDependencies());
  EXPECT_EQ(root_loop->control_predecessors().size(), 1);
  const HloInstruction* select_instr_loop =
      root_loop->control_predecessors()[0];
  EXPECT_EQ(select_instr_loop->opcode(), HloOpcode::kSelect);
}

TEST_F(CollectivePipelinerTest,
       TransformIncrementIndexByOneNotFirstIdxSinkCustomCall) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.35 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.35, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  %c = bf16[] custom-call(), custom_call_target="Boh"
  %b = bf16[1,8,128] broadcast(c), dimensions={}
  %a = bf16[1,8,128] add(ar.1, b)
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, a, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.35)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/false,
                           /*level_to_operate_on=*/0,
                           /*pipeline_use_tree=*/true,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::kForwardSink)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
  const HloInstruction* all_reduce = module->entry_computation()
                                         ->root_instruction()
                                         ->operand(0)
                                         ->operand(1)
                                         ->operand(0)
                                         ->operand(0);
  EXPECT_EQ(all_reduce->opcode(), HloOpcode::kAllReduce);
  EXPECT_EQ(all_reduce->shape().dimensions(0), 3);
}

// Checks that we shouldn't pipeline Send/Recv by accident while pipelining
// other collective, such as all-gather. In the test, the chain leading to
// all-gather contains Recv/Recv-done, which prevents us from pipelining the
// all-gather backward.
TEST_F(CollectivePipelinerTest, NotTransformAllGatherWithRecvInChainBackwards) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,1,2,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.k = bf16[3,1,2,128] get-tuple-element(param), index=2
  constant.2561 = s32[] constant(0)
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)

  after-all = token[] after-all()
  recv = (bf16[1,1,2,128], u32[], token[]) recv(after-all), channel_id=2, frontend_attributes={
      _xla_send_recv_source_target_pairs="{{0, 1}, {1, 2}, {2, 3}, {3, 4}}"
    }
  send = (bf16[1,1,2,128], u32[], token[]) send(get-tuple-element.k, after-all), channel_id=2, frontend_attributes={
      _xla_send_recv_source_target_pairs="{{0, 1}, {1, 2}, {2, 3}, {3, 4}}"
    }
  send-done = token[] send-done(send), channel_id=2
  recv-done = (bf16[1,1,2,128], token[]) recv-done(recv), channel_id=2
  recv-data = bf16[1,1,2,128] get-tuple-element(recv-done), index=0

  dynamic-slice.k = bf16[1,1,2,128] dynamic-slice(recv-data, select.1348, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,1,2,128}
  r = bf16[1,2,128] reshape(dynamic-slice.k)
  a = bf16[1,2,128] add(r, r)
  ag = bf16[1,8,128] all-gather(a), dimensions={1}, replica_groups={}
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.395, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, ag)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, ar.1, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.k)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  p1 = bf16[3,1,2,128] parameter(1)
  tuple = (s32[], bf16[3,8,128], bf16[3,1,2,128]) tuple(c0, p0, p1)
  while = (s32[], bf16[3,8,128], bf16[3,1,2,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_FALSE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                            /*pipeline_use_tree=*/false,
                            /*process_different_sized_ops=*/false,
                            CollectivePipeliner::PipeliningDirection::kBackward,
                            IsAllGather)
                   .value());
}

TEST_F(CollectivePipelinerTest, TransformRecvSendBackwards) {
  constexpr absl::string_view hlo_string = R"(
  HloModule module
  cond {
    param = (u32[], f32[1, 1024, 1024]) parameter(0)
    count = get-tuple-element(%param), index=0
    ub = u32[] constant(25)
    ROOT result = pred[] compare(count, ub), direction=LT
  }

  body {
    param = (u32[], f32[1, 1024, 1024]) parameter(0)
    count = get-tuple-element(%param), index=0
    p = get-tuple-element(%param), index=1
    c1 = u32[] constant(1)
    new_count = u32[] add(count, c1)

    after-all = token[] after-all()
    recv = (f32[1, 1024, 1024], u32[], token[]) recv(after-all), channel_id=1, frontend_attributes={
      _xla_send_recv_source_target_pairs="{{0, 1}, {1, 2}, {2, 3}, {3, 4}}"
    }
    send = (f32[1, 1024, 1024], u32[], token[]) send(p, after-all), channel_id=1, frontend_attributes={
      _xla_send_recv_source_target_pairs="{{0, 1}, {1, 2}, {2, 3}, {3, 4}}"
    }
    recv-done = (f32[1, 1024, 1024], token[]) recv-done(recv), channel_id=1
    recv-data = f32[1, 1024, 1024] get-tuple-element(recv-done), index=0

    replica = u32[] replica-id()
    c10 = u32[] constant(10)
    sum = u32[] add(replica, c10)
    sum2 = u32[] add(sum, count)
    conv = f32[] convert(sum2)
    b = f32[1, 1024, 1024] add(p, recv-data)
    c = f32[1, 1024, 1024] multiply(b, b)
    d = f32[1, 1024, 1024] tan(c)
    s = f32[1, 1024, 1024] dot(c, d), lhs_batch_dims={0}, lhs_contracting_dims={1}, rhs_batch_dims={0}, rhs_contracting_dims={1}

    send-done = token[] send-done(send), channel_id=1
    ROOT result = (u32[], f32[1, 1024, 1024]) tuple(new_count, s)
  }

  ENTRY test_computation {
    c0 = u32[] constant(0)
    f0 = f32[] constant(0.0)
    init = f32[1, 1024, 1024] broadcast(f0), dimensions={}
    while_init = (u32[], f32[1, 1024, 1024]) tuple(c0, init)
    while_result = (u32[], f32[1, 1024, 1024]) while(while_init), body=body, condition=cond, backend_config="{\"known_trip_count\":{\"n\":\"25\"}}"
    ROOT result = f32[1, 1024, 1024] get-tuple-element(while_result), index=1
  }
  )";

  auto should_pipeline = [](const HloInstruction* instruction) {
    if (!HloPredicateIsOp<HloOpcode::kRecvDone>(instruction)) return false;
    const HloRecvDoneInstruction* recv_done =
        dynamic_cast<const HloRecvDoneInstruction*>(instruction);
    if (recv_done->is_host_transfer()) return false;
    // Check that the recv-done is used for non-trivial computation, which can
    // also help avoid repeatedly pipelining a loop.
    return (recv_done->user_count() == 1 && recv_done->parent() != nullptr &&
            recv_done->users()[0] != recv_done->parent()->root_instruction());
  };
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/false,
                           /*process_different_sized_ops=*/false,
                           CollectivePipeliner::PipeliningDirection::kBackward,
                           should_pipeline)
                  .value());
  XLA_VLOG_LINES(10, module->ToString());
  auto recv1 =
      DynCast<HloRecvInstruction>(FindInstruction(module.get(), "recv.1"));
  EXPECT_NE(recv1, nullptr);
  auto recv2 =
      DynCast<HloRecvInstruction>(FindInstruction(module.get(), "recv.2"));
  EXPECT_NE(recv2, nullptr);
  EXPECT_EQ(recv1->channel_id(), recv2->channel_id());

  auto send1 =
      DynCast<HloSendInstruction>(FindInstruction(module.get(), "send.1"));
  EXPECT_NE(send1, nullptr);
  auto send2 =
      DynCast<HloSendInstruction>(FindInstruction(module.get(), "send.2"));
  EXPECT_NE(send2, nullptr);
  EXPECT_EQ(send1->channel_id(), send2->channel_id());

  EXPECT_EQ(recv1->channel_id(), send1->channel_id());
}

TEST_F(CollectivePipelinerTest, MultiUsesElementwiseMerge) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  c2 = bf16[] constant(2.0)
  bc = bf16[1,8,128] broadcast(c2)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  ar.2 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  mul2 = bf16[1,8,128] multiply(ar.1, bc)
  mul3 = bf16[1,8,128] multiply(mul2, ar.2)
  mul4 = bf16[1,8,128] multiply(mul3, mul)
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, mul4, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5)
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/true,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
}

TEST_F(CollectivePipelinerTest, MultiUsesElementwiseFeedTwo) {
  constexpr absl::string_view hlo_string = R"(
HloModule module

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,8,128], bf16[3,8,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,8,128] get-tuple-element(param), index=1
  get-tuple-element.5 = bf16[3,8,128] get-tuple-element(param), index=2
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  constant.2561 = s32[] constant(0)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.99 = bf16[1,8,128] dynamic-slice(get-tuple-element.5, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,8,128}
  mul = bf16[1,8,128] multiply(dynamic-slice.99, dynamic-slice.99)
  c2 = bf16[] constant(2.0)
  bc = bf16[1,8,128] broadcast(c2)
  ar.1 = bf16[1,8,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  ar.2 = bf16[1,8,128] all-reduce(ar.1), replica_groups={}, to_apply=add, channel_id=1
  mul2 = bf16[1,8,128] multiply(ar.1, bc), control-predecessors={ar.1}
  mul3 = bf16[1,8,128] multiply(mul2, ar.2)
  mul4 = bf16[1,8,128] multiply(mul3, mul)
  dynamic-update-slice.35 = bf16[3,8,128] dynamic-update-slice(get-tuple-element.395, mul4, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.5), control-predecessors={ar.1}
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,8,128] parameter(0)
  tuple = (s32[], bf16[3,8,128], bf16[3,8,128]) tuple(c0, p0, p0)
  while = (s32[], bf16[3,8,128], bf16[3,8,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,8,128] get-tuple-element(while), index=1
}
)";
  auto module = ParseAndReturnUnverifiedModule(hlo_string, config_).value();
  EXPECT_TRUE(RunOptimizer(module.get(), /*last_run=*/true, 0,
                           /*pipeline_use_tree=*/true,
                           /*process_different_sized_ops=*/true,
                           CollectivePipeliner::PipeliningDirection::kForward)
                  .value());
  XLA_VLOG_LINES(1, module->ToString());
}

}  // namespace
}  // namespace xla
