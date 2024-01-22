/* Copyright 2017 The OpenXLA Authors.

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

#include "xla/service/gpu/ir_emission_utils.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/FPEnv.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/MemRef/IR/MemRef.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypeInterfaces.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Interfaces/SideEffectInterfaces.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/literal.h"
#include "xla/mlir_hlo/lhlo/IR/lhlo_ops.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/primitive_util.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/gpu/hlo_traversal.h"
#include "xla/service/gpu/target_util.h"
#include "xla/service/hlo_parser.h"
#include "xla/service/llvm_ir/buffer_assignment_util.h"
#include "xla/service/llvm_ir/llvm_type_conversion_util.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/status_macros.h"
#include "xla/statusor.h"
#include "xla/translate/mhlo_to_hlo/location_exporter.h"
#include "xla/translate/mhlo_to_hlo/type_to_shape.h"
#include "xla/types.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/ml_dtypes.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

namespace {

// Return whether the given shape is rank 2 excluding the batch dimensions.
bool IsRank2(const Shape& shape, int64_t batch_dimensions_size) {
  return shape.rank() == batch_dimensions_size + 2;
}

Shape GetShapeFromTensorType(mlir::Value value) {
  constexpr char kDefaultLayoutAttrName[] = "xla_shape";

  mlir::Operation* op = value.getDefiningOp();
  CHECK(op);
  CHECK(value.getType().isa<mlir::TensorType>());
  Shape shape;
  if (auto attr = op->getAttrOfType<mlir::StringAttr>(kDefaultLayoutAttrName)) {
    shape = *xla::ParseShape(
        absl::string_view(attr.getValue().data(), attr.getValue().size()));
  } else {
    shape = TypeToShape(value.getType());
  }
  return shape;
}

}  // namespace

bool IsMatrixMultiplication(const HloInstruction& dot) {
  if (dot.opcode() != HloOpcode::kDot) {
    return false;
  }
  const Shape& lhs_shape = dot.operand(0)->shape();
  const Shape& rhs_shape = dot.operand(1)->shape();
  const DotDimensionNumbers& dim_numbers = dot.dot_dimension_numbers();

  PrimitiveType output_primitive_type = dot.shape().element_type();
  bool type_is_allowed =
      (output_primitive_type == F8E4M3FN || output_primitive_type == F8E5M2 ||
       output_primitive_type == F16 || output_primitive_type == BF16 ||
       output_primitive_type == F32 || output_primitive_type == F64 ||
       output_primitive_type == C64 || output_primitive_type == C128) ||
      (output_primitive_type == S32 && lhs_shape.element_type() == S8 &&
       rhs_shape.element_type() == S8);
  bool shapes_are_valid =
      type_is_allowed &&
      IsRank2(lhs_shape, dim_numbers.lhs_batch_dimensions_size()) &&
      IsRank2(rhs_shape, dim_numbers.lhs_batch_dimensions_size()) &&
      IsRank2(dot.shape(), dim_numbers.lhs_batch_dimensions_size()) &&
      !ShapeUtil::IsZeroElementArray(lhs_shape) &&
      !ShapeUtil::IsZeroElementArray(rhs_shape);

  if (!shapes_are_valid) {
    return false;
  }

  // The size of the reduction dimension should match. The shape inference
  // guarantees this invariant, so the check here is for programming
  // errors.
  CHECK_EQ(lhs_shape.dimensions(dim_numbers.lhs_contracting_dimensions(0)),
           rhs_shape.dimensions(dim_numbers.rhs_contracting_dimensions(0)));

  return true;
}

const char* const kCusolverCholeskyCallTarget = "__cusolver$cholesky";

bool IsCustomCallToCusolver(const HloInstruction& hlo) {
  if (hlo.opcode() != HloOpcode::kCustomCall) {
    return false;
  }
  return hlo.custom_call_target() == kCusolverCholeskyCallTarget;
}

bool IsCustomCallToTopK(const HloInstruction& hlo) {
  return hlo.opcode() == HloOpcode::kCustomCall &&
         hlo.custom_call_target() == kTopKCustomCallTarget;
}

bool IsInputFusibleSlices(mlir::Operation* unnested_hlo,
                          bool verify_no_strides) {
  auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(unnested_hlo);
  if (!fusion) {
    return false;
  }

  auto is_non_strided = [](mlir::DenseIntElementsAttr strides) -> bool {
    return absl::c_all_of(
        strides, [](const llvm::APInt& stride) { return stride == 1; });
  };

  for (mlir::Value value : fusion.getFusionResults()) {
    auto slice =
        mlir::dyn_cast_or_null<mlir::mhlo::SliceOp>(value.getDefiningOp());
    if (!slice) {
      return false;
    }
    if (verify_no_strides && !is_non_strided(slice.getStrides())) {
      return false;
    }
  }
  return true;
}

bool IsSliceWithUnitStrides(const HloInstruction* instr) {
  auto slice = DynCast<HloSliceInstruction>(instr);
  return slice && absl::c_all_of(slice->slice_strides(),
                                 [](int64_t stride) { return stride == 1; });
}

bool IsContiguousSlice(const HloInstruction& instr) {
  auto slice = DynCast<HloSliceInstruction>(&instr);
  if (!slice) return false;
  // No need to check for strides because if stride != 1 there's no way
  // src and dst dimensions match.
  const Shape& src_shape = slice->operand(0)->shape();
  const Shape& dst_shape = slice->shape();
  bool sliced_dim_found = false;
  for (auto dim : src_shape.layout().minor_to_major()) {
    if (!sliced_dim_found) {
      sliced_dim_found = dst_shape.dimensions(dim) < src_shape.dimensions(dim);
      continue;
    }
    if (dst_shape.dimensions(dim) != 1) return false;
  }
  return true;
}

// This emits a device-side call to
// "i32 vprintf(i8* fmt, arguments_type* arguments)" in the driver; see
// http://docs.nvidia.com/cuda/ptx-writers-guide-to-interoperability/index.html#system-calls
llvm::Value* EmitPrintf(absl::string_view fmt,
                        absl::Span<llvm::Value* const> arguments,
                        llvm::IRBuilder<>* builder) {
  std::vector<llvm::Type*> argument_types;

  // Variadic arguments implicit promotion [1] converts float to double,
  // and bool/char/short are converted to int.
  // [1] https://en.cppreference.com/w/cpp/language/variadic_arguments
  auto requires_int32_promotion = [](llvm::Type* type) {
    return type->isIntegerTy(/*BitWidth=*/1) ||
           type->isIntegerTy(/*BitWidth=*/8) ||
           type->isIntegerTy(/*BitWidth=*/16);
  };
  auto requires_double_promotion = [](llvm::Type* type) {
    return type->isFloatingPointTy();
  };

  for (auto argument : arguments) {
    llvm::Type* type = argument->getType();
    if (requires_double_promotion(type)) {
      argument_types.push_back(builder->getDoubleTy());
    } else if (requires_int32_promotion(type)) {
      argument_types.push_back(builder->getInt32Ty());
    } else {
      argument_types.push_back(type);
    }
  }
  auto* arguments_type = llvm::StructType::create(argument_types);
  llvm::Value* arguments_ptr = builder->CreateAlloca(arguments_type);
  for (size_t i = 0; i < arguments.size(); ++i) {
    llvm::Value* value = arguments[i];
    llvm::Type* type = value->getType();
    if (requires_double_promotion(type)) {
      value = builder->CreateFPCast(value, builder->getDoubleTy());
    } else if (requires_int32_promotion(type)) {
      value = builder->CreateIntCast(value, builder->getInt32Ty(),
                                     /*isSigned=*/true);
    }
    builder->CreateStore(
        value,
        builder->CreateGEP(arguments_type, arguments_ptr,
                           {builder->getInt64(0), builder->getInt32(i)}));
  }
  llvm::Type* ptr_ty = builder->getPtrTy();
  return builder->CreateCall(
      builder->GetInsertBlock()->getParent()->getParent()->getOrInsertFunction(
          "vprintf",
          llvm::FunctionType::get(builder->getInt32Ty(), {ptr_ty, ptr_ty},
                                  /*isVarArg=*/false)),
      {builder->CreateGlobalStringPtr(llvm_ir::AsStringRef(fmt)),
       builder->CreatePointerCast(arguments_ptr, ptr_ty)});
}

// Helper function to emit call to AMDGPU shfl_down function.
llvm::Value* EmitAMDGPUShflDown(llvm::Value* value, llvm::Value* offset,
                                llvm::IRBuilder<>* b) {
  llvm::Module* module = b->GetInsertBlock()->getModule();
  CHECK_EQ(value->getType()->getPrimitiveSizeInBits(), 32);
  auto* i32_ty = b->getInt32Ty();
  llvm::FunctionCallee shfl_fn = module->getOrInsertFunction(
      llvm_ir::AsStringRef("__ockl_readuplane_i32"),
      llvm::FunctionType::get(/*Result=*/i32_ty, {i32_ty, i32_ty},
                              /*isVarArg=*/false));
  // AMDGPU device function requires first argument as i32.
  llvm::Value* result =
      b->CreateCall(shfl_fn, {b->CreateBitCast(value, i32_ty), offset});
  // AMDGPU device function always returns an i32 type.
  return b->CreateBitCast(result, value->getType());
}

// Helper function to emit call to NVPTX shfl_down intrinsic.
llvm::Value* EmitNVPTXShflDown(llvm::Value* value, llvm::Value* offset,
                               llvm::IRBuilder<>* b) {
  llvm::Module* module = b->GetInsertBlock()->getModule();
  llvm::Intrinsic::ID llvm_intrinsic_id;
  CHECK_EQ(value->getType()->getPrimitiveSizeInBits(), 32);
  if (value->getType()->isFloatTy()) {
    llvm_intrinsic_id = llvm::Intrinsic::nvvm_shfl_sync_down_f32;
  } else {
    llvm_intrinsic_id = llvm::Intrinsic::nvvm_shfl_sync_down_i32;
  }
  llvm::Function* intrinsic =
      llvm::Intrinsic::getDeclaration(module, llvm_intrinsic_id, {});
  return b->CreateCall(
      intrinsic, {b->getInt32(-1), value, offset, b->getInt32(WarpSize() - 1)});
}

// Helper function to emit call to SPIR shfl_down intrinsic.
llvm::Value* EmitSPIRShflDown(llvm::Value* value, llvm::Value* offset,
                              llvm::IRBuilder<>* b) {
  CHECK_EQ(value->getType()->getPrimitiveSizeInBits(), 32);
  if (value->getType()->isFloatTy()) {
    return EmitDeviceFunctionCall(
        "_Z34__spirv_GroupNonUniformShuffleDownffj",
        {b->getInt32(3), value, offset}, {U32, F32, U32}, F32,
        llvm::AttrBuilder(b->getContext())
            .addAttribute(llvm::Attribute::NoUnwind)
            .addAttribute(llvm::Attribute::Convergent),
        b);
  } else {
    return EmitDeviceFunctionCall(
        "_Z34__spirv_GroupNonUniformShuffleDownjjj",
        {b->getInt32(3), value, offset}, {U32, U32, U32}, U32,
        llvm::AttrBuilder(b->getContext())
            .addAttribute(llvm::Attribute::NoUnwind)
            .addAttribute(llvm::Attribute::Convergent),
        b);
  }
}

llvm::Value* EmitFullWarpShuffleDown(llvm::Value* value, llvm::Value* offset,
                                     llvm::IRBuilder<>* builder) {
  int bit_width = value->getType()->getPrimitiveSizeInBits();
  llvm::Module* module = builder->GetInsertBlock()->getModule();
  llvm::Triple target_triple = llvm::Triple(module->getTargetTriple());

  // Special case for efficiency
  if (value->getType()->isFloatTy() && bit_width == 32) {
    if (target_triple.isNVPTX()) {
      return EmitNVPTXShflDown(value, offset, builder);
    } else if (target_triple.getArch() == llvm::Triple::amdgcn) {
      return EmitAMDGPUShflDown(value, offset, builder);
    } else if (target_triple.isSPIR()) {
      return EmitSPIRShflDown(value, offset, builder);
    } else {
      LOG(FATAL) << "Invalid triple " << target_triple.str();
    }
  }

  // We must split values wider than 32 bits as the "shfl" instruction operates
  // on 32-bit values.
  int num_segments = CeilOfRatio(bit_width, 32);
  llvm::Value* x = builder->CreateBitCast(
      builder->CreateZExt(
          builder->CreateBitCast(value, builder->getIntNTy(bit_width)),
          builder->getIntNTy(32 * num_segments)),
      llvm::VectorType::get(builder->getInt32Ty(), num_segments, false));
  for (int i = 0; i < num_segments; ++i) {
    llvm::Value* insert_val;
    if (target_triple.isNVPTX()) {
      insert_val = EmitNVPTXShflDown(builder->CreateExtractElement(x, i),
                                     offset, builder);
    } else if (target_triple.getArch() == llvm::Triple::amdgcn) {
      insert_val = EmitAMDGPUShflDown(builder->CreateExtractElement(x, i),
                                      offset, builder);
    } else if (target_triple.isSPIR()) {
      insert_val = EmitSPIRShflDown(builder->CreateExtractElement(x, i), offset,
                                    builder);
    } else {
      LOG(FATAL) << "Invalid triple " << target_triple.str();
    }
    x = builder->CreateInsertElement(x, insert_val, i);
  }
  return builder->CreateBitCast(
      builder->CreateTrunc(
          builder->CreateBitCast(x, builder->getIntNTy(32 * num_segments)),
          builder->getIntNTy(bit_width)),
      value->getType());
}

llvm::Value* IsBlock0Thread0(llvm::IRBuilder<>* b) {
  llvm::Value* is_thread0 = b->CreateICmpEQ(
      b->getInt32(0),
      EmitCallToTargetIntrinsic(TargetIntrinsicID::kThreadIdx, {}, {}, b));

  llvm::Value* is_block0 = b->CreateICmpEQ(
      b->getInt32(0),
      EmitCallToTargetIntrinsic(TargetIntrinsicID::kBlockIdx, {}, {}, b));
  return b->CreateAnd(is_thread0, is_block0);
}

// Given an LMHLO op, returns the operand index of the first output operand.
//
// Notice that an operand alised to an output isn't an output, even though in
// that case WritesMlirBuffer() returns true on that operand.
//
// An operand is !WritesMlirBuffer() || equals (aliases) to a later operand. An
// output is the opposite, being both WritesMlirBuffer() and does not equal to
// any later operand.
int PartitionLmhloOperandsAndOutputs(mlir::Operation* op) {
  CHECK(op->getDialect() == op->getContext()->getLoadedDialect("lmhlo"));

  int i;
  for (i = op->getOperands().size() - 1; i >= 0; i--) {
    const bool aliased =
        std::find(op->getOperands().begin() + i + 1, op->getOperands().end(),
                  op->getOperand(i)) != op->getOperands().end();
    if (!WritesMlirBuffer(op, op->getOperand(i)) || aliased) {
      break;
    }
  }
  return i + 1;
}

llvm::SmallVector<mlir::Value> GetHloOperands(mlir::Operation* op) {
  if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
    return fusion.getInputBuffers();
  }
  if (op->getDialect() == op->getContext()->getLoadedDialect("lmhlo")) {
    int output_start = PartitionLmhloOperandsAndOutputs(op);
    llvm::SmallVector<mlir::Value> operands;
    for (int i = 0; i < output_start; i++) {
      operands.push_back(op->getOperand(i));
    }
    return operands;
  }
  if (op->getDialect() == op->getContext()->getLoadedDialect("mhlo")) {
    return op->getOperands();
  }
  LOG(FATAL) << "Unexpected op: " << llvm_ir::DumpToString(op);
}

llvm::SmallVector<mlir::Value> GetHloOutputs(mlir::Operation* op) {
  if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
    return fusion.getOutputBuffers();
  }
  if (op->getDialect() == op->getContext()->getLoadedDialect("lmhlo")) {
    int output_start = PartitionLmhloOperandsAndOutputs(op);
    llvm::SmallVector<mlir::Value> outputs;
    for (int i = output_start; i < op->getNumOperands(); i++) {
      outputs.push_back(op->getOperand(i));
    }
    return outputs;
  }
  if (op->getDialect() == op->getContext()->getLoadedDialect("mhlo")) {
    return op->getResults();
  }
  LOG(FATAL) << "Unexpected op: " << llvm_ir::DumpToString(op);
}

bool WritesMlirBuffer(mlir::Operation* op, mlir::Value operand) {
  llvm::SmallVector<mlir::MemoryEffects::EffectInstance, 2> effects;
  mlir::cast<mlir::MemoryEffectOpInterface>(op).getEffectsOnValue(operand,
                                                                  effects);
  return absl::c_any_of(
      effects, [](const mlir::MemoryEffects::EffectInstance& instance) {
        return mlir::isa<mlir::MemoryEffects::Write>(instance.getEffect());
      });
}

static int64_t GetMemRefSizeInBytes(mlir::MemRefType type) {
  // For i1 memrefs, the underlying allocation is 8 bits.
  if (type.getElementType().isInteger(/*width=*/1)) {
    return type.getNumElements();
  } else if (auto complexType =
                 type.getElementType().dyn_cast<mlir::ComplexType>()) {
    auto elementType = complexType.getElementType();
    return elementType.getIntOrFloatBitWidth() * type.getNumElements() * 2 /
           CHAR_BIT;
  } else {
    return type.getNumElements() * type.getElementTypeBitWidth() / CHAR_BIT;
  }
}

static int64_t GetAllocationIndex(mlir::BlockArgument func_arg,
                                  std::string* constant_name) {
  auto func_op =
      mlir::cast<mlir::func::FuncOp>(func_arg.getParentRegion()->getParentOp());
  if (constant_name) {
    if (auto constant_name_attr = func_op.getArgAttrOfType<mlir::StringAttr>(
            func_arg.getArgNumber(), "lmhlo.constant_name")) {
      *constant_name = constant_name_attr.getValue().str();
    }
  }
  return func_arg.getArgNumber();
}

absl::StatusOr<BufferAllocation::Slice> GetAllocationSlice(
    mlir::Value v, absl::Span<const BufferAllocation* const> allocations,
    std::string* constant_name) {
  if (constant_name) {
    constant_name->clear();
  }

  int64_t size = GetMemRefSizeInBytes(v.getType().cast<mlir::MemRefType>());

  // We match the following patterns here:
  //  base := ViewOp(arg) | get_global_memref (global_memref) | arg
  //  root := base | MemRefReinterpretCastOp(base) | CollapseShapeOp(base)

  if (auto cast = mlir::dyn_cast_or_null<mlir::memref::ReinterpretCastOp>(
          v.getDefiningOp())) {
    v = cast.getViewSource();
  }
  if (auto collapse_shape =
          mlir::dyn_cast_or_null<mlir::memref::CollapseShapeOp>(
              v.getDefiningOp())) {
    v = collapse_shape.getSrc();
  }

  if (auto view =
          mlir::dyn_cast_or_null<mlir::memref::ViewOp>(v.getDefiningOp())) {
    TF_RET_CHECK(view.getSource().isa<mlir::BlockArgument>());

    const BufferAllocation* allocation = allocations[GetAllocationIndex(
        view.getSource().cast<mlir::BlockArgument>(), constant_name)];
    return BufferAllocation::Slice(
        allocation,
        mlir::cast<mlir::arith::ConstantOp>(view.getByteShift().getDefiningOp())
            .getValue()
            .cast<mlir::IntegerAttr>()
            .getValue()
            .getSExtValue(),
        size);
  }
  if (auto get_global = mlir::dyn_cast_or_null<mlir::memref::GetGlobalOp>(
          v.getDefiningOp())) {
    auto module = get_global->getParentOfType<mlir::ModuleOp>();
    if (constant_name) {
      *constant_name = get_global.getName().str();
    }
    auto global = mlir::cast<mlir::memref::GlobalOp>(
        module.lookupSymbol(get_global.getName()));
    int64_t index =
        global->getAttrOfType<mlir::IntegerAttr>("lmhlo.alloc").getInt();

    return BufferAllocation::Slice(allocations[index], 0,
                                   allocations[index]->size());
  }
  if (auto arg = v.dyn_cast<mlir::BlockArgument>()) {
    return BufferAllocation::Slice(
        allocations[GetAllocationIndex(arg, constant_name)], 0, size);
  }

  return Unimplemented(
      "Operand has to be in the form of ViewOp(arg) or "
      "StaticMemRefCastOp(ViewOp(arg)) or arg");
}

absl::StatusOr<BufferAllocation::Slice> GetAllocationSlice(
    const BufferAssignment& buffer_assignment, const HloInstruction* instr,
    const ShapeIndex& index) {
  return buffer_assignment.GetUniqueSlice(instr, index);
}

std::vector<const HloInstruction*> GetOutputDefiningDynamicUpdateSlices(
    const std::vector<const HloInstruction*>& roots) {
  // Same as GetOutputDefiningDynamicUpdateSliceOps but on a HLO fusion
  // computation instead of a LMHLO FusionOp.
  std::vector<const HloInstruction*> dus_ops;
  for (const HloInstruction* root : roots) {
    while (root->opcode() == HloOpcode::kBitcast) {
      root = root->operand(0);
    }

    if (root->opcode() == HloOpcode::kDynamicUpdateSlice) {
      dus_ops.push_back(root);
    }
  }

  return dus_ops;
}

std::vector<mlir::mhlo::DynamicUpdateSliceOp>
GetOutputDefiningDynamicUpdateSliceOps(mlir::lmhlo::FusionOp fusion) {
  std::vector<mlir::mhlo::DynamicUpdateSliceOp> dus_ops;

  auto fusion_results = fusion.getFusionResults();
  for (const auto& fusion_result : fusion_results) {
    // A dynamic slice update is said to be "defining" of a result if that
    // result is the output of a dynamic slice update, or if that result is
    // the output of a bitcast of a dynamic slice update---since a bitcast may
    // be handled here as a no-op.
    if (auto dus = mlir::dyn_cast<mlir::mhlo::DynamicUpdateSliceOp>(
            fusion_result.getDefiningOp())) {
      dus_ops.push_back(dus);
    }

    if (auto bitcast = mlir::dyn_cast<mlir::mhlo::BitcastOp>(
            fusion_result.getDefiningOp())) {
      if (auto dus = mlir::dyn_cast<mlir::mhlo::DynamicUpdateSliceOp>(
              bitcast.getOperand().getDefiningOp())) {
        dus_ops.push_back(dus);
      }
    }
  }
  return dus_ops;
}

template <typename T>
absl::InlinedVector<const HloInstruction*, 4> GetStartIndices(T instr) {
  absl::InlinedVector<const HloInstruction*, 4> result;
  for (int i = instr->first_index_operand_number(); i < instr->operand_count();
       i++) {
    const HloInstruction* index = instr->operand(i);
    result.push_back(index);
  }
  return result;
}

absl::StatusOr<bool> CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
    const HloFusionInstruction* fusion,
    const BufferAssignment* buffer_assignment,
    const std::vector<const HloInstruction*>& roots) {
  std::vector<const HloInstruction*> dus_instrs =
      GetOutputDefiningDynamicUpdateSlices(roots);

  // Get output buffers for fusion.
  std::vector<BufferAllocation::Slice> output_buffers;
  TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
      fusion->shape(), [&](const Shape& shape, const ShapeIndex index) {
        if (shape.IsArray()) {
          TF_ASSIGN_OR_RETURN(BufferAllocation::Slice buffer,
                              buffer_assignment->GetUniqueSlice(fusion, index));
          output_buffers.push_back(buffer);
        }
        return absl::OkStatus();
      }));

  // This check could probably be relaxed: if code generation is made to use a
  // separate parallel loop for each dynamic slice update, then it shouldn't be
  // necessary for every output to be a dynamic slice update, nor to have the
  // same shape.
  if (dus_instrs.size() != output_buffers.size()) {
    return false;
  }

  if (output_buffers.empty()) {
    return Internal("Output buffers should not be empty");
  }

  Shape update_shape = dus_instrs[0]->operand(1)->shape();

  // TODO(anlunx): Reuse this code in both HLO and LMHLO path.
  for (int i = 0; i < dus_instrs.size(); ++i) {
    auto* dus = Cast<HloDynamicUpdateSliceInstruction>(dus_instrs[i]);

    // Dynamic slice updates should have a single path to the root to avoid
    // allowing a dynamic slice update to depend on another, as this would not
    // be guaranteed to work with the current codegen.
    if (!dus->IsRoot() && dus->user_count() != 1) return false;

    // We follow DUS users until we find a root instruction. We support only
    // few patterns:
    //
    //   (1) ROOT dynamic-update-slice
    //   (2) ROOT tuple(dynamic-update-slice)
    //   (3) ROOT bitcast(dynamic-update-slice)
    //   (4) ROOT tuple(bitcast(dynamic-update-slice))
    HloInstruction* dus_user = dus->IsRoot() ? nullptr : dus->users().front();

    // Since the direct consumer of an output dynamic slice update may be a
    // bitcast, we also check that this bitcast is used a single time.
    // This property is also important because reads and writes on the parameter
    // to be updated are done using the shape and layout of the dynamic slice
    // update. This is a valid approach only if a subsequent bitcast is not read
    // by any other op within the fusion as this may result in codegen
    // accessing elements using the wrong physical layout.
    if (dus_user && dus_user->opcode() == HloOpcode::kBitcast) {
      if (!dus_user->IsRoot() && dus_user->user_count() != 1) return false;

      // Stop following DUS users if we found a root.
      dus_user = dus_user->IsRoot() ? nullptr : dus_user->users().front();
    }

    // Check that last DUS user is a tuple operation at ROOT position.
    if (dus_user && dus_user->opcode() == HloOpcode::kTuple) {
      if (!dus_user->IsRoot()) return false;

      // Stop following DUS users if we found a root.
      dus_user = nullptr;
    }

    // We can't emit DUS fusion if we have unsupported DUS users.
    if (dus_user != nullptr) return false;

    // Find "real" DUS operand by skipping bitcasted operands.
    const HloInstruction* operand = dus->operand(0);
    if (operand->opcode() == HloOpcode::kBitcast) {
      operand = operand->operand(0);
    }

    // Operand to a DUS (or Bitcast) must be a fusion parameter.
    auto* parameter = DynCast<HloParameterInstruction>(operand);
    if (!parameter) return false;

    // We require that the parameter being updated is only read at the same
    // index positions by all users, since we otherwise risk a race condition
    // when updating the parameter inplace.
    std::queue<const HloInstruction*> q;
    absl::flat_hash_set<const HloInstruction*> visited;
    q.push(parameter);
    visited.insert(parameter);
    // We have already checked above that the DUS only has one user. So we don't
    // need to visit it during the breadth-first search.
    visited.insert(dus);
    while (!q.empty()) {
      const HloInstruction* instr = q.front();
      q.pop();
      for (const HloInstruction* user : instr->users()) {
        if (user->opcode() == HloOpcode::kDynamicSlice &&
            dus->operand(0) == user->operand(0) &&
            update_shape == user->shape()) {
          // We can still emit in-place in this case if the same slice is
          // accessed by the DUS and the DS. If they don't access the same
          // slice, the two slices might partially overlap and read/write the
          // same index at different times, and then we cannot guarantee that we
          // read before it is overwritten. However if both access only a single
          // element, there also can be no race condition.
          absl::InlinedVector<const HloInstruction*, 4> user_start_indices =
              GetStartIndices(Cast<HloDynamicSliceInstruction>(user));
          absl::InlinedVector<const HloInstruction*, 4> dus_start_indices =
              GetStartIndices(dus);
          if (ShapeUtil::ElementsIn(update_shape) != 1 &&
              user_start_indices != dus_start_indices) {
            return false;
          }
        } else if (user != dus && !user->IsElementwise() &&
                   user->opcode() != HloOpcode::kBitcast &&
                   user->opcode() != HloOpcode::kTuple) {
          return false;
        }
        if (visited.insert(user).second) {
          q.push(user);
        }
      }
    }

    // This check could probably be relaxed: if code generation is made to use a
    // separate parallel loop for each dynamic slice update, then it shouldn't
    // be necessary for the shape to be the same for all the dynamic slice
    // updates. Note that this equality check purposefully ignores the element
    // type.
    if (dus->operand(1)->shape() != update_shape) {
      return false;
    }

    const HloInstruction* lhs = fusion->operand(parameter->parameter_number());
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice lhs_buffer,
                        buffer_assignment->GetUniqueSlice(lhs, {}));
    BufferAllocation::Slice rhs_buffer = output_buffers[i];
    if (lhs_buffer != rhs_buffer) {
      return false;
    }
  }

  return true;
}

bool CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
    mlir::lmhlo::FusionOp fusion,
    absl::Span<const BufferAllocation* const> allocations) {
  std::vector<mlir::mhlo::DynamicUpdateSliceOp> dus_ops =
      GetOutputDefiningDynamicUpdateSliceOps(fusion);

  // This check could probably be relaxed: if code generation is made to use a
  // separate parallel loop for each dynamic slice update, then it shouldn't be
  // necessary for every output to be a dynamic slice update, nor to have the
  // same shape.
  if (dus_ops.size() != fusion.getFusionResults().size()) {
    return false;
  }

  auto output_buffers = fusion.getOutputBuffers();
  CHECK_GE(output_buffers.size(), 1);
  CHECK_EQ(dus_ops.size(), output_buffers.size());

  auto update_shape =
      dus_ops[0].getUpdate().getType().cast<mlir::ShapedType>().getShape();

  // We can safely assume here that the slices being updated do not overlap, as
  // constructing a fusion with them would not be safe otherwise.
  for (auto [dus, output_buffer] : llvm::zip(dus_ops, output_buffers)) {
    // Dynamic slice updates should have a single path to the root---this to
    // avoid allowing a dynamic slice update to depend on another, as this would
    // not be guaranteed to work with the current codegen.
    if (!dus->hasOneUse()) {
      return false;
    }

    // Since the direct consumer of an output dynamic slice update may be a
    // bitcast, we also check that this bitcast is used a single time.
    // This property is also important because reads and writes on the parameter
    // to be updated are done using the shape and layout of the dynamic slice
    // update. This is a valid approach only if a subsequent bitcast is not read
    // by any other op within the fusion---as this may result in codegen
    // accessing elements using the wrong physical layout.
    auto dus_user = *dus->user_begin();
    if (auto bitcast = mlir::dyn_cast<mlir::mhlo::BitcastOp>(dus_user)) {
      if (!bitcast->hasOneUse()) {
        return false;
      }
      dus_user = *bitcast->user_begin();
    }
    if (!mlir::isa<mlir::bufferization::MaterializeInDestinationOp>(dus_user)) {
      return false;
    }
    auto operand = dus.getOperand();
    // A bitcast separating a fusion input from a dynamic slice update can be
    // treated as a no-op.
    if (auto bitcast =
            mlir::dyn_cast<mlir::mhlo::BitcastOp>(operand.getDefiningOp())) {
      operand = bitcast.getOperand();
    }

    auto parameter = mlir::dyn_cast<mlir::bufferization::ToTensorOp>(
        operand.getDefiningOp());

    if (!parameter) {
      return false;
    }

    // We require that the parameter being updated is only read at the same
    // index positions by all users, since we otherwise risk a race condition
    // when updating the parameter inplace.
    std::queue<mlir::Operation*> q;
    absl::flat_hash_set<mlir::Operation*> visited;
    q.push(parameter);
    visited.insert(parameter);
    // We have already checked above that the DUS only has one user: a
    // (possibly bitcasted) MaterializeInDestinationOp. So we don't need to
    // visit it during the breadth-first search.
    visited.insert(dus);
    while (!q.empty()) {
      auto op = q.front();
      q.pop();
      for (auto user : op->getUsers()) {
        if (mlir::isa<mlir::mhlo::DynamicSliceOp>(user) &&
            dus->getOperand(0) == user->getOperand(0) &&
            update_shape == user->getResult(0)
                                .getType()
                                .cast<mlir::ShapedType>()
                                .getShape()) {
          // We can still emit in-place in this case if the same slice is
          // accessed by the DUS and the DS. If they don't access the same
          // slice, the two slices might partially overlap and read/write the
          // same index at different times, and then we cannot guarantee that we
          // read before it is overwritten. However if both access only a single
          // element, there also can be no race condition.
          if (mlir::ShapedType::getNumElements(update_shape) != 1 &&
              dus.getStartIndices() !=
                  mlir::dyn_cast<mlir::mhlo::DynamicSliceOp>(user)
                      .getStartIndices()) {
            return false;
          }
        } else if (user != dus &&
                   !user->hasTrait<mlir::OpTrait::Elementwise>() &&
                   !mlir::isa<mlir::mhlo::BitcastOp, mlir::mhlo::TupleOp>(
                       user)) {
          return false;
        }
        if (visited.insert(user).second) {
          q.push(user);
        }
      }
    }

    // This check could probably be relaxed: if code generation is made to use a
    // separate parallel loop for each dynamic slice update, then it shouldn't
    // be necessary for the shape to be the same for all the dynamic slice
    // updates. Note that this equality check purposefully ignores the element
    // type.
    if (dus.getUpdate().getType().cast<mlir::ShapedType>().getShape() !=
        update_shape) {
      return false;
    }

    auto maybe_lhs = GetAllocationSlice(parameter.getMemref(), allocations);
    auto maybe_rhs = GetAllocationSlice(output_buffer, allocations);

    if (!(maybe_lhs.ok() && maybe_rhs.ok() && *maybe_lhs == *maybe_rhs)) {
      return false;
    }
  }

  return true;
}

Shape GetShape(mlir::Value value) {
  Shape shape;
  if (value.getType().isa<mlir::MemRefType>()) {
    shape = TypeToShape(value.getType());
  } else if (value.getType().isa<mlir::TensorType>()) {
    shape = GetShapeFromTensorType(value);
  } else if (value.getType().isa<mlir::TupleType>()) {
    shape = TypeToShape(value.getType());
  } else {
    LOG(FATAL) << "Unexpected value type to get shape for";
  }
  if (primitive_util::Is4BitType(shape.element_type())) {
    // 4-bit types are always packed on the GPU
    shape.mutable_layout()->set_element_size_in_bits(4);
  }
  return shape;
}

std::optional<TransposeDescription> FindTiledTranspose(
    const HloInstruction& instr) {
  if (instr.opcode() != HloOpcode::kCopy) {
    return std::nullopt;
  }

  if (std::optional<Vector3> tr = ShapeUtil::GetNormalizedTransposeShape(
          instr.operand(0)->shape(), instr.shape(), Vector3{0, 2, 1})) {
    if ((tr->at(1) >= kMinDimensionToTransposeTiled &&
         tr->at(2) >= kMinDimensionToTransposeTiled) ||
        (tr->at(1) >= kMinDimensionToTransposeTiled2 &&
         tr->at(2) >= kMinDimensionToTransposeTiled2 &&
         tr->at(1) * tr->at(2) >= kMinTotalDimensionsToTransposeTiled)) {
      return TransposeDescription{&instr, *tr,
                                  /*permutation=*/Vector3{0, 2, 1}};
    }
  }
  if (std::optional<Vector3> tr = ShapeUtil::GetNormalizedTransposeShape(
          instr.operand(0)->shape(), instr.shape(), Vector3{2, 1, 0})) {
    if ((tr->at(0) >= kMinDimensionToTransposeTiled &&
         tr->at(2) >= kMinDimensionToTransposeTiled) ||
        (tr->at(0) >= kMinDimensionToTransposeTiled2 &&
         tr->at(2) >= kMinDimensionToTransposeTiled2 &&
         tr->at(0) * tr->at(2) >= kMinTotalDimensionsToTransposeTiled)) {
      return TransposeDescription{&instr, *tr,
                                  /*permutation=*/Vector3{2, 1, 0}};
    }
  }
  return std::nullopt;
}

// Find 021 or 210 transpose in logical + physical transposition.
std::optional<TransposeDescription> FindTiledLogicalTranspose(
    const HloInstruction& instr) {
  if (instr.opcode() != HloOpcode::kTranspose) {
    return std::nullopt;
  }

  // TODO(cheshire): avoid code duplication.
  if (std::optional<Vector3> tr = ShapeUtil::GetNormalizedLogicalTransposeShape(
          instr.operand(0)->shape(), instr.shape(), instr.dimensions(),
          Vector3{0, 2, 1})) {
    if ((tr->at(1) >= kMinDimensionToTransposeTiled &&
         tr->at(2) >= kMinDimensionToTransposeTiled) ||
        (tr->at(1) >= kMinDimensionToTransposeTiled2 &&
         tr->at(2) >= kMinDimensionToTransposeTiled2 &&
         tr->at(1) * tr->at(2) >= kMinTotalDimensionsToTransposeTiled)) {
      return TransposeDescription{&instr, *tr,
                                  /*permutation=*/Vector3{0, 2, 1}};
    }
  }
  if (std::optional<Vector3> tr = ShapeUtil::GetNormalizedLogicalTransposeShape(
          instr.operand(0)->shape(), instr.shape(), instr.dimensions(),
          Vector3{2, 1, 0})) {
    if ((tr->at(0) >= kMinDimensionToTransposeTiled &&
         tr->at(2) >= kMinDimensionToTransposeTiled) ||
        (tr->at(0) >= kMinDimensionToTransposeTiled2 &&
         tr->at(2) >= kMinDimensionToTransposeTiled2 &&
         tr->at(0) * tr->at(2) >= kMinTotalDimensionsToTransposeTiled)) {
      return TransposeDescription{&instr, *tr,
                                  /*permutation=*/Vector3{2, 1, 0}};
    }
  }
  return std::nullopt;
}

std::optional<TransposeDescription> GetDescriptionForTiledTransposeEmitter(
    const HloInstruction& root, const HloInstruction& hero) {
  // TODO(b/284431534): Figure out how to make the shared memory transpose
  // emitter faster for this case.
  if (hero.shape().element_type() == F32 && root.shape().element_type() == S8) {
    return std::nullopt;
  }

  if (auto d1 = FindTiledTranspose(hero)) {
    return d1;
  }
  if (auto d2 = FindTiledLogicalTranspose(hero)) {
    return d2;
  }
  return std::nullopt;
}

bool IsIntermediate(const HloInstruction* instr, int allowed_operand_count,
                    const HloFusionAdaptor* fusion) {
  // Number of operands should be in range [1, allowed_operand_count].
  if (instr->operand_count() == 0 ||
      instr->operand_count() > allowed_operand_count) {
    return false;
  }

  if (instr->IsElementwise()) {
    // All elementwise ops are considered intermediate, except for copies that
    // modify the layout. Copies that do not modify the layout are used in
    // CopyFusion.
    if (instr->opcode() == HloOpcode::kCopy) {
      return instr->shape() == instr->operand(0)->shape();
    }
    return true;
  }

  // `instr` is a bitcast or a bitcast-like operation.
  switch (instr->opcode()) {
    case HloOpcode::kBitcast:
      return true;
    case HloOpcode::kReshape:
      return ShapeUtil::ReshapeIsBitcast(instr->operand(0)->shape(),
                                         instr->shape());
    case HloOpcode::kTranspose:
      return ShapeUtil::TransposeIsBitcast(instr->operand(0)->shape(),
                                           instr->shape(), instr->dimensions());
    default:
      return false;
  }
}

static bool IsParameter(const HloInstruction& instr) {
  return instr.opcode() == HloOpcode::kParameter;
}

static std::optional<HloInstructionAdaptor> FindTransposeHero(
    HloInstructionAdaptor root, const HloFusionAdaptor& fusion) {
  std::optional<HloInstructionAdaptor> transpose = std::nullopt;
  auto visit = [&transpose](HloInstructionAdaptor node) {
    if (FindTiledLogicalTranspose(node.instruction())) {
      // If we do not find a unique transpose op, use the original non-trivial
      // hero.
      if (transpose) {
        transpose = std::nullopt;
        return TraversalResult::kInterrupt;
      }
      transpose = node;
      return TraversalResult::kSkip;
    }

    if (!IsIntermediate(&node.instruction(), /*allowed_operand_count=*/3)) {
      return TraversalResult::kSkip;
    }
    return TraversalResult::kAdvance;
  };
  HloBfsConsumersFirstTraversal({root}, fusion, visit);
  return transpose;
}

const HloInstruction& FindNonTrivialHero(const HloInstruction& instr,
                                         const HloFusionAdaptor& fusion) {
  HloInstructionAdaptor idx{instr};

  // Go up the chain of trivial element-wise(+bitcast, -copy) operations. Such
  // chains are bound to be quite small, as we restrict the number of users as
  // well. Note that no memoization is needed due to user number constraints: we
  // never have to revisit same nodes.
  auto get_intermediate_arg =
      [&](HloInstructionAdaptor node) -> std::optional<HloInstructionAdaptor> {
    if (IsIntermediate(&node.instruction(), 1, &fusion) &&
        fusion.ContainsInstruction(node.GetOperand(0))) {
      return node.GetOperand(0);
    }
    return std::nullopt;
  };
  while (auto arg = get_intermediate_arg(idx)) {
    idx = *arg;
  }

  // Try a bit harder to find a transpose hero. The shared memory transpose
  // emitter also works if there are ops with more than 1 operand on the path
  // between root and the transpose op, we still want the restriction though
  // that each op on the path is elementwise and has only 1 user.
  std::optional<HloInstructionAdaptor> transpose =
      FindTransposeHero(idx, fusion);
  HloInstructionAdaptor hero = transpose ? *transpose : idx;
  auto visit = [](HloInstructionAdaptor node) {
    return node.instruction().opcode() != HloOpcode::kTuple &&
           node.instruction().opcode() != HloOpcode::kParameter &&
           !IsIntermediate(&node.instruction(),
                           /*allowed_operand_count=*/3);
  };
  bool has_nontrivial_user =
      HloAnyOf(hero.GetUsers(), fusion, visit, /*visit_operands=*/false);
  if (has_nontrivial_user) {
    return instr;
  }
  return hero.instruction();
}

const HloInstruction& FindNonTrivialHero(const HloInstruction& instr) {
  CHECK_NE(instr.opcode(), HloOpcode::kFusion);
  return FindNonTrivialHero(instr,
                            *HloFusionAdaptor::ForComputation(instr.parent()));
}

void VLogModule(int level, const llvm::Module& module) {
  XLA_VLOG_LINES(level, llvm_ir::DumpToString(&module));
}

void VerifyModule(const llvm::Module& module) {
  std::string error_str;
  llvm::raw_string_ostream error_stream(error_str);
  bool broken = llvm::verifyModule(module, &error_stream);
  CHECK(!broken) << error_str;
}

llvm::Type* GetIndexTypeForKernel(const HloInstruction* hlo,
                                  int64_t launch_size, llvm::IRBuilder<>* b) {
  // Find the unnested hlo instruction for which the kernel is generated for.
  const HloInstruction* unnested_hlo = hlo;
  const HloComputation* computation = hlo->parent();
  if (computation->IsFusionComputation()) {
    unnested_hlo = computation->FusionInstruction();
  }

  auto shape_in_range = [&](const Shape& s) {
    bool in_range = true;
    ShapeUtil::ForEachSubshape(s, [&](const Shape& sub_shape,
                                      const ShapeIndex& /*index*/) {
      if (sub_shape.IsArray() && !IsInt32(ShapeUtil::ElementsIn(sub_shape))) {
        in_range = false;
      }
    });

    return in_range;
  };

  llvm::Type* i64_ty = b->getInt64Ty();
  // Check launch dimension
  if (!IsInt32(launch_size)) {
    return i64_ty;
  }

  // Check the size of result tensors
  if (!shape_in_range(unnested_hlo->shape())) {
    return i64_ty;
  }

  auto hlo_shape_in_range = [&](const HloInstruction* operand) -> bool {
    return shape_in_range(operand->shape());
  };

  // Check the size of input tensors
  if (!absl::c_all_of(unnested_hlo->operands(), hlo_shape_in_range)) {
    return i64_ty;
  }

  // Check the size of the internal result tensors
  if (unnested_hlo->opcode() == HloOpcode::kFusion) {
    if (!absl::c_all_of(
            unnested_hlo->fused_instructions_computation()->instructions(),
            hlo_shape_in_range)) {
      return i64_ty;
    }
  }

  return b->getInt32Ty();
}

llvm::Type* GetIndexTypeForKernel(mlir::Operation* op, int64_t launch_size,
                                  llvm::IRBuilder<>* b) {
  auto shape_in_range = [&](const Shape& s) {
    bool in_range = true;
    ShapeUtil::ForEachSubshape(s, [&](const Shape& sub_shape,
                                      const ShapeIndex& /*index*/) {
      if (sub_shape.IsArray() && !IsInt32(ShapeUtil::ElementsIn(sub_shape))) {
        in_range = false;
      }
    });

    return in_range;
  };

  llvm::Type* i64_ty = b->getInt64Ty();
  // Check launch dimension
  if (!IsInt32(launch_size)) {
    return i64_ty;
  }

  // Check the size of result tensors
  for (auto result : GetHloOutputs(op)) {
    if (!shape_in_range(GetShape(result))) {
      return i64_ty;
    }
  }

  auto hlo_shape_in_range = [&](mlir::Value operand) -> bool {
    return shape_in_range(GetShape(operand));
  };

  // Check the size of input tensors
  if (!absl::c_all_of(op->getOperands(), hlo_shape_in_range)) {
    return i64_ty;
  }

  // Check the size of the internal result tensors
  if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
    auto result = fusion.getRegion().walk([&](mlir::Operation* op) {
      for (mlir::Value result : op->getResults()) {
        if (!hlo_shape_in_range(result)) {
          return mlir::WalkResult::interrupt();
        }
      }
      return mlir::WalkResult::advance();
    });
    if (result.wasInterrupted()) {
      return i64_ty;
    }
  }

  return b->getInt32Ty();
}

std::string GetIrNameFromLoc(mlir::Location loc) {
  return llvm_ir::SanitizeConstantName(
      mlir::mhlo::GetDebugNameFromLocation(loc));
}

bool IsAMDGPU(const llvm::Module* module) {
  return llvm::Triple(module->getTargetTriple()).isAMDGPU();
}

bool IsSPIR(const llvm::Module* module) {
  return llvm::Triple(module->getTargetTriple()).isSPIR();
}

absl::StatusOr<DenseDataIntermediate> LiteralToXlaFormat(
    const Literal& literal) {
  PrimitiveType element_type = literal.shape().element_type();
  if (!primitive_util::IsArrayType(element_type)) {
    return Internal("Unsupported type in LiteralToXlaFormat");
  }

  int64_t byte_size = literal.size_bytes();
  if (primitive_util::Is4BitType(element_type)) {
    std::vector<uint8_t> output(CeilOfRatio(byte_size, int64_t{2}));
    absl::Span<char> output_span =
        absl::MakeSpan(reinterpret_cast<char*>(output.data()), output.size());
    PackInt4(
        absl::MakeSpan(reinterpret_cast<const char*>(literal.untyped_data()),
                       byte_size),
        output_span);
    return DenseDataIntermediate::Own(std::move(output));
  }

  return DenseDataIntermediate::Alias(absl::MakeSpan(
      reinterpret_cast<const uint8_t*>(literal.untyped_data()), byte_size));
}

}  // namespace gpu
}  // namespace xla
