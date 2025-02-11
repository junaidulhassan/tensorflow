/* Copyright 2024 The OpenXLA Authors.

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
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"

#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "absl/status/status.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/AsmParser/AsmParser.h"  // from @llvm-project
#include "mlir/Dialect/Affine/IR/AffineOps.h"  // from @llvm-project
#include "mlir/Dialect/DLTI/DLTI.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/Math/IR/Math.h"  // from @llvm-project
#include "mlir/Dialect/SCF/IR/SCF.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/fusions/mlir/ir/xla_gpu_ops.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/hlo_parser.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/status_macros.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/tests/filecheck.h"
#include "xla/tests/hlo_test_base.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace mlir_converter {
namespace {

class ElementalHloToMlirTest : public HloTestBase {
 public:
  ElementalHloToMlirTest() {
    context_.loadDialect<mlir::tensor::TensorDialect, mlir::func::FuncDialect,
                         mlir::affine::AffineDialect, mlir::arith::ArithDialect,
                         mlir::math::MathDialect, mlir::scf::SCFDialect,
                         mlir::mhlo::MhloDialect, mlir::LLVM::LLVMDialect,
                         mlir::DLTIDialect, xla::gpu::XlaGpuDialect>();
  }

  // Converts the root subgraph of the entry function of the given hlo module to
  // MLIR.
  absl::Status Run(const std::string& hlo, const std::string& filecheck_str,
                   std::function<EpilogueSpecification(HloComputation* entry)>
                       epilogue_spec_fn = nullptr) {
    auto hlo_module = ParseAndReturnVerifiedModule(hlo).value();

    mlir::ImplicitLocOpBuilder builder(mlir::UnknownLoc::get(&context_),
                                       &context_);
    auto module = llvm_ir::CreateMlirModuleOp(builder.getLoc());
    (*module)->setAttr(
        mlir::DLTIDialect::kDataLayoutAttrName,
        mlir::parseAttribute("#dlti.dl_spec<#dlti.dl_entry<index,32:i32>>",
                             builder.getContext()));
    builder.setInsertionPointToStart(module->getBody());
    auto* entry_computation = hlo_module->entry_computation();
    std::optional<EpilogueSpecification> epilogue_spec = std::nullopt;
    if (epilogue_spec_fn) {
      epilogue_spec = epilogue_spec_fn(entry_computation);
    }
    PartitionedComputations partitioned_computations(entry_computation,
                                                     &context_, epilogue_spec);
    auto fns = partitioned_computations.DeclareFunctions(module.get());
    auto entry_func = fns[&partitioned_computations
                               .FindPartitionedComputation(entry_computation)
                               .GetRootSubgraph()];
    auto& entry_pc =
        partitioned_computations.FindPartitionedComputation(entry_computation);
    auto call_targets = partitioned_computations.CreateCallTargetProvider(fns);
    TF_RETURN_IF_ERROR(SubgraphToMlirFunction(
        entry_pc, entry_pc.GetRootSubgraph(), entry_func, call_targets));

    if (const auto& epilogue = partitioned_computations.epilogue()) {
      TF_RETURN_IF_ERROR(SubgraphToMlirFunction(entry_pc, *epilogue,
                                                fns[&*epilogue], call_targets));
    }

    // Canonicalize and CSE for better readability of check tests.
    mlir::PassManager pm(&context_);
    pm.addPass(mlir::createCanonicalizerPass());
    pm.addPass(mlir::createCSEPass());
    TF_RET_CHECK(pm.run(module.get()).succeeded());

    std::string out;
    llvm::raw_string_ostream stream(out);
    stream << module.get();

    TF_ASSIGN_OR_RETURN(auto filecheck_result,
                        RunFileCheck(out, filecheck_str));
    TF_RET_CHECK(filecheck_result);
    return absl::OkStatus();
  }

  mlir::MLIRContext context_;
};

TEST_F(ElementalHloToMlirTest, Reduce) {
  TF_EXPECT_OK(Run(R"(
    add {
      p0 = f32[] parameter(0)
      p1 = f32[] parameter(1)
      ROOT sum = f32[] add(p0, p1)
    }

    ENTRY main {
      p0 = f32[10,20,30,40] parameter(0)
      p1 = f32[] parameter(1)
      ROOT r = f32[10,30] reduce(p0, p1), dimensions={1,3},
                                          to_apply=add
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:   %[[ARG0:.*]]: tensor<10x20x30x40xf32>
    // CHECK-SAME:   %[[ARG1:.*]]: tensor<f32>
    // CHECK-SAME:   %[[X:.*]]: index {{.*}}, %[[Y:.*]]: index {{.*}} -> f32
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1
    // CHECK-DAG:  %[[C20:.*]] = arith.constant 20
    // CHECK-DAG:  %[[C40:.*]] = arith.constant 40
    // CHECK:      %[[INIT:.*]] = tensor.extract %[[ARG1]][]
    // CHECK:      %[[RET:.*]] = scf.for %[[I:.*]] = %[[C0]] to %[[C20]]
    // CHECK-SAME:   step %[[C1]] iter_args(%[[ACC:.*]] = %[[INIT]])
    // CHECK:        %[[RET_INNER:.*]] = scf.for %[[J:.*]] = %[[C0]] to %[[C40]]
    // CHECK-SAME:     iter_args(%[[ACC_INNER:.*]] = %[[ACC]])
    // CHECK:          %[[VAL:.*]] = tensor.extract %[[ARG0]]
    // CHECK-SAME:        [%[[X]], %[[I]], %[[Y]], %[[J]]]
    // CHECK:          %[[UPD:.*]] = func.call @add_sum(%[[ACC_INNER]],
    // CHECK-SAME:                                      %[[VAL]])
    // CHECK:          scf.yield %[[UPD]]
    // CHECK:        }
    // CHECK:        scf.yield %[[RET_INNER]]
    // CHECK:      }
    // CHECK:      return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, ReduceUnsigned) {
  TF_EXPECT_OK(Run(R"(
    add {
      p0 = u32[] parameter(0)
      p1 = u32[] parameter(1)
      ROOT sum = u32[] add(p0, p1)
    }

    ENTRY main {
      p0 = u32[10,20,30,40] parameter(0)
      p1 = u32[] parameter(1)
      ROOT r = u32[10,30] reduce(p0, p1), dimensions={1,3},
                                          to_apply=add
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:   %[[ARG0:.*]]: tensor<10x20x30x40xui32>
    // CHECK-SAME:   %[[ARG1:.*]]: tensor<ui32>
    // CHECK-SAME:   %[[X:.*]]: index {{.*}}, %[[Y:.*]]: index {{.*}} -> ui32
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1
    // CHECK-DAG:  %[[C20:.*]] = arith.constant 20
    // CHECK-DAG:  %[[C40:.*]] = arith.constant 40
    // CHECK:      %[[INIT:.*]] = tensor.extract %[[ARG1]][]
    // CHECK:      %[[RET:.*]] = scf.for %[[I:.*]] = %[[C0]] to %[[C20]]
    // CHECK-SAME:   step %[[C1]] iter_args(%[[ACC:.*]] = %[[INIT]])
    // CHECK:        %[[RET_INNER:.*]] = scf.for %[[J:.*]] = %[[C0]] to %[[C40]]
    // CHECK-SAME:     iter_args(%[[ACC_INNER:.*]] = %[[ACC]])
    // CHECK:          %[[VAL:.*]] = tensor.extract %[[ARG0]]
    // CHECK-SAME:        [%[[X]], %[[I]], %[[Y]], %[[J]]]
    // CHECK:          %[[UPD:.*]] = func.call @add_sum(%[[ACC_INNER]],
    // CHECK-SAME:                                      %[[VAL]])
    // CHECK:          scf.yield %[[UPD]]
    // CHECK:        }
    // CHECK:        scf.yield %[[RET_INNER]]
    // CHECK:      }
    // CHECK:      return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, ReduceWindow) {
  TF_EXPECT_OK(Run(R"(
    add {
      p0 = f32[] parameter(0)
      p1 = f32[] parameter(1)
      ROOT sum = f32[] add(p0, p1)
    }

    ENTRY main {
      p0 = f32[42,12,8] parameter(0)
      p1 = f32[] parameter(1)
      ROOT r = f32[42,3,8] reduce-window(p0, p1), window={
                                                size=1x1x7
                                                stride=1x4x1
                                                pad=0_0x0_0x3_3
                                               },
                                               to_apply=add
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:   %[[ARG0:.*]]: tensor<42x12x8xf32>
    // CHECK-SAME:   %[[ARG1:.*]]: tensor<f32>
    // CHECK-SAME:   %[[X:arg[0-9]*]]: index {{[^}]*}}},
    // CHECK-SAME:   %[[Y:arg[0-9]*]]: index {{[^}]*}}},
    // CHECK-SAME:   %[[Z:arg[0-9]*]]: index {{[^}]*}}}) -> f32
    // CHECK-DAG:  %[[C10:.*]] = arith.constant 10
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1
    // CHECK-DAG:  %[[C7:.*]] = arith.constant 7
    // CHECK:      %[[INIT:.*]] = tensor.extract %[[ARG1]][]
    // CHECK:      %[[RET:.*]] = scf.for %[[I:.*]] = %[[C0]] to %[[C7]]
    // CHECK-SAME:   step %[[C1]] iter_args(%[[ACC:.*]] = %[[INIT]])
    // CHECK:      %[[J:.*]] = affine.apply affine_map<()[s0] ->
    // CHECK-SAME: (s0 * 4)>()[%[[Y]]]
    // CHECK:      %[[K:.*]] = affine.apply affine_map<()[s0, s1] ->
    // CHECK-SAME: (s0 + s1 - 3)>()[%[[I]], %[[Z]]]
    // CHECK:          %[[VAL:.*]] = tensor.extract %[[ARG0]]
    // CHECK-SAME:        [%[[X]], %[[J]], %[[K]]]
    // CHECK:          %[[UPD:.*]] = func.call @add_sum(%[[ACC]],
    // CHECK-SAME:                                      %[[VAL]])
    // CHECK:          scf.yield %[[UPD]]
    // CHECK:        }
    // CHECK:      }
    // CHECK:      return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, ReduceWindowWithRescaling) {
  TF_EXPECT_OK(Run(R"(
    add {
      p0 = f32[] parameter(0)
      p1 = f32[] parameter(1)
      ROOT sum = f32[] add(p0, p1)
    }

    ENTRY main {
      p0 = f32[42,12,8] parameter(0)
      p1 = f32[] parameter(1)
      ROOT r = f32[19,12,8] reduce-window(p0, p1), window={
                                                size=8x1x1
                                                stride=4x1x1
                                                pad=0_0x0_0x0_0
                                                lhs_dilate=2x1x1
                                               },
                                               to_apply=add
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:   %[[ARG0:.*]]: tensor<42x12x8xf32>
    // CHECK-SAME:   %[[ARG1:.*]]: tensor<f32>
    // CHECK-SAME:   %[[X:arg[0-9]*]]: index {{[^}]*}}},
    // CHECK-SAME:   %[[Y:arg[0-9]*]]: index {{[^}]*}}},
    // CHECK-SAME:   %[[Z:arg[0-9]*]]: index {{[^}]*}}}) -> f32
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C4:.*]] = arith.constant 4 : index

    // We have a window size of 8, but expect a loop from 0 to 4
    // due to the base dilation of 2 and the applied symbol rescaling:
    // CHECK:      scf.for %[[I:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK:      %[[K:.*]] = affine.apply affine_map<()[s0, s1] ->
    // If symbol rescaling wasn't working we would have a
    // `s0 floordiv <base_dilation>` in the map:
    // CHECK-SAME: (s0 + s1 * 2)>()[%[[I]], %[[X]]]
    // CHECK:      tensor.extract %[[ARG0]][%[[K]], %[[Y]], %[[Z]]]
  )"));
}

TEST_F(ElementalHloToMlirTest, Concatenate) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[10,20,30] parameter(0)
      p1 = f32[10,15,30] parameter(1)
      p2 = f32[10,3,30] parameter(2)
      ROOT r = f32[10,38,30] concatenate(p0, p1, p2), dimensions={1}
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<10x20x30xf32>,
    // CHECK-SAME:     %[[ARG1:.*]]: tensor<10x15x30xf32>,
    // CHECK-SAME:     %[[ARG2:.*]]: tensor<10x3x30xf32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}},
    // CHECK-SAME:     %[[Z:.*]]: index {{{.*}}}
    // CHECK-DAG:    %[[C35:.*]] = arith.constant 35
    // CHECK-DAG:    %[[C20:.*]] = arith.constant 20
    // CHECK:        %[[IN_BOUNDS:.*]] = arith.cmpi ult, %[[Y]], %[[C20]]
    // CHECK:        %[[CONCAT:.*]] = scf.if %[[IN_BOUNDS]]
    // CHECK:          %[[P0_VAL:.*]] = xla_gpu.pure_call @main_p0
    // CHECK-SAME:         %[[X]], %[[Y]], %[[Z]]
    // CHECK:          scf.yield %[[P0_VAL]]
    // CHECK:        } else {
    // CHECK:          %[[IN_BOUNDS:.*]] = arith.cmpi ult, %[[Y]], %[[C35]]
    // CHECK:          %[[CONCAT2:.*]] = scf.if %[[IN_BOUNDS]]
    // CHECK:            %[[OFFSET:.*]] = arith.subi %[[Y]], %[[C20]]
    // CHECK:            %[[P1_VAL:.*]] = xla_gpu.pure_call @main_p1
    // CHECK-SAME:           %[[X]], %[[OFFSET]], %[[Z]]
    // CHECK:            scf.yield %[[P1_VAL]]
    // CHECK:          } else {
    // CHECK:            %[[OFFSET:.*]] = arith.subi %[[Y]], %[[C35]]
    // CHECK:            %[[P2_VAL:.*]] = xla_gpu.pure_call @main_p2
    // CHECK-SAME:           %[[X]], %[[OFFSET]], %[[Z]]
    // CHECK:            scf.yield %[[P2_VAL]]
    // CHECK:          }
    // CHECK:          scf.yield %[[CONCAT2]]
    // CHECK:        }
    // CHECK:        return %[[CONCAT]]
  )"));
}

TEST_F(ElementalHloToMlirTest, ConcatenateUnsigned) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = u32[10,20,30] parameter(0)
      p1 = u32[10,15,30] parameter(1)
      ROOT r = u32[10,35,30] concatenate(p0, p1), dimensions={1}
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<10x20x30xui32>,
    // CHECK-SAME:     %[[ARG1:.*]]: tensor<10x15x30xui32>
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}},
    // CHECK-SAME:     %[[Z:.*]]: index {{{.*}}}
    // CHECK-DAG:    %[[C20:.*]] = arith.constant 20
    // CHECK:        %[[IN_BOUNDS:.*]] = arith.cmpi ult, %[[Y]], %[[C20]]
    // CHECK:        %[[CONCAT:.*]] = scf.if %[[IN_BOUNDS]]
    // CHECK:          %[[P0_VAL:.*]] = xla_gpu.pure_call @main_p0
    // CHECK-SAME:         %[[X]], %[[Y]], %[[Z]]
    // CHECK:          %[[CAST0:.*]] = builtin.unrealized_conversion_cast %[[P0_VAL]]
    // CHECK:          scf.yield %[[CAST0]]
    // CHECK:        } else {
    // CHECK:          %[[OFFSET:.*]] = arith.subi %[[Y]], %[[C20]]
    // CHECK:          %[[P1_VAL:.*]] = xla_gpu.pure_call @main_p1
    // CHECK-SAME:         %[[X]], %[[OFFSET]], %[[Z]]
    // CHECK:          %[[CAST1:.*]] = builtin.unrealized_conversion_cast %[[P1_VAL]]
    // CHECK:          scf.yield %[[CAST1]]
    // CHECK:        }
    // CHECK:        %[[CAST2:.*]] = builtin.unrealized_conversion_cast %[[CONCAT]]
    // CHECK:        return %[[CAST2]]
  )"));
}

TEST_F(ElementalHloToMlirTest, Gather) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      operand = f32[33,34] parameter(0)
      indices = s32[1806,1] parameter(1)
      ROOT r = f32[1806,7,8] gather(operand, indices), offset_dims={1,2},
                                 collapsed_slice_dims={}, start_index_map={0},
                                 index_vector_dim=1, slice_sizes={7,8}
    })",
                   R"(
    // CHECK:      @main_r(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<33x34xf32>,
    // CHECK-SAME:     %[[ARG1:.*]]: tensor<1806x1xi32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}},
    // CHECK-SAME:     %[[Z:.*]]: index {{{.*}}}
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:    %[[C26:.*]] = arith.constant 26
    // CHECK:        %[[IDX_I32:.*]] = tensor.extract %[[ARG1]][%[[X]], %[[C0]]]
    // CHECK:        %[[IDX:.*]] = arith.index_cast %[[IDX_I32]] : i32 to index
    // CHECK:        %[[CLAMP_HIGH:.*]] = arith.minsi %[[IDX]], %[[C26]]
    // CHECK:        %[[CLAMPED:.*]] = arith.maxsi %[[CLAMP_HIGH]], %[[C0]]
    // CHECK:        %[[X_IN:.*]] = arith.addi %[[CLAMPED]], %[[Y]]
    // CHECK:        %[[RET:.*]] = tensor.extract %[[ARG0]][%[[X_IN]], %[[Z]]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, Pad) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[4, 4] parameter(0)
      p1 = f32[] parameter(1)
      ROOT pad = f32[12, 16] pad(p0, p1), padding=1_4_1x4_8_0
    })",
                   R"(
    // CHECK:      @main_pad(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4x4xf32>,
    // CHECK-SAME:     %[[ARG1:.*]]: tensor<f32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}}
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4
    // CHECK-DAG:    %[[C7:.*]] = arith.constant 7
    // CHECK:        %[[CONSTRAINT_VAL:.*]] = affine.apply
    // CHECK-SAME:     <()[s0] -> (s0 - ((s0 - 1) floordiv 2) * 2 - 1)>
    // CHECK-SAME:     ()[%[[X]]]
    // CHECK:        %[[CONSTRAINT:.*]] = arith.cmpi eq, %[[CONSTRAINT_VAL]], %[[C0]]
    // CHECK:        %[[X_L:.*]] = arith.cmpi sge, %[[X]], %[[C1]]
    // CHECK:        %[[X_H:.*]] = arith.cmpi sle, %[[X]], %[[C7]]
    // CHECK:        %[[X_BOUNDS:.*]] = arith.andi %[[X_L]], %[[X_H]]
    // CHECK:        %[[X_AND_CONSTRAINT:.*]] = arith.andi %[[CONSTRAINT]], %[[X_BOUNDS]]
    // CHECK:        %[[Y_L:.*]] = arith.cmpi sge, %[[Y]], %[[C4]]
    // CHECK:        %[[Y_H:.*]] = arith.cmpi sle, %[[Y]], %[[C7]]
    // CHECK:        %[[Y_BOUNDS:.*]] = arith.andi %[[Y_L]], %[[Y_H]]
    // CHECK:        %[[FROM_INPUT:.*]] = arith.andi %[[X_AND_CONSTRAINT]], %[[Y_BOUNDS]]
    // CHECK:        %[[RET:.*]] = scf.if %[[FROM_INPUT]]
    // CHECK:          %[[X_IN:.*]] = affine.apply
    // CHECK-SAME:         <()[s0] -> ((s0 - 1) floordiv 2)>()[%[[X]]]
    // CHECK:          %[[Y_IN:.*]] = affine.apply
    // CHECK-SAME:         <()[s0] -> (s0 - 4)>()[%[[Y]]]
    // CHECK:          %[[VAL:.*]] = tensor.extract %[[ARG0]][%[[X_IN]], %[[Y_IN]]]
    // CHECK:          scf.yield %[[VAL]]
    // CHECK:        } else {
    // CHECK:          %[[PAD_VAL:.*]] = tensor.extract %[[ARG1]][]
    // CHECK:          scf.yield %[[PAD_VAL]]
    // CHECK:        }
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, PadUnsigned) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = u32[4, 4] parameter(0)
      p1 = u32[] parameter(1)
      ROOT pad = u32[12, 16] pad(p0, p1), padding=1_4_1x4_8_0
    })",
                   R"(
    // CHECK:      @main_pad(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4x4xui32>,
    // CHECK-SAME:     %[[ARG1:.*]]: tensor<ui32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}}
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4
    // CHECK-DAG:    %[[C7:.*]] = arith.constant 7
    // CHECK:        %[[CONSTRAINT_VAL:.*]] = affine.apply
    // CHECK-SAME:     <()[s0] -> (s0 - ((s0 - 1) floordiv 2) * 2 - 1)>
    // CHECK-SAME:     ()[%[[X]]]
    // CHECK:        %[[CONSTRAINT:.*]] = arith.cmpi eq, %[[CONSTRAINT_VAL]], %[[C0]]
    // CHECK:        %[[X_L:.*]] = arith.cmpi sge, %[[X]], %[[C1]]
    // CHECK:        %[[X_H:.*]] = arith.cmpi sle, %[[X]], %[[C7]]
    // CHECK:        %[[X_BOUNDS:.*]] = arith.andi %[[X_L]], %[[X_H]]
    // CHECK:        %[[X_AND_CONSTRAINT:.*]] = arith.andi %[[CONSTRAINT]], %[[X_BOUNDS]]
    // CHECK:        %[[Y_L:.*]] = arith.cmpi sge, %[[Y]], %[[C4]]
    // CHECK:        %[[Y_H:.*]] = arith.cmpi sle, %[[Y]], %[[C7]]
    // CHECK:        %[[Y_BOUNDS:.*]] = arith.andi %[[Y_L]], %[[Y_H]]
    // CHECK:        %[[FROM_INPUT:.*]] = arith.andi %[[X_AND_CONSTRAINT]], %[[Y_BOUNDS]]
    // CHECK:        %[[RET:.*]] = scf.if %[[FROM_INPUT]]
    // CHECK:          %[[X_IN:.*]] = affine.apply
    // CHECK-SAME:         <()[s0] -> ((s0 - 1) floordiv 2)>()[%[[X]]]
    // CHECK:          %[[Y_IN:.*]] = affine.apply
    // CHECK-SAME:         <()[s0] -> (s0 - 4)>()[%[[Y]]]
    // CHECK:          %[[VAL:.*]] = tensor.extract %[[ARG0]][%[[X_IN]], %[[Y_IN]]]
    // CHECK:          %[[CAST0:.*]] = builtin.unrealized_conversion_cast %[[VAL]]
    // CHECK:          scf.yield %[[CAST0]]
    // CHECK:        } else {
    // CHECK:          %[[PAD_VAL:.*]] = tensor.extract %[[ARG1]][]
    // CHECK:          %[[CAST1:.*]] = builtin.unrealized_conversion_cast %[[PAD_VAL]]
    // CHECK:          scf.yield %[[CAST1]]
    // CHECK:        }
    // CHECK:          %[[CAST2:.*]] = builtin.unrealized_conversion_cast %[[RET]]
    // CHECK:        return %[[CAST2]]
  )"));
}

TEST_F(ElementalHloToMlirTest, DotWithF32Type) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[3, 4] parameter(0)
      p1 = f32[4, 5] parameter(1)
      ROOT dot = f32[3, 5] dot(p0, p1), lhs_contracting_dims={1}, rhs_contracting_dims={0}
    })",
                   R"(
    // CHECK:      @main_dot(
    // CHECK-SAME: %[[A:.*]]: tensor<3x4xf32>, %[[B:.*]]: tensor<4x5xf32>,
    // CHECK-SAME: %[[I:.*]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[J:.*]]: index {xla.range = [0 : index, 4 : index]})
    // CHECK-SAME: -> f32
    // CHECK-SAME: {
    // CHECK-DAG:    %[[ACCUM_INIT:.*]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:    %[[C2:.*]] = arith.constant 2 : index
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4 : index
    // CHECK:        %[[FOR0:.*]] = scf.for %[[K:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK-SAME:   iter_args(%[[ACCUM:.*]] = %[[ACCUM_INIT]]) -> (f32) {
    // CHECK-DAG:      %[[CMPI0:.*]] = arith.cmpi sge, %[[I]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI1:.*]] = arith.cmpi sle, %[[I]], %[[C2]] : index
    // CHECK-DAG:      %[[I_IN_RANGE:.*]] = arith.andi %[[CMPI0]], %[[CMPI1]] : i1
    // CHECK-DAG:      %[[CMPI2:.*]] = arith.cmpi sge, %[[J]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI3:.*]] = arith.cmpi sle, %[[J]], %[[C4]] : index
    // CHECK-DAG:      %[[J_IN_RANGE:.*]] = arith.andi %[[CMPI2]], %[[CMPI3]] : i1
    // CHECK-DAG:      %[[I_J_IN_RANGE:.*]] = arith.andi %[[I_IN_RANGE]], %[[J_IN_RANGE]] : i1
    // CHECK:          %[[IF0:.*]] = scf.if %[[I_J_IN_RANGE]] -> (f32) {
    // CHECK-DAG:        %[[A_I_K:.*]] = tensor.extract %[[A]][%[[I]], %[[K]]] : tensor<3x4xf32>
    // CHECK-DAG:        %[[B_K_J:.*]] = tensor.extract %[[B]][%[[K]], %[[J]]] : tensor<4x5xf32>
    // CHECK-DAG:        %[[MULF0:.*]] = arith.mulf %[[A_I_K]], %[[B_K_J]] : f32
    // CHECK-DAG:        %[[ADDF0:.*]] = arith.addf %[[ACCUM]], %[[MULF0]] : f32
    // CHECK-DAG:        scf.yield %[[ADDF0]] : f32
    // CHECK:          } else {
    // CHECK:            scf.yield %[[ACCUM]] : f32
    // CHECK:          }
    // CHECK:          scf.yield %[[IF0]] : f32
    // CHECK:        }
    // CHECK:        return %[[FOR0]] : f32
    // CHECK:      }
  )"));
}

TEST_F(ElementalHloToMlirTest, DotWithBF16Type) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = bf16[3, 4] parameter(0)
      p1 = bf16[4, 5] parameter(1)
      ROOT dot = bf16[3, 5] dot(p0, p1), lhs_contracting_dims={1}, rhs_contracting_dims={0}
    })",
                   R"(
    // CHECK:      @main_dot(
    // CHECK-SAME: %[[A:.*]]: tensor<3x4xbf16>, %[[B:.*]]: tensor<4x5xbf16>,
    // CHECK-SAME: %[[I:.*]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[J:.*]]: index {xla.range = [0 : index, 4 : index]})
    // CHECK-SAME: -> bf16
    // CHECK-SAME: {
    // CHECK-DAG:    %[[ACCUM_INIT:.*]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:    %[[C2:.*]] = arith.constant 2 : index
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4 : index
    // CHECK:        %[[FOR0:.*]] = scf.for %[[K:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK-SAME:   iter_args(%[[ACCUM:.*]] = %[[ACCUM_INIT]]) -> (f32) {
    // CHECK-DAG:      %[[CMPI0:.*]] = arith.cmpi sge, %[[I]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI1:.*]] = arith.cmpi sle, %[[I]], %[[C2]] : index
    // CHECK-DAG:      %[[I_IN_RANGE:.*]] = arith.andi %[[CMPI0]], %[[CMPI1]] : i1
    // CHECK-DAG:      %[[CMPI2:.*]] = arith.cmpi sge, %[[J]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI3:.*]] = arith.cmpi sle, %[[J]], %[[C4]] : index
    // CHECK-DAG:      %[[J_IN_RANGE:.*]] = arith.andi %[[CMPI2]], %[[CMPI3]] : i1
    // CHECK-DAG:      %[[I_J_IN_RANGE:.*]] = arith.andi %[[I_IN_RANGE]], %[[J_IN_RANGE]] : i1
    // CHECK:          %[[IF0:.*]] = scf.if %[[I_J_IN_RANGE]] -> (f32) {
    // CHECK-DAG:        %[[A_I_K:.*]] = tensor.extract %[[A]][%[[I]], %[[K]]] : tensor<3x4xbf16>
    // CHECK-DAG:        %[[B_K_J:.*]] = tensor.extract %[[B]][%[[K]], %[[J]]] : tensor<4x5xbf16>
    // CHECK-DAG:        %[[A_I_K_F32:.*]] = arith.extf %[[A_I_K]] :  bf16 to f32
    // CHECK-DAG:        %[[B_K_J_F32:.*]] = arith.extf %[[B_K_J]] :  bf16 to f32
    // CHECK-DAG:        %[[MULF0:.*]] = arith.mulf %[[A_I_K_F32]], %[[B_K_J_F32]] : f32
    // CHECK-DAG:        %[[ADDF0:.*]] = arith.addf %[[ACCUM]], %[[MULF0]] : f32
    // CHECK-DAG:        scf.yield %[[ADDF0]] : f32
    // CHECK:          } else {
    // CHECK:            scf.yield %[[ACCUM]] : f32
    // CHECK:          }
    // CHECK:          scf.yield %[[IF0]] : f32
    // CHECK:        }
    // CHECK:        %[[FOR0_BF16:.*]] = arith.truncf %[[FOR0]] : f32 to bf16
    // CHECK:        return %[[FOR0_BF16]] : bf16
    // CHECK:      }
  )"));
}

TEST_F(ElementalHloToMlirTest, DotWithS32Type) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = s32[3, 4] parameter(0)
      p1 = s32[4, 5] parameter(1)
      ROOT dot = s32[3, 5] dot(p0, p1), lhs_contracting_dims={1}, rhs_contracting_dims={0}
    })",
                   R"(
    // CHECK:      @main_dot(
    // CHECK-SAME: %[[A:.*]]: tensor<3x4xi32>, %[[B:.*]]: tensor<4x5xi32>,
    // CHECK-SAME: %[[I:.*]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[J:.*]]: index {xla.range = [0 : index, 4 : index]})
    // CHECK-SAME: -> i32
    // CHECK-SAME: {
    // CHECK-DAG:    %[[ACCUM_INIT:.*]] = arith.constant 0 : i32
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:    %[[C2:.*]] = arith.constant 2 : index
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4 : index
    // CHECK:        %[[FOR0:.*]] = scf.for %[[K:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK-SAME:   iter_args(%[[ACCUM:.*]] = %[[ACCUM_INIT]]) -> (i32) {
    // CHECK-DAG:      %[[CMPI0:.*]] = arith.cmpi sge, %[[I]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI1:.*]] = arith.cmpi sle, %[[I]], %[[C2]] : index
    // CHECK-DAG:      %[[I_IN_RANGE:.*]] = arith.andi %[[CMPI0]], %[[CMPI1]] : i1
    // CHECK-DAG:      %[[CMPI2:.*]] = arith.cmpi sge, %[[J]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI3:.*]] = arith.cmpi sle, %[[J]], %[[C4]] : index
    // CHECK-DAG:      %[[J_IN_RANGE:.*]] = arith.andi %[[CMPI2]], %[[CMPI3]] : i1
    // CHECK-DAG:      %[[I_J_IN_RANGE:.*]] = arith.andi %[[I_IN_RANGE]], %[[J_IN_RANGE]] : i1
    // CHECK:          %[[IF0:.*]] = scf.if %[[I_J_IN_RANGE]] -> (i32) {
    // CHECK-DAG:        %[[A_I_K:.*]] = tensor.extract %[[A]][%[[I]], %[[K]]] : tensor<3x4xi32>
    // CHECK-DAG:        %[[B_K_J:.*]] = tensor.extract %[[B]][%[[K]], %[[J]]] : tensor<4x5xi32>
    // CHECK-DAG:        %[[MUL0:.*]] = arith.muli %[[A_I_K]], %[[B_K_J]] : i32
    // CHECK-DAG:        %[[ADD0:.*]] = arith.addi %[[ACCUM]], %[[MUL0]] : i32
    // CHECK-DAG:        scf.yield %[[ADD0]] : i32
    // CHECK:          } else {
    // CHECK:            scf.yield %[[ACCUM]] : i32
    // CHECK:          }
    // CHECK:          scf.yield %[[IF0]] : i32
    // CHECK:        }
    // CHECK:        return %[[FOR0]] : i32
    // CHECK:      }
  )"));
}

TEST_F(ElementalHloToMlirTest, DotWithU32Type) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = u32[3, 4] parameter(0)
      p1 = u32[4, 5] parameter(1)
      ROOT dot = u32[3, 5] dot(p0, p1), lhs_contracting_dims={1}, rhs_contracting_dims={0}
    })",
                   R"(
    // CHECK:      @main_dot(
    // CHECK-SAME: %[[A:.*]]: tensor<3x4xui32>, %[[B:.*]]: tensor<4x5xui32>,
    // CHECK-SAME: %[[I:.*]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[J:.*]]: index {xla.range = [0 : index, 4 : index]})
    // CHECK-SAME: -> ui32
    // CHECK-SAME: {
    // CHECK-DAG:    %[[ACCUM_INIT:.*]] = arith.constant 0 : i32
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:    %[[C2:.*]] = arith.constant 2 : index
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4 : index
    // CHECK:        %[[FOR0:.*]] = scf.for %[[K:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK-SAME:   iter_args(%[[ACCUM:.*]] = %[[ACCUM_INIT]]) -> (i32) {
    // CHECK-DAG:      %[[CMPI0:.*]] = arith.cmpi sge, %[[I]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI1:.*]] = arith.cmpi sle, %[[I]], %[[C2]] : index
    // CHECK-DAG:      %[[I_IN_RANGE:.*]] = arith.andi %[[CMPI0]], %[[CMPI1]] : i1
    // CHECK-DAG:      %[[CMPI2:.*]] = arith.cmpi sge, %[[J]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI3:.*]] = arith.cmpi sle, %[[J]], %[[C4]] : index
    // CHECK-DAG:      %[[J_IN_RANGE:.*]] = arith.andi %[[CMPI2]], %[[CMPI3]] : i1
    // CHECK-DAG:      %[[I_J_IN_RANGE:.*]] = arith.andi %[[I_IN_RANGE]], %[[J_IN_RANGE]] : i1
    // CHECK:          %[[IF0:.*]] = scf.if %[[I_J_IN_RANGE]] -> (i32) {
    // CHECK-DAG:        %[[A_I_K:.*]] = tensor.extract %[[A]][%[[I]], %[[K]]] : tensor<3x4xui32>
    // CHECK-DAG:        %[[A_I_K_I32:.*]] = builtin.unrealized_conversion_cast %[[A_I_K]] : ui32 to i32
    // CHECK-DAG:        %[[B_K_J:.*]] = tensor.extract %[[B]][%[[K]], %[[J]]] : tensor<4x5xui32>
    // CHECK-DAG:        %[[B_K_J_I32:.*]] = builtin.unrealized_conversion_cast %[[B_K_J]] : ui32 to i32
    // CHECK-DAG:        %[[MUL0:.*]] = arith.muli %[[A_I_K_I32]], %[[B_K_J_I32]] : i32
    // CHECK-DAG:        %[[ADD0:.*]] = arith.addi %[[ACCUM]], %[[MUL0]] : i32
    // CHECK-DAG:        scf.yield %[[ADD0]] : i32
    // CHECK:          } else {
    // CHECK:            scf.yield %[[ACCUM]] : i32
    // CHECK:          }
    // CHECK:          scf.yield %[[IF0]] : i32
    // CHECK:        }
    // CHECK:        %[[FOR0_UI32:.*]] = builtin.unrealized_conversion_cast %[[FOR0]] : i32 to ui32
    // CHECK:        return %[[FOR0_UI32]] : ui32
    // CHECK:      }
  )"));
}

TEST_F(ElementalHloToMlirTest, DotWithPredType) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = pred[3, 4] parameter(0)
      p1 = pred[4, 5] parameter(1)
      ROOT dot = pred[3, 5] dot(p0, p1), lhs_contracting_dims={1}, rhs_contracting_dims={0}
    })",
                   R"(
    // CHECK:      @main_dot(
    // CHECK-SAME: %[[A:.*]]: tensor<3x4xi1>, %[[B:.*]]: tensor<4x5xi1>,
    // CHECK-SAME: %[[I:.*]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[J:.*]]: index {xla.range = [0 : index, 4 : index]})
    // CHECK-SAME: -> i1
    // CHECK-SAME: {
    // CHECK-DAG:    %[[ACCUM_INIT:.*]] = arith.constant false
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:    %[[C2:.*]] = arith.constant 2 : index
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4 : index
    // CHECK:        %[[FOR0:.*]] = scf.for %[[K:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK-SAME:   iter_args(%[[ACCUM:.*]] = %[[ACCUM_INIT]]) -> (i1) {
    // CHECK-DAG:      %[[CMPI0:.*]] = arith.cmpi sge, %[[I]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI1:.*]] = arith.cmpi sle, %[[I]], %[[C2]] : index
    // CHECK-DAG:      %[[I_IN_RANGE:.*]] = arith.andi %[[CMPI0]], %[[CMPI1]] : i1
    // CHECK-DAG:      %[[CMPI2:.*]] = arith.cmpi sge, %[[J]], %[[C0]] : index
    // CHECK-DAG:      %[[CMPI3:.*]] = arith.cmpi sle, %[[J]], %[[C4]] : index
    // CHECK-DAG:      %[[J_IN_RANGE:.*]] = arith.andi %[[CMPI2]], %[[CMPI3]] : i1
    // CHECK-DAG:      %[[I_J_IN_RANGE:.*]] = arith.andi %[[I_IN_RANGE]], %[[J_IN_RANGE]] : i1
    // CHECK:          %[[IF0:.*]] = scf.if %[[I_J_IN_RANGE]] -> (i1) {
    // CHECK-DAG:        %[[A_I_K:.*]] = tensor.extract %[[A]][%[[I]], %[[K]]] : tensor<3x4xi1>
    // CHECK-DAG:        %[[B_K_J:.*]] = tensor.extract %[[B]][%[[K]], %[[J]]] : tensor<4x5xi1>
    // CHECK-DAG:        %[[AND0:.*]] = arith.andi %[[A_I_K]], %[[B_K_J]] : i1
    // CHECK-DAG:        %[[OR0:.*]] = arith.ori %[[ACCUM]], %[[AND0]] : i1
    // CHECK-DAG:        scf.yield %[[OR0]] : i1
    // CHECK:          } else {
    // CHECK:            scf.yield %[[ACCUM]] : i1
    // CHECK:          }
    // CHECK:          scf.yield %[[IF0]] : i1
    // CHECK:        }
    // CHECK:        return %[[FOR0]] : i1
    // CHECK:      }
  )"));
}

TEST_F(ElementalHloToMlirTest, DotWithBatchAnd2ContractingDims) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[7, 3, 4, 5] parameter(0)
      p1 = f32[5, 6, 4, 7] parameter(1)
      ROOT dot = f32[7, 3, 6] dot(p0, p1),
                 lhs_contracting_dims={2, 3}, rhs_contracting_dims={2, 0},
                 lhs_batch_dims={0}, rhs_batch_dims={3}
    })",
                   R"(
    // CHECK:      @main_dot(
    // CHECK-SAME: %[[A:.*]]: tensor<7x3x4x5xf32>, %[[B:.*]]: tensor<5x6x4x7xf32>,
    // CHECK-SAME: %[[N:.*]]: index {xla.range = [0 : index, 6 : index]},
    // CHECK-SAME: %[[I:.*]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[J:.*]]: index {xla.range = [0 : index, 5 : index]})
    // CHECK-SAME: -> f32
    // CHECK-SAME: {
    // CHECK-DAG:    %[[C0F:.*]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:    %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:    %[[C2:.*]] = arith.constant 2 : index
    // CHECK-DAG:    %[[C4:.*]] = arith.constant 4 : index
    // CHECK-DAG:    %[[C5:.*]] = arith.constant 5 : index
    // CHECK-DAG:    %[[C6:.*]] = arith.constant 6 : index
    // CHECK:        %[[FOR0:.*]] = scf.for %[[K:.*]] = %[[C0]] to %[[C4]] step %[[C1]]
    // CHECK-SAME:   iter_args(%[[ACCUM0:.*]] = %[[C0F]]) -> (f32) {
    // CHECK:          %[[FOR1:.*]] = scf.for %[[L:.*]] = %[[C0]] to %[[C5]] step %[[C1]]
    // CHECK-SAME:     iter_args(%[[ACCUM1:.*]] = %[[ACCUM0]]) -> (f32) {
    // CHECK-DAG:        %[[CMPI0:.*]] = arith.cmpi sge, %[[N]], %[[C0]] : index
    // CHECK-DAG:        %[[CMPI1:.*]] = arith.cmpi sle, %[[N]], %[[C6]] : index
    // CHECK-DAG:        %[[N_IN_RANGE:.*]] = arith.andi %[[CMPI0]], %[[CMPI1]] : i1
    // CHECK-DAG:        %[[CMPI2:.*]] = arith.cmpi sge, %[[I]], %[[C0]] : index
    // CHECK-DAG:        %[[CMPI3:.*]] = arith.cmpi sle, %[[I]], %[[C2]] : index
    // CHECK-DAG:        %[[I_IN_RANGE:.*]] = arith.andi %[[CMPI2]], %[[CMPI3]] : i1
    // CHECK-DAG:        %[[N_I_IN_RANGE:.*]] = arith.andi %[[N_IN_RANGE]], %[[I_IN_RANGE]] : i1
    // CHECK-DAG:        %[[CMPI4:.*]] = arith.cmpi sge, %[[J]], %[[C0]] : index
    // CHECK-DAG:        %[[CMPI5:.*]] = arith.cmpi sle, %[[J]], %[[C5]] : index
    // CHECK-DAG:        %[[J_IN_RANGE:.*]] = arith.andi %[[CMPI4]], %[[CMPI5]] : i1
    // CHECK-DAG:        %[[N_I_J_IN_RANGE:.*]] = arith.andi %[[N_I_IN_RANGE]], %[[J_IN_RANGE]] : i1
    // CHECK:            %[[IF0:.*]] = scf.if %[[N_I_J_IN_RANGE]] -> (f32) {
    // CHECK-DAG:          %[[A_N_I_K_L:.*]] = tensor.extract %[[A]][%[[N]], %[[I]], %[[K]], %[[L]]] : tensor<7x3x4x5xf32>
    // CHECK-DAG:          %[[B_L_J_K_N:.*]] = tensor.extract %[[B]][%[[L]], %[[J]], %[[K]], %[[N]]] : tensor<5x6x4x7xf32>
    // CHECK-DAG:          %[[MULF0:.*]] = arith.mulf %[[A_N_I_K_L]], %[[B_L_J_K_N]] : f32
    // CHECK-DAG:          %[[ADDF0:.*]] = arith.addf %[[ACCUM1]], %[[MULF0]] : f32
    // CHECK-DAG:          scf.yield %[[ADDF0]] : f32
    // CHECK:            } else {
    // CHECK:              scf.yield %[[ACCUM1]] : f32
    // CHECK:            }
    // CHECK:            scf.yield %[[IF0]] : f32
    // CHECK:          }
    // CHECK:          scf.yield %[[FOR1]] : f32
    // CHECK:        }
    // CHECK:        return %[[FOR0]] : f32
    // CHECK:      }
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionSimple) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[4,3,5,16] parameter(1)
      ROOT conv = f32[2,6,8,16] convolution(p0, p1), window={size=3x5 pad=0_0x0_0}, dim_labels=b01f_i01o->b01f
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<4x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 1 : index]},
    // CHECK-SAME: %[[W:.+]]: index {xla.range = [0 : index, 5 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 7 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C4:.+]] = arith.constant 4 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A1]]) -> (f32) {
    // CHECK:      %[[R3:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[B]], %[[XX]], %[[YY]], %[[I]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<4x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionWithWindowStrides) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[4,3,5,16] parameter(1)
      ROOT conv = f32[2,3,4,16] convolution(p0, p1), window={size=3x5 stride=2x2 pad=0_0x0_0}, dim_labels=b01f_i01o->b01f
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<4x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 1 : index]},
    // CHECK-SAME: %[[W:.+]]: index {xla.range = [0 : index, 2 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 3 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C4:.+]] = arith.constant 4 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A1]]) -> (f32) {
    // CHECK:      %[[R3:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1 * 2)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1 * 2)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[B]], %[[XX]], %[[YY]], %[[I]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<4x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionWithPadding) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[4,3,5,16] parameter(1)
      ROOT conv = f32[2,8,12,16] convolution(p0, p1), window={size=3x5 pad=1_1x2_2}, dim_labels=b01f_i01o->b01f
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<4x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 1 : index]},
    // CHECK-SAME: %[[W:.+]]: index {xla.range = [0 : index, 7 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 11 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C2:.+]] = arith.constant 2 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C4:.+]] = arith.constant 4 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK-DAG:  %[[C8:.+]] = arith.constant 8 : index
    // CHECK-DAG:  %[[C13:.+]] = arith.constant 13 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A1]]) -> (f32) {
    // CHECK-DAG:  %[[TESTX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[X]], %[[W]]]
    // CHECK-DAG:  %[[TXGE:.+]] = arith.cmpi sge, %[[TESTX]], %[[C1]] : index
    // CHECK-DAG:  %[[TXLE:.+]] = arith.cmpi sle, %[[TESTX]], %[[C8]] : index
    // CHECK-DAG:  %[[TX:.+]] = arith.andi %[[TXGE]], %[[TXLE]] : i1
    // CHECK-DAG:  %[[TESTY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:  %[[TYGE:.+]] = arith.cmpi sge, %[[TESTY]], %[[C2]] : index
    // CHECK-DAG:  %[[TYLE:.+]] = arith.cmpi sle, %[[TESTY]], %[[C13]] : index
    // CHECK-DAG:  %[[TY:.+]] = arith.andi %[[TYGE]], %[[TYLE]] : i1
    // CHECK:      %[[R3:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1 - 1)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1 - 2)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[B]], %[[XX]], %[[YY]], %[[I]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<4x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionWithLhsDilation) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[4,3,5,16] parameter(1)
      ROOT conv = f32[2,13,19,16] convolution(p0, p1), window={size=3x5 pad=0_0x0_0 lhs_dilate=2x2}, dim_labels=b01f_i01o->b01f
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<4x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 1 : index]},
    // CHECK-SAME: %[[W:.+]]: index {xla.range = [0 : index, 12 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 18 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C4:.+]] = arith.constant 4 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A1]]) -> (f32) {
    // CHECK-DAG:  %[[TESTX:.+]] = affine.apply affine_map<()[s0, s1] -> ((s0 + s1) mod 2)>()[%[[X]], %[[W]]]
    // CHECK-DAG:  %[[TX:.+]] = arith.cmpi eq, %[[TESTX]], %[[C0]] : index
    // CHECK-DAG:  %[[TESTY:.+]] = affine.apply affine_map<()[s0, s1] -> ((s0 + s1) mod 2)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:  %[[TY:.+]] = arith.cmpi eq, %[[TESTY]], %[[C0]] : index
    // CHECK:      %[[R3:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> ((s0 + s1) floordiv 2)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> ((s0 + s1) floordiv 2)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[B]], %[[XX]], %[[YY]], %[[I]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<4x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionWithRhsDilation) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[4,3,5,16] parameter(1)
      ROOT conv = f32[2,4,4,16] convolution(p0, p1), window={size=3x5 pad=0_0x0_0 rhs_dilate=2x2}, dim_labels=b01f_i01o->b01f
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<4x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 1 : index]},
    // CHECK-SAME: %[[W:[^ ]+]]: index {xla.range = [0 : index, 3 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 3 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C4:.+]] = arith.constant 4 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A1]]) -> (f32) {
    // CHECK:      %[[R3:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 * 2 + s1)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 * 2 + s1)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[B]], %[[XX]], %[[YY]], %[[I]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<4x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionWithFeatureGroupCount) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[2,3,5,16] parameter(1)
      ROOT conv = f32[2,6,8,16] convolution(p0, p1), window={size=3x5 pad=0_0x0_0}, dim_labels=b01f_i01o->b01f, feature_group_count=2
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<2x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 1 : index]},
    // CHECK-SAME: %[[W:.+]]: index {xla.range = [0 : index, 5 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 7 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C2:.+]] = arith.constant 2 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C2]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A1]]) -> (f32) {
    // CHECK:      %[[R3:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[II:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + (s1 floordiv 8) * 2)>()[%[[I]], %[[O]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[B]], %[[XX]], %[[YY]], %[[II]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<2x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvolutionWithBatchGroupCount) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[2,8,12,4] parameter(0)
      p1 = f32[4,3,5,16] parameter(1)
      ROOT conv = f32[1,6,8,16] convolution(p0, p1), window={size=3x5 pad=0_0x0_0}, dim_labels=b01f_i01o->b01f, batch_group_count=2
    })",
                   R"(
    // CHECK:      @main_conv(
    // CHECK-SAME: %[[LHS:.+]]: tensor<2x8x12x4xf32>, %[[RHS:.*]]: tensor<4x3x5x16xf32>,
    // CHECK-SAME: %[[B:.+]]: index {xla.range = [0 : index, 0 : index]},
    // CHECK-SAME: %[[W:.+]]: index {xla.range = [0 : index, 5 : index]},
    // CHECK-SAME: %[[H:.+]]: index {xla.range = [0 : index, 7 : index]},
    // CHECK-SAME: %[[O:.+]]: index {xla.range = [0 : index, 15 : index]})
    // CHECK-SAME: -> f32
    // CHECK-DAG:  %[[INIT:.+]] = arith.constant 0.000000e+00 : f32
    // CHECK-DAG:  %[[C0:.+]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.+]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C2:.+]] = arith.constant 2 : index
    // CHECK-DAG:  %[[C3:.+]] = arith.constant 3 : index
    // CHECK-DAG:  %[[C4:.+]] = arith.constant 4 : index
    // CHECK-DAG:  %[[C5:.+]] = arith.constant 5 : index
    // CHECK:      %[[R0:.+]] = scf.for %[[X:.+]] = %[[C0]] to %[[C3]] step %[[C1]] iter_args(%[[A0:.+]] = %[[INIT]]) -> (f32) {
    // CHECK-NEXT: %[[R1:.+]] = scf.for %[[Y:.+]] = %[[C0]] to %[[C5]] step %[[C1]] iter_args(%[[A1:.+]] = %[[A0]]) -> (f32) {
    // CHECK-NEXT: %[[R2:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]] iter_args(%[[A2:.+]] = %[[A1]]) -> (f32) {
    // CHECK-NEXT: %[[R3:.+]] = scf.for %[[G:.+]] = %[[C0]] to %[[C2]] step %[[C1]] iter_args(%[[ACC:.+]] = %[[A2]]) -> (f32) {
    // CHECK:      %[[R4:.+]] = scf.if {{.+}} -> (f32) {
    // CHECK-DAG:    %[[BB:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[G]], %[[B]]]
    // CHECK-DAG:    %[[XX:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[X]], %[[W]]]
    // CHECK-DAG:    %[[YY:.+]] = affine.apply affine_map<()[s0, s1] -> (s0 + s1)>()[%[[Y]], %[[H]]]
    // CHECK-DAG:    %[[VL:.+]] = tensor.extract %[[LHS]][%[[BB]], %[[XX]], %[[YY]], %[[I]]] : tensor<2x8x12x4xf32>
    // CHECK-DAG:    %[[VR:.+]] = tensor.extract %[[RHS]][%[[I]], %[[X]], %[[Y]], %[[O]]] : tensor<4x3x5x16xf32>
    // CHECK:        %[[MUL:.+]] = arith.mulf %[[VL]], %[[VR]] : f32
    // CHECK-NEXT:   %[[ADD:.+]] = arith.addf %[[ACC]], %[[MUL]] : f32
    // CHECK-NEXT:   scf.yield %[[ADD]] : f32
    // CHECK-NEXT: } else {
    // CHECK-NEXT:   scf.yield %[[ACC]] : f32
    // CHECK-NEXT: }
    // CHECK-NEXT: scf.yield %[[R4]] : f32
    // CHECK:      scf.yield %[[R3]] : f32
    // CHECK:      scf.yield %[[R2]] : f32
    // CHECK:      scf.yield %[[R1]] : f32
    // CHECK:      return %[[R0]] : f32
  )"));
}

TEST_F(ElementalHloToMlirTest, Transpose) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[4,5,6] parameter(0)
      ROOT transpose = f32[6,5,4] transpose(p0), dimensions={2,1,0}
    })",
                   R"(
    // CHECK:      @main_transpose(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4x5x6xf32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}},
    // CHECK-SAME:     %[[Z:.*]]: index {{{.*}}}
    // CHECK:        %[[RET:.*]] = tensor.extract %[[ARG0]]
    // CHECK-SAME:     [%[[Z]], %[[Y]], %[[X]]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, Broadcast) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[4,5] parameter(0)
      ROOT broadcast = f32[6,4,5] broadcast(p0), dimensions={1,2}
    })",
                   R"(
    // CHECK:      @main_broadcast(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4x5xf32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}},
    // CHECK-SAME:     %[[Z:.*]]: index {{{.*}}}
    // CHECK:        %[[RET:.*]] = tensor.extract %[[ARG0]]
    // CHECK-SAME:     [%[[Y]], %[[Z]]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, Add) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[4] parameter(0)
      p1 = f32[4] parameter(1)
      ROOT add = f32[4] add(p0, p1)
    })",
                   R"(
    // CHECK:      @main_add(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4xf32>, %[[ARG1:.*]]: tensor<4xf32>,
    // CHECK-SAME:     %[[X:.*]]: index {{.*}}
    // CHECK:        %[[A:.*]] = tensor.extract %[[ARG0]][%[[X]]]
    // CHECK:        %[[B:.*]] = tensor.extract %[[ARG1]][%[[X]]]
    // CHECK:        %[[RET:.*]] = arith.addf %[[A]], %[[B]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, Complex) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[4] parameter(0)
      p1 = f32[4] parameter(1)
      ROOT add = c64[4] complex(p0, p1)
    })",
                   R"(
    // CHECK:      @main_add(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4xf32>, %[[ARG1:.*]]: tensor<4xf32>,
    // CHECK-SAME:     %[[X:.*]]: index {{.*}}
    // CHECK:        %[[A:.*]] = tensor.extract %[[ARG0]][%[[X]]]
    // CHECK:        %[[B:.*]] = tensor.extract %[[ARG1]][%[[X]]]
    // CHECK:        %[[RET:.*]] = complex.create %[[A]], %[[B]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, ComplexAbs) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = c64[4] parameter(0)
      ROOT abs = f32[4] abs(p0)
    })",
                   R"(
    // CHECK:      @main_abs(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4xcomplex<f32>>
    // CHECK-SAME:     %[[X:.*]]: index {{.*}}
    // CHECK:        %[[A:.*]] = tensor.extract %[[ARG0]][%[[X]]]
    // CHECK:        %[[RET:.*]] = complex.abs %[[A]] : complex<f32>
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, UnsignedDiv) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = u32[4] parameter(0)
      p1 = u32[4] parameter(1)
      ROOT div = u32[4] divide(p0, p1)
    })",
                   R"(
    // CHECK:      @main_div(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<4xui32>, %[[ARG1:.*]]: tensor<4xui32>,
    // CHECK-SAME:     %[[X:.*]]: index {{.*}}
    // CHECK:        %[[DIV:.*]] = arith.divui %{{.*}}, %{{.*}} : i32
  )"));
}

TEST_F(ElementalHloToMlirTest, ConvertToUnsigned) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[4] parameter(0)
      ROOT convert = u32[4] convert(p0)
    })",
                   R"(
    // CHECK:      @main_convert(
    // CHECK:        arith.fptoui %{{.*}} : f32 to i32
  )"));
}

TEST_F(ElementalHloToMlirTest, PopulationCountUnsigned) {
  TF_EXPECT_OK(Run(R"(
     ENTRY main{
       p0 = u32[10,1,4]{2,1,0} parameter(0)
       ROOT popcnt = u32[10,1,4]{2,1,0} popcnt(p0)
     })",
                   R"(
    // CHECK:      @main_popcnt(
    // CHECK:        builtin.unrealized_conversion_cast %{{.*}} : ui32 to i32
    // CHECK:        math.ctpop %{{.*}} : i32
    // CHECK:        builtin.unrealized_conversion_cast %{{.*}} : i32 to ui32
  )"));
}

TEST_F(ElementalHloToMlirTest, Epilogue) {
  TF_EXPECT_OK(Run(
      R"(
      ENTRY main {
        %p0 = f32[2,16,17] parameter(0)
        %log = f32[2,16,17] log(%p0)
        %transpose = f32[2,17,16] transpose(%log), dimensions={0,2,1}
        %p1 = f32[] parameter(1)
        %bc = f32[2,17,16] broadcast(%p1), dimensions={}
        ROOT %add = f32[2,17,16] add(%transpose, %bc)
      })",
      R"(
      // CHECK:      @main__epilogue__(
      // CHECK-SAME:     %[[ARG0:.*]]: tensor<2x16x17xf32>
      // CHECK-SAME:     %[[ARG1:.*]]: tensor<f32>
      // CHECK-SAME:     %[[X:.*]]: index {xla.range = [0 : index, 1 :
      // CHECK-SAME:     %[[Y:.*]]: index {xla.range = [0 : index, 15 :
      // CHECK-SAME:     %[[Z:.*]]: index {xla.range = [0 : index, 16 :
      // CHECK-SAME:     %[[TRANSPOSE:.*]]: f32) -> f32
      // CHECK:        %[[B:.*]] = tensor.extract %[[ARG1]][]
      // CHECK:        %[[RET:.*]] = arith.addf %[[TRANSPOSE]], %[[B]]
      // CHECK:        return %[[RET]])",
      [this](HloComputation* entry) {
        EpilogueSpecification epilogue;
        epilogue.heroes.push_back(entry->GetInstructionWithName("transpose"));
        epilogue.index_ranges = {2, 16, 17};
        epilogue.root_indexing.push_back(
            mlir::AffineMap::getMultiDimIdentityMap(3, &context_)
                .getSubMap({0, 2, 1}));
        return epilogue;
      }));
}

TEST_F(ElementalHloToMlirTest, ScalarConstant) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      p0 = f32[1,1] parameter(0)
      c1 = f32[1,1] constant({{1.0}})
      ROOT add = f32[1,1] add(p0, c1)
    })",
                   R"(
    // CHECK:      @main_add(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<1x1xf32>
    // CHECK-SAME:     %[[X:.*]]: index {{.*}}, %[[Y:.*]]: index {{.*}}
    // CHECK:        %[[C_1:.*]] = arith.constant 1
    // CHECK:        %[[A:.*]] = tensor.extract %[[ARG0]][%[[X]], %[[Y]]]
    // CHECK:        %[[RET:.*]] = arith.addf %[[A]], %[[C_1]]
    // CHECK:        return %[[RET]]
  })"));
}

TEST_F(ElementalHloToMlirTest, DynamicSlice) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      in = f32[20,30] parameter(0)
      i0 = s32[] parameter(1)
      i1 = s32[] parameter(2)
      ROOT slice = f32[4,5] dynamic-slice(in, i0, i1), dynamic_slice_sizes={4,5}
    })",
                   R"(
    // CHECK:      @main_slice(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<20x30xf32>,
    // CHECK-SAME:     %[[I0_T:.*]]: tensor<i32>, %[[I1_T:.*]]: tensor<i32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:    %[[C16:.*]] = arith.constant 16
    // CHECK-DAG:    %[[C25:.*]] = arith.constant 25
    // CHECK:        %[[I0:.*]] = tensor.extract %[[I0_T]]
    // CHECK:        %[[I0_1:.*]] = arith.index_cast %[[I0]]
    // CHECK:        %[[I0_2:.*]] = arith.minsi %[[I0_1]], %[[C16]]
    // CHECK:        %[[I0_3:.*]] = arith.maxsi %[[I0_2]], %[[C0]]
    // CHECK:        %[[X_IN:.*]] = arith.addi %[[X]], %[[I0_3]]
    // CHECK:        %[[I1:.*]] = tensor.extract %[[I1_T]]
    // CHECK:        %[[I1_1:.*]] = arith.index_cast %[[I1]]
    // CHECK:        %[[I1_2:.*]] = arith.minsi %[[I1_1]], %[[C25]]
    // CHECK:        %[[I1_3:.*]] = arith.maxsi %[[I1_2]], %[[C0]]
    // CHECK:        %[[Y_IN:.*]] = arith.addi %[[Y]], %[[I1_3]]
    // CHECK:        %[[RET:.*]] = tensor.extract %[[ARG0]][%[[X_IN]], %[[Y_IN]]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, DynamicSliceUnsignedIndices) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      in = f32[20,30] parameter(0)
      i0 = u32[] parameter(1)
      i1 = u32[] parameter(2)
      ROOT slice = f32[4,5] dynamic-slice(in, i0, i1), dynamic_slice_sizes={4,5}
    })",
                   R"(
    // CHECK:      @main_slice(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<20x30xf32>,
    // CHECK-SAME:     %[[I0_T:.*]]: tensor<ui32>, %[[I1_T:.*]]: tensor<ui32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {
    // CHECK-DAG:    %[[C16:.*]] = arith.constant 16
    // CHECK-DAG:    %[[C25:.*]] = arith.constant 25
    // CHECK:        %[[I0:.*]] = tensor.extract %[[I0_T]]
    // CHECK:        %[[I0_SIGNLESS:.*]] = builtin.unrealized_conversion_cast %[[I0]] : ui32 to i32
    // CHECK:        %[[I0_1:.*]] = arith.index_castui %[[I0_SIGNLESS]]
    // CHECK:        %[[I0_2:.*]] = arith.minui %[[I0_1]], %[[C16]]
    // CHECK:        %[[X_IN:.*]] = arith.addi %[[X]], %[[I0_2]]
    // CHECK:        %[[I1:.*]] = tensor.extract %[[I1_T]]
    // CHECK:        %[[I1_SIGNLESS:.*]] = builtin.unrealized_conversion_cast %[[I1]] : ui32 to i32
    // CHECK:        %[[I1_1:.*]] = arith.index_castui %[[I1_SIGNLESS]]
    // CHECK:        %[[I1_2:.*]] = arith.minui %[[I1_1]], %[[C25]]
    // CHECK:        %[[Y_IN:.*]] = arith.addi %[[Y]], %[[I1_2]]
    // CHECK:        %[[RET:.*]] = tensor.extract %[[ARG0]][%[[X_IN]], %[[Y_IN]]]
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, DynamicUpdateSlice) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      in = f32[20,30] parameter(0)
      updates = f32[5,6] parameter(1)
      i0 = s32[] parameter(2)
      i1 = s32[] parameter(3)
      ROOT updated = f32[20,30] dynamic-update-slice(in, updates, i0, i1)
    })",
                   R"(
    // CHECK:      @main_updated(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<20x30xf32>, %[[ARG1:.*]]: tensor<5x6xf32>
    // CHECK-SAME:     %[[I0_T:.*]]: tensor<i32>, %[[I1_T:.*]]: tensor<i32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:    %[[C5:.*]] = arith.constant 5
    // CHECK-DAG:    %[[C6:.*]] = arith.constant 6
    // CHECK-DAG:    %[[C15:.*]] = arith.constant 15
    // CHECK-DAG:    %[[C24:.*]] = arith.constant 24
    // CHECK:        %[[I0:.*]] = tensor.extract %[[I0_T]]
    // CHECK:        %[[I0_1:.*]] = arith.index_cast %[[I0]]
    // CHECK:        %[[I0_2:.*]] = arith.minsi %[[I0_1]], %[[C15]]
    // CHECK:        %[[START_X:.*]] = arith.maxsi %[[I0_2]], %[[C0]]
    // CHECK:        %[[END_X:.*]] = arith.addi %[[START_X]], %[[C5]]
    // CHECK:        %[[LOW_X:.*]] = arith.cmpi sge, %[[X]], %[[START_X]]
    // CHECK:        %[[HIGH_X:.*]] = arith.cmpi slt, %[[X]], %[[END_X]]
    // CHECK:        %[[BOUNDS_X:.*]] = arith.andi %[[LOW_X]], %[[HIGH_X]]
    // CHECK:        %[[UPDATES_X:.*]] = arith.subi %[[X]], %[[START_X]]
    // CHECK:        arith.andi
    // CHECK:        %[[BOUNDS:.*]] = arith.andi
    // CHECK:        scf.if %[[BOUNDS]]
    // CHECK:          tensor.extract %[[ARG1]][%[[UPDATES_X]]
    // CHECK:        } else {
    // CHECK:          tensor.extract %[[ARG0]][%[[X]]
  )"));
}

TEST_F(ElementalHloToMlirTest, DynamicUpdateSliceUnsigned) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      in = u32[20,30] parameter(0)
      updates = u32[5,6] parameter(1)
      i0 = s32[] parameter(2)
      i1 = s32[] parameter(3)
      ROOT updated = u32[20,30] dynamic-update-slice(in, updates, i0, i1)
    })",
                   R"(
    // CHECK:      @main_updated(
    // CHECK-SAME:     %[[ARG0:.*]]: tensor<20x30xui32>, %[[ARG1:.*]]: tensor<5x6xui32>
    // CHECK-SAME:     %[[I0_T:.*]]: tensor<i32>, %[[I1_T:.*]]: tensor<i32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {
    // CHECK-DAG:    %[[C0:.*]] = arith.constant 0
    // CHECK-DAG:    %[[C5:.*]] = arith.constant 5
    // CHECK-DAG:    %[[C6:.*]] = arith.constant 6
    // CHECK-DAG:    %[[C15:.*]] = arith.constant 15
    // CHECK-DAG:    %[[C24:.*]] = arith.constant 24
    // CHECK:        %[[I0:.*]] = tensor.extract %[[I0_T]]
    // CHECK:        %[[I0_1:.*]] = arith.index_cast %[[I0]]
    // CHECK:        %[[I0_2:.*]] = arith.minsi %[[I0_1]], %[[C15]]
    // CHECK:        %[[START_X:.*]] = arith.maxsi %[[I0_2]], %[[C0]]
    // CHECK:        %[[END_X:.*]] = arith.addi %[[START_X]], %[[C5]]
    // CHECK:        %[[LOW_X:.*]] = arith.cmpi sge, %[[X]], %[[START_X]]
    // CHECK:        %[[HIGH_X:.*]] = arith.cmpi slt, %[[X]], %[[END_X]]
    // CHECK:        %[[BOUNDS_X:.*]] = arith.andi %[[LOW_X]], %[[HIGH_X]]
    // CHECK:        %[[UPDATES_X:.*]] = arith.subi %[[X]], %[[START_X]]
    // CHECK:        arith.andi
    // CHECK:        %[[BOUNDS:.*]] = arith.andi
    // CHECK:        scf.if %[[BOUNDS]]
    // CHECK:          %[[VAL0:.*]] = tensor.extract %[[ARG1]][%[[UPDATES_X]]
    // CHECK:          builtin.unrealized_conversion_cast %[[VAL0]]
    // CHECK:        } else {
    // CHECK:          %[[VAL1:.*]] = tensor.extract %[[ARG0]][%[[X]]
    // CHECK:          builtin.unrealized_conversion_cast %[[VAL1]]
  )"));
}

TEST_F(ElementalHloToMlirTest, IotaUnsigned) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      ROOT iota = u32[10,20] iota(), iota_dimension=0
    })",
                   R"(
    // CHECK:      @main_iota(
    // CHECK-SAME:     %[[I0:.*]]: index {{.*}}, %[[I1:.*]]: index {{.*}} {
    // CHECK:        %[[VAL:.*]] = arith.index_castui %[[I0]] : index to i32
    // CHECK:        builtin.unrealized_conversion_cast %[[VAL]] : i32 to ui32
  )"));
}

TEST_F(ElementalHloToMlirTest, IotaComplex) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      ROOT iota = c64[6,4,5] iota(), iota_dimension=1
    })",
                   R"(
    // CHECK:      @main_iota(
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}},
    // CHECK-SAME:     %[[Z:.*]]: index {{{.*}}}
    // CHECK:        %[[ZERO:.*]] = arith.constant 0.000000e+00 : f32
    // CHECK:        %[[I:.*]] = arith.index_castui %[[Y]] : index to i32
    // CHECK:        %[[F:.*]] = arith.sitofp %[[I]] : i32 to f32
    // CHECK:        %[[RET:.*]] = complex.create %[[F]], %[[ZERO]] : complex<f32>
    // CHECK:        return %[[RET]]
  )"));
}

TEST_F(ElementalHloToMlirTest, MixedIndexingTuple) {
  TF_EXPECT_OK(Run(R"(
    ENTRY main {
      %p0 = f32[10,10] parameter(0)
      %p1 = f32[100] parameter(1)
      ROOT tuple = (f32[10,10], f32[100]) tuple(%p0, %p1)
    })",
                   R"(
    // CHECK:      @main_tuple(
    // CHECK-SAME:     %[[P0:.*]]: tensor<10x10xf32>,
    // CHECK-SAME:     %[[P1:.*]]: tensor<100xf32>,
    // CHECK-SAME:     %[[X:.*]]: index {{{.*}}}, %[[Y:.*]]: index {{{.*}}}
    // CHECK:        %[[A:.*]] = tensor.extract %[[P0]][%[[X]], %[[Y]]]
    // CHECK:        %[[IDX:.*]] = affine.apply
    // CHECK-SAME:       affine_map<()[s0, s1] -> (s0 * 10 + s1)>()
    // CHECK-SAME:       [%[[X]], %[[Y]]]
    // CHECK:        %[[B:.*]] = tensor.extract %[[P1]][%[[IDX]]]
    // CHECK:        return %[[A]], %[[B]]
  )"));
}

TEST_F(ElementalHloToMlirTest, ReducePrecision) {
  TF_EXPECT_OK(Run(R"(
                     ENTRY main {
                       %p0 = f32[5,7] parameter(0)
                       ROOT r = f32[5,7] reduce-precision(%p0),
                        exponent_bits=8, mantissa_bits=23
                     }
                   )",
                   "// CHECK: @main"));
}

TEST_F(ElementalHloToMlirTest, Map) {
  TF_EXPECT_OK(Run(R"(
    mapper {
      a = f32[] parameter(0)
      b = f32[] parameter(1)
      ROOT add = f32[] add(a, b)
    }
    ENTRY main {
      %p0 = f32[5,7] parameter(0)
      %p1 = f32[5,7] parameter(1)
      ROOT r = f32[5,7] map(%p0, %p1), dimensions={}, to_apply=mapper
    })",
                   R"(
    // CHECK: @main
    // CHECK-NEXT: tensor.extract
    // CHECK-NEXT: tensor.extract
    // CHECK-NEXT: pure_call @mapper_add
    // CHECK-NEXT: return
  )"));
}

}  // namespace
}  // namespace mlir_converter
}  // namespace gpu
}  // namespace xla
