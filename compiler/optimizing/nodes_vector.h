/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_OPTIMIZING_NODES_VECTOR_H_
#define ART_COMPILER_OPTIMIZING_NODES_VECTOR_H_

// This #include should never be used by compilation, because this header file (nodes_vector.h)
// is included in the header file nodes.h itself. However it gives editing tools better context.
#include "nodes.h"

namespace art HIDDEN {

// Memory alignment, represented as an offset relative to a base, where 0 <= offset < base,
// and base is a power of two. For example, the value Alignment(16, 0) means memory is
// perfectly aligned at a 16-byte boundary, whereas the value Alignment(16, 4) means
// memory is always exactly 4 bytes above such a boundary.
class Alignment {
 public:
  Alignment(size_t base, size_t offset) : base_(base), offset_(offset) {
    DCHECK_LT(offset, base);
    DCHECK(IsPowerOfTwo(base));
  }

  // Returns true if memory is at least aligned at the given boundary.
  // Assumes requested base is power of two.
  bool IsAlignedAt(size_t base) const {
    DCHECK_NE(0u, base);
    DCHECK(IsPowerOfTwo(base));
    return ((offset_ | base_) & (base - 1u)) == 0;
  }

  size_t Base() const { return base_; }

  size_t Offset() const { return offset_; }

  std::string ToString() const {
    return "ALIGN(" + std::to_string(base_) + "," + std::to_string(offset_) + ")";
  }

  bool operator==(const Alignment& other) const {
    return base_ == other.base_ && offset_ == other.offset_;
  }

 private:
  size_t base_;
  size_t offset_;
};

//
// Definitions of abstract vector operations in HIR.
//

// Abstraction of a vector operation, i.e., an operation that performs
// GetVectorLength() x GetPackedType() operations simultaneously.
class HVecOperation : public HVariableInputSizeInstruction {
 public:
  // A SIMD operation looks like a FPU location.
  // TODO: we could introduce SIMD types in HIR.
  static constexpr DataType::Type kSIMDType = DataType::Type::kFloat64;

  HVecOperation(InstructionKind kind,
                ArenaAllocator* allocator,
                DataType::Type packed_type,
                SideEffects side_effects,
                size_t number_of_inputs,
                size_t vector_length,
                uint32_t dex_pc)
      : HVariableInputSizeInstruction(kind,
                                      kSIMDType,
                                      side_effects,
                                      dex_pc,
                                      allocator,
                                      number_of_inputs,
                                      kArenaAllocVectorNode),
        vector_length_(vector_length) {
    SetPackedField<PackedTypeField>(packed_type);
    // By default vector operations are not predicated.
    SetPackedField<PredicationKindField>(PredicationKind::kNotPredicated);
    DCHECK_LT(1u, vector_length);
  }

  // Predicated instructions execute a corresponding operation only on vector elements which are
  // active (governing predicate is true for that element); the following modes determine what
  // is happening with inactive elements.
  //
  // See HVecPredSetOperation.
  enum class PredicationKind {
    kNotPredicated,        // Instruction doesn't take any predicate as an input.
    kZeroingForm,          // Inactive elements are reset to zero.
    kMergingForm,          // Inactive elements keep their value.
    kLast = kMergingForm,
  };

  PredicationKind GetPredicationKind() const { return GetPackedField<PredicationKindField>(); }

  // Returns whether the vector operation must be predicated in predicated SIMD mode
  // (see CodeGenerator::SupportsPredicatedSIMD). The method reflects semantics of
  // the instruction class rather than the state of a particular instruction instance.
  //
  // This property is introduced for robustness purpose - to maintain and check the invariant:
  // all instructions of the same vector operation class must be either all predicated or all
  // not predicated (depending on the predicated SIMD support) in a correct graph.
  virtual bool MustBePredicatedInPredicatedSIMDMode() {
    return true;
  }

  bool IsPredicated() const {
    return GetPredicationKind() != PredicationKind::kNotPredicated;
  }

  // See HVecPredSetOperation.
  void SetGoverningPredicate(HInstruction* input, PredicationKind pred_kind) {
    DCHECK(!IsPredicated());
    DCHECK(input->IsVecPredSetOperation());
    AddInput(input);
    SetPackedField<PredicationKindField>(pred_kind);
    DCHECK(IsPredicated());
  }

  void SetMergingGoverningPredicate(HInstruction* input) {
    SetGoverningPredicate(input, PredicationKind::kMergingForm);
  }
  void SetZeroingGoverningPredicate(HInstruction* input) {
    SetGoverningPredicate(input, PredicationKind::kZeroingForm);
  }

  // See HVecPredSetOperation.
  HVecPredSetOperation* GetGoverningPredicate() const {
    DCHECK(IsPredicated());
    HInstruction* pred_input = InputAt(InputCount() - 1);
    DCHECK(pred_input->IsVecPredSetOperation());
    return pred_input->AsVecPredSetOperation();
  }

  // Returns whether two vector operations are predicated by the same vector predicate
  // with the same predication type.
  static bool HaveSamePredicate(HVecOperation* instr0, HVecOperation* instr1) {
    HVecPredSetOperation* instr0_predicate = instr0->GetGoverningPredicate();
    HVecOperation::PredicationKind instr0_predicate_kind = instr0->GetPredicationKind();
    return instr1->GetGoverningPredicate() == instr0_predicate &&
           instr1->GetPredicationKind() == instr0_predicate_kind;
  }

  // Returns the number of elements packed in a vector.
  size_t GetVectorLength() const {
    return vector_length_;
  }

  // Returns the number of bytes in a full vector.
  size_t GetVectorNumberOfBytes() const {
    return vector_length_ * DataType::Size(GetPackedType());
  }

  // Returns the true component type packed in a vector.
  DataType::Type GetPackedType() const {
    return GetPackedField<PackedTypeField>();
  }

  // Assumes vector nodes cannot be moved by default. Each concrete implementation
  // that can be moved should override this method and return true.
  //
  // Note: similar approach is used for instruction scheduling (if it is turned on for the target):
  // by default HScheduler::IsSchedulable returns false for a particular HVecOperation.
  // HScheduler${ARCH}::IsSchedulable can be overridden to return true for an instruction (see
  // scheduler_arm64.h for example) if it is safe to schedule it; in this case one *must* also
  // look at/update HScheduler${ARCH}::IsSchedulingBarrier for this instruction.
  //
  // Note: For newly introduced vector instructions HScheduler${ARCH}::IsSchedulingBarrier must be
  // altered to return true if the instruction might reside outside the SIMD loop body since SIMD
  // registers are not kept alive across vector loop boundaries (yet).
  bool CanBeMoved() const override { return false; }

  // Tests if all data of a vector node (vector length and packed type) is equal.
  // Each concrete implementation that adds more fields should test equality of
  // those fields in its own method *and* call all super methods.
  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsVecOperation());
    const HVecOperation* o = other->AsVecOperation();
    return GetVectorLength() == o->GetVectorLength() && GetPackedType() == o->GetPackedType();
  }

  // Maps an integral type to the same-size signed type and leaves other types alone.
  static DataType::Type ToSignedType(DataType::Type type) {
    switch (type) {
      case DataType::Type::kBool:  // 1-byte storage unit
      case DataType::Type::kUint8:
        return DataType::Type::kInt8;
      case DataType::Type::kUint16:
        return DataType::Type::kInt16;
      default:
        DCHECK(type != DataType::Type::kVoid && type != DataType::Type::kReference) << type;
        return type;
    }
  }

  // Maps an integral type to the same-size unsigned type and leaves other types alone.
  static DataType::Type ToUnsignedType(DataType::Type type) {
    switch (type) {
      case DataType::Type::kBool:  // 1-byte storage unit
      case DataType::Type::kInt8:
        return DataType::Type::kUint8;
      case DataType::Type::kInt16:
        return DataType::Type::kUint16;
      default:
        DCHECK(type != DataType::Type::kVoid && type != DataType::Type::kReference) << type;
        return type;
    }
  }

  // Maps an integral type to the same-size (un)signed type. Leaves other types alone.
  static DataType::Type ToProperType(DataType::Type type, bool is_unsigned) {
    return is_unsigned ? ToUnsignedType(type) : ToSignedType(type);
  }

  // Helper method to determine if an instruction returns a SIMD value.
  // TODO: This method is needed until we introduce SIMD as proper type.
  static bool ReturnsSIMDValue(HInstruction* instruction) {
    if (instruction->IsVecOperation()) {
      return !instruction->IsVecExtractScalar();  // only scalar returning vec op
    } else if (instruction->IsPhi()) {
      // Vectorizer only uses Phis in reductions, so checking for a 2-way phi
      // with a direct vector operand as second argument suffices.
      return
          instruction->GetType() == kSIMDType &&
          instruction->InputCount() == 2 &&
          instruction->InputAt(1)->IsVecOperation();
    }
    return false;
  }

  DECLARE_ABSTRACT_INSTRUCTION(VecOperation);

 protected:
  // Additional packed bits.
  static constexpr size_t kPredicationKind = HInstruction::kNumberOfGenericPackedBits;
  static constexpr size_t kPredicationKindSize =
      MinimumBitsToStore(static_cast<size_t>(PredicationKind::kLast));
  static constexpr size_t kFieldPackedType = kPredicationKind + kPredicationKindSize;
  static constexpr size_t kFieldPackedTypeSize =
      MinimumBitsToStore(static_cast<size_t>(DataType::Type::kLast));
  static constexpr size_t kNumberOfVectorOpPackedBits = kFieldPackedType + kFieldPackedTypeSize;
  static_assert(kNumberOfVectorOpPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using PackedTypeField = BitField<DataType::Type, kFieldPackedType, kFieldPackedTypeSize>;
  using PredicationKindField = BitField<PredicationKind, kPredicationKind, kPredicationKindSize>;

  DEFAULT_COPY_CONSTRUCTOR(VecOperation);

 private:
  const size_t vector_length_;
};

// Abstraction of a unary vector operation.
class HVecUnaryOperation : public HVecOperation {
 public:
  HVecUnaryOperation(InstructionKind kind,
                     ArenaAllocator* allocator,
                     HInstruction* input,
                     DataType::Type packed_type,
                     size_t vector_length,
                     uint32_t dex_pc)
      : HVecOperation(kind,
                      allocator,
                      packed_type,
                      SideEffects::None(),
                      /* number_of_inputs= */ 1,
                      vector_length,
                      dex_pc) {
    SetRawInputAt(0, input);
  }

  HInstruction* GetInput() const { return InputAt(0); }

  DECLARE_ABSTRACT_INSTRUCTION(VecUnaryOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecUnaryOperation);
};

// Abstraction of a binary vector operation.
class HVecBinaryOperation : public HVecOperation {
 public:
  HVecBinaryOperation(InstructionKind kind,
                      ArenaAllocator* allocator,
                      HInstruction* left,
                      HInstruction* right,
                      DataType::Type packed_type,
                      size_t vector_length,
                      uint32_t dex_pc)
      : HVecOperation(kind,
                      allocator,
                      packed_type,
                      SideEffects::None(),
                      /* number_of_inputs= */ 2,
                      vector_length,
                      dex_pc) {
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  HInstruction* GetLeft() const { return InputAt(0); }
  HInstruction* GetRight() const { return InputAt(1); }

  DECLARE_ABSTRACT_INSTRUCTION(VecBinaryOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecBinaryOperation);
};

// Abstraction of a vector operation that references memory, with an alignment.
// The Android runtime guarantees elements have at least natural alignment.
class HVecMemoryOperation : public HVecOperation {
 public:
  HVecMemoryOperation(InstructionKind kind,
                      ArenaAllocator* allocator,
                      DataType::Type packed_type,
                      SideEffects side_effects,
                      size_t number_of_inputs,
                      size_t vector_length,
                      uint32_t dex_pc)
      : HVecOperation(kind,
                      allocator,
                      packed_type,
                      side_effects,
                      number_of_inputs,
                      vector_length,
                      dex_pc),
        alignment_(DataType::Size(packed_type), 0) {
    DCHECK_GE(number_of_inputs, 2u);
  }

  void SetAlignment(Alignment alignment) { alignment_ = alignment; }

  Alignment GetAlignment() const { return alignment_; }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsVecMemoryOperation());
    const HVecMemoryOperation* o = other->AsVecMemoryOperation();
    return HVecOperation::InstructionDataEquals(o) && GetAlignment() == o->GetAlignment();
  }

  DECLARE_ABSTRACT_INSTRUCTION(VecMemoryOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecMemoryOperation);

 private:
  Alignment alignment_;
};

// Packed type consistency checker ("same vector length" integral types may mix freely).
// Tests relaxed type consistency in which packed same-size integral types can co-exist,
// but other type mixes are an error.
inline static bool HasConsistentPackedTypes(HInstruction* input, DataType::Type type) {
  if (input->IsPhi()) {
    return input->GetType() == HVecOperation::kSIMDType;  // carries SIMD
  }
  DCHECK(input->IsVecOperation());
  DataType::Type input_type = input->AsVecOperation()->GetPackedType();
  DCHECK_EQ(HVecOperation::ToUnsignedType(input_type) == HVecOperation::ToUnsignedType(type),
            HVecOperation::ToSignedType(input_type) == HVecOperation::ToSignedType(type));
  return HVecOperation::ToSignedType(input_type) == HVecOperation::ToSignedType(type);
}

//
// Definitions of concrete unary vector operations in HIR.
//

// Replicates the given scalar into a vector,
// viz. replicate(x) = [ x, .. , x ].
class HVecReplicateScalar final : public HVecUnaryOperation {
 public:
  HVecReplicateScalar(ArenaAllocator* allocator,
                      HInstruction* scalar,
                      DataType::Type packed_type,
                      size_t vector_length,
                      uint32_t dex_pc)
      : HVecUnaryOperation(
            kVecReplicateScalar, allocator, scalar, packed_type, vector_length, dex_pc) {
    DCHECK(!ReturnsSIMDValue(scalar));
  }

  // A replicate needs to stay in place, since SIMD registers are not
  // kept alive across vector loop boundaries (yet).
  bool CanBeMoved() const override { return false; }

  DECLARE_INSTRUCTION(VecReplicateScalar);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecReplicateScalar);
};

// Extracts a particular scalar from the given vector,
// viz. extract[ x1, .. , xn ] = x_i.
//
// TODO: for now only i == 1 case supported.
class HVecExtractScalar final : public HVecUnaryOperation {
 public:
  HVecExtractScalar(ArenaAllocator* allocator,
                    HInstruction* input,
                    DataType::Type packed_type,
                    size_t vector_length,
                    size_t index,
                    uint32_t dex_pc)
      : HVecUnaryOperation(
            kVecExtractScalar, allocator, input, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(input, packed_type));
    DCHECK_LT(index, vector_length);
    DCHECK_EQ(index, 0u);
    // Yields a single component in the vector.
    // Overrides the kSIMDType set by the VecOperation constructor.
    SetPackedField<TypeField>(packed_type);
  }

  // An extract needs to stay in place, since SIMD registers are not
  // kept alive across vector loop boundaries (yet).
  bool CanBeMoved() const override { return false; }

  DECLARE_INSTRUCTION(VecExtractScalar);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecExtractScalar);
};

// Reduces the given vector into the first element as sum/min/max,
// viz. sum-reduce[ x1, .. , xn ] = [ y, ---- ], where y = sum xi
// and the "-" denotes "don't care" (implementation dependent).
class HVecReduce final : public HVecUnaryOperation {
 public:
  enum ReductionKind {
    kSum = 1,
    kMin = 2,
    kMax = 3
  };

  HVecReduce(ArenaAllocator* allocator,
             HInstruction* input,
             DataType::Type packed_type,
             size_t vector_length,
             ReductionKind reduction_kind,
             uint32_t dex_pc)
      : HVecUnaryOperation(kVecReduce, allocator, input, packed_type, vector_length, dex_pc),
        reduction_kind_(reduction_kind) {
    DCHECK(HasConsistentPackedTypes(input, packed_type));
  }

  ReductionKind GetReductionKind() const { return reduction_kind_; }

  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsVecReduce());
    const HVecReduce* o = other->AsVecReduce();
    return HVecOperation::InstructionDataEquals(o) && GetReductionKind() == o->GetReductionKind();
  }

  DECLARE_INSTRUCTION(VecReduce);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecReduce);

 private:
  const ReductionKind reduction_kind_;
};

// Converts every component in the vector,
// viz. cnv[ x1, .. , xn ]  = [ cnv(x1), .. , cnv(xn) ].
class HVecCnv final : public HVecUnaryOperation {
 public:
  HVecCnv(ArenaAllocator* allocator,
          HInstruction* input,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecUnaryOperation(kVecCnv, allocator, input, packed_type, vector_length, dex_pc) {
    DCHECK(input->IsVecOperation());
    DCHECK_NE(GetInputType(), GetResultType());  // actual convert
  }

  DataType::Type GetInputType() const { return InputAt(0)->AsVecOperation()->GetPackedType(); }
  DataType::Type GetResultType() const { return GetPackedType(); }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecCnv);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecCnv);
};

// Negates every component in the vector,
// viz. neg[ x1, .. , xn ]  = [ -x1, .. , -xn ].
class HVecNeg final : public HVecUnaryOperation {
 public:
  HVecNeg(ArenaAllocator* allocator,
          HInstruction* input,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecUnaryOperation(kVecNeg, allocator, input, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(input, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecNeg);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecNeg);
};

// Takes absolute value of every component in the vector,
// viz. abs[ x1, .. , xn ]  = [ |x1|, .. , |xn| ]
// for signed operand x.
class HVecAbs final : public HVecUnaryOperation {
 public:
  HVecAbs(ArenaAllocator* allocator,
          HInstruction* input,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecUnaryOperation(kVecAbs, allocator, input, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(input, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecAbs);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecAbs);
};

// Bitwise- or boolean-nots every component in the vector,
// viz. not[ x1, .. , xn ]  = [ ~x1, .. , ~xn ], or
//      not[ x1, .. , xn ]  = [ !x1, .. , !xn ] for boolean.
class HVecNot final : public HVecUnaryOperation {
 public:
  HVecNot(ArenaAllocator* allocator,
          HInstruction* input,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecUnaryOperation(kVecNot, allocator, input, packed_type, vector_length, dex_pc) {
    DCHECK(input->IsVecOperation());
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecNot);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecNot);
};

//
// Definitions of concrete binary vector operations in HIR.
//

// Adds every component in the two vectors,
// viz. [ x1, .. , xn ] + [ y1, .. , yn ] = [ x1 + y1, .. , xn + yn ].
class HVecAdd final : public HVecBinaryOperation {
 public:
  HVecAdd(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecAdd, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecAdd);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecAdd);
};

// Adds every component in the two vectors using saturation arithmetic,
// viz. [ x1, .. , xn ] + [ y1, .. , yn ] = [ x1 +_sat y1, .. , xn +_sat yn ]
// for either both signed or both unsigned operands x, y (reflected in packed_type).
class HVecSaturationAdd final : public HVecBinaryOperation {
 public:
  HVecSaturationAdd(ArenaAllocator* allocator,
                    HInstruction* left,
                    HInstruction* right,
                    DataType::Type packed_type,
                    size_t vector_length,
                    uint32_t dex_pc)
      : HVecBinaryOperation(
          kVecSaturationAdd, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecSaturationAdd);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecSaturationAdd);
};

// Performs halving add on every component in the two vectors, viz.
// rounded   [ x1, .. , xn ] hradd [ y1, .. , yn ] = [ (x1 + y1 + 1) >> 1, .. , (xn + yn + 1) >> 1 ]
// truncated [ x1, .. , xn ] hadd  [ y1, .. , yn ] = [ (x1 + y1)     >> 1, .. , (xn + yn )    >> 1 ]
// for either both signed or both unsigned operands x, y (reflected in packed_type).
class HVecHalvingAdd final : public HVecBinaryOperation {
 public:
  HVecHalvingAdd(ArenaAllocator* allocator,
                 HInstruction* left,
                 HInstruction* right,
                 DataType::Type packed_type,
                 size_t vector_length,
                 bool is_rounded,
                 uint32_t dex_pc)
      : HVecBinaryOperation(
            kVecHalvingAdd, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
    SetPackedFlag<kFieldHAddIsRounded>(is_rounded);
  }

  bool IsRounded() const { return GetPackedFlag<kFieldHAddIsRounded>(); }

  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsVecHalvingAdd());
    const HVecHalvingAdd* o = other->AsVecHalvingAdd();
    return HVecOperation::InstructionDataEquals(o) && IsRounded() == o->IsRounded();
  }

  DECLARE_INSTRUCTION(VecHalvingAdd);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecHalvingAdd);

 private:
  // Additional packed bits.
  static constexpr size_t kFieldHAddIsRounded = HVecOperation::kNumberOfVectorOpPackedBits;
  static constexpr size_t kNumberOfHAddPackedBits = kFieldHAddIsRounded + 1;
  static_assert(kNumberOfHAddPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
};

// Subtracts every component in the two vectors,
// viz. [ x1, .. , xn ] - [ y1, .. , yn ] = [ x1 - y1, .. , xn - yn ].
class HVecSub final : public HVecBinaryOperation {
 public:
  HVecSub(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecSub, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecSub);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecSub);
};

// Subtracts every component in the two vectors using saturation arithmetic,
// viz. [ x1, .. , xn ] + [ y1, .. , yn ] = [ x1 -_sat y1, .. , xn -_sat yn ]
// for either both signed or both unsigned operands x, y (reflected in packed_type).
class HVecSaturationSub final : public HVecBinaryOperation {
 public:
  HVecSaturationSub(ArenaAllocator* allocator,
                    HInstruction* left,
                    HInstruction* right,
                    DataType::Type packed_type,
                    size_t vector_length,
                    uint32_t dex_pc)
      : HVecBinaryOperation(
          kVecSaturationSub, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecSaturationSub);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecSaturationSub);
};

// Multiplies every component in the two vectors,
// viz. [ x1, .. , xn ] * [ y1, .. , yn ] = [ x1 * y1, .. , xn * yn ].
class HVecMul final : public HVecBinaryOperation {
 public:
  HVecMul(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecMul, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecMul);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecMul);
};

// Divides every component in the two vectors,
// viz. [ x1, .. , xn ] / [ y1, .. , yn ] = [ x1 / y1, .. , xn / yn ].
class HVecDiv final : public HVecBinaryOperation {
 public:
  HVecDiv(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecDiv, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecDiv);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecDiv);
};

// Takes minimum of every component in the two vectors,
// viz. MIN( [ x1, .. , xn ] , [ y1, .. , yn ]) = [ min(x1, y1), .. , min(xn, yn) ]
// for either both signed or both unsigned operands x, y (reflected in packed_type).
class HVecMin final : public HVecBinaryOperation {
 public:
  HVecMin(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecMin, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecMin);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecMin);
};

// Takes maximum of every component in the two vectors,
// viz. MAX( [ x1, .. , xn ] , [ y1, .. , yn ]) = [ max(x1, y1), .. , max(xn, yn) ]
// for either both signed or both unsigned operands x, y (reflected in packed_type).
class HVecMax final : public HVecBinaryOperation {
 public:
  HVecMax(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecMax, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
    DCHECK(HasConsistentPackedTypes(right, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecMax);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecMax);
};

// Bitwise-ands every component in the two vectors,
// viz. [ x1, .. , xn ] & [ y1, .. , yn ] = [ x1 & y1, .. , xn & yn ].
class HVecAnd final : public HVecBinaryOperation {
 public:
  HVecAnd(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecAnd, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(left->IsVecOperation() && right->IsVecOperation());
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecAnd);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecAnd);
};

// Bitwise-and-nots every component in the two vectors,
// viz. [ x1, .. , xn ] and-not [ y1, .. , yn ] = [ ~x1 & y1, .. , ~xn & yn ].
class HVecAndNot final : public HVecBinaryOperation {
 public:
  HVecAndNot(ArenaAllocator* allocator,
             HInstruction* left,
             HInstruction* right,
             DataType::Type packed_type,
             size_t vector_length,
             uint32_t dex_pc)
         : HVecBinaryOperation(
               kVecAndNot, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(left->IsVecOperation() && right->IsVecOperation());
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecAndNot);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecAndNot);
};

// Bitwise-ors every component in the two vectors,
// viz. [ x1, .. , xn ] | [ y1, .. , yn ] = [ x1 | y1, .. , xn | yn ].
class HVecOr final : public HVecBinaryOperation {
 public:
  HVecOr(ArenaAllocator* allocator,
         HInstruction* left,
         HInstruction* right,
         DataType::Type packed_type,
         size_t vector_length,
         uint32_t dex_pc)
      : HVecBinaryOperation(kVecOr, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(left->IsVecOperation() && right->IsVecOperation());
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecOr);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecOr);
};

// Bitwise-xors every component in the two vectors,
// viz. [ x1, .. , xn ] ^ [ y1, .. , yn ] = [ x1 ^ y1, .. , xn ^ yn ].
class HVecXor final : public HVecBinaryOperation {
 public:
  HVecXor(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecXor, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(left->IsVecOperation() && right->IsVecOperation());
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecXor);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecXor);
};

// Logically shifts every component in the vector left by the given distance,
// viz. [ x1, .. , xn ] << d = [ x1 << d, .. , xn << d ].
class HVecShl final : public HVecBinaryOperation {
 public:
  HVecShl(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecShl, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecShl);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecShl);
};

// Arithmetically shifts every component in the vector right by the given distance,
// viz. [ x1, .. , xn ] >> d = [ x1 >> d, .. , xn >> d ].
class HVecShr final : public HVecBinaryOperation {
 public:
  HVecShr(ArenaAllocator* allocator,
          HInstruction* left,
          HInstruction* right,
          DataType::Type packed_type,
          size_t vector_length,
          uint32_t dex_pc)
      : HVecBinaryOperation(kVecShr, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecShr);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecShr);
};

// Logically shifts every component in the vector right by the given distance,
// viz. [ x1, .. , xn ] >>> d = [ x1 >>> d, .. , xn >>> d ].
class HVecUShr final : public HVecBinaryOperation {
 public:
  HVecUShr(ArenaAllocator* allocator,
           HInstruction* left,
           HInstruction* right,
           DataType::Type packed_type,
           size_t vector_length,
           uint32_t dex_pc)
      : HVecBinaryOperation(kVecUShr, allocator, left, right, packed_type, vector_length, dex_pc) {
    DCHECK(HasConsistentPackedTypes(left, packed_type));
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecUShr);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecUShr);
};

//
// Definitions of concrete miscellaneous vector operations in HIR.
//

// Assigns the given scalar elements to a vector,
// viz. set( array(x1, .. , xn) ) = [ x1, .. ,            xn ] if n == m,
//      set( array(x1, .. , xm) ) = [ x1, .. , xm, 0, .. , 0 ] if m <  n.
class HVecSetScalars final : public HVecOperation {
 public:
  HVecSetScalars(ArenaAllocator* allocator,
                 HInstruction* scalars[],
                 DataType::Type packed_type,
                 size_t vector_length,
                 size_t number_of_scalars,
                 uint32_t dex_pc)
      : HVecOperation(kVecSetScalars,
                      allocator,
                      packed_type,
                      SideEffects::None(),
                      number_of_scalars,
                      vector_length,
                      dex_pc) {
    for (size_t i = 0; i < number_of_scalars; i++) {
      DCHECK(!ReturnsSIMDValue(scalars[i]));
      SetRawInputAt(0, scalars[i]);
    }
  }

  // Setting scalars needs to stay in place, since SIMD registers are not
  // kept alive across vector loop boundaries (yet).
  bool CanBeMoved() const override { return false; }

  DECLARE_INSTRUCTION(VecSetScalars);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecSetScalars);
};

// Multiplies every component in the two vectors, adds the result vector to the accumulator vector,
// viz. [ a1, .. , an ] + [ x1, .. , xn ] * [ y1, .. , yn ] = [ a1 + x1 * y1, .. , an + xn * yn ].
// For floating point types, Java rounding behavior must be preserved; the products are rounded to
// the proper precision before being added. "Fused" multiply-add operations available on several
// architectures are not usable since they would violate Java language rules.
class HVecMultiplyAccumulate final : public HVecOperation {
 public:
  HVecMultiplyAccumulate(ArenaAllocator* allocator,
                         InstructionKind op,
                         HInstruction* accumulator,
                         HInstruction* mul_left,
                         HInstruction* mul_right,
                         DataType::Type packed_type,
                         size_t vector_length,
                         uint32_t dex_pc)
      : HVecOperation(kVecMultiplyAccumulate,
                      allocator,
                      packed_type,
                      SideEffects::None(),
                      /* number_of_inputs= */ 3,
                      vector_length,
                      dex_pc),
        op_kind_(op) {
    DCHECK(op == InstructionKind::kAdd || op == InstructionKind::kSub);
    DCHECK(HasConsistentPackedTypes(accumulator, packed_type));
    DCHECK(HasConsistentPackedTypes(mul_left, packed_type));
    DCHECK(HasConsistentPackedTypes(mul_right, packed_type));
    // Remove the following if we add an architecture that supports floating point multiply-add
    // with Java-compatible rounding.
    DCHECK(DataType::IsIntegralType(packed_type));
    SetRawInputAt(0, accumulator);
    SetRawInputAt(1, mul_left);
    SetRawInputAt(2, mul_right);
  }

  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsVecMultiplyAccumulate());
    const HVecMultiplyAccumulate* o = other->AsVecMultiplyAccumulate();
    return HVecOperation::InstructionDataEquals(o) && GetOpKind() == o->GetOpKind();
  }

  InstructionKind GetOpKind() const { return op_kind_; }

  DECLARE_INSTRUCTION(VecMultiplyAccumulate);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecMultiplyAccumulate);

 private:
  // Indicates if this is a MADD or MSUB.
  const InstructionKind op_kind_;
};

// Takes the absolute difference of two vectors, and adds the results to
// same-precision or wider-precision components in the accumulator,
// viz. SAD([ a1, .. , am ], [ x1, .. , xn ], [ y1, .. , yn ]) =
//          [ a1 + sum abs(xi-yi), .. , am + sum abs(xj-yj) ],
//      for m <= n, non-overlapping sums, and signed operands x, y.
class HVecSADAccumulate final : public HVecOperation {
 public:
  HVecSADAccumulate(ArenaAllocator* allocator,
                    HInstruction* accumulator,
                    HInstruction* sad_left,
                    HInstruction* sad_right,
                    DataType::Type packed_type,
                    size_t vector_length,
                    uint32_t dex_pc)
      : HVecOperation(kVecSADAccumulate,
                      allocator,
                      packed_type,
                      SideEffects::None(),
                      /* number_of_inputs= */ 3,
                      vector_length,
                      dex_pc) {
    DCHECK(HasConsistentPackedTypes(accumulator, packed_type));
    DCHECK(sad_left->IsVecOperation());
    DCHECK(sad_right->IsVecOperation());
    DCHECK_EQ(ToSignedType(sad_left->AsVecOperation()->GetPackedType()),
              ToSignedType(sad_right->AsVecOperation()->GetPackedType()));
    SetRawInputAt(0, accumulator);
    SetRawInputAt(1, sad_left);
    SetRawInputAt(2, sad_right);
  }

  DECLARE_INSTRUCTION(VecSADAccumulate);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecSADAccumulate);
};

// Performs dot product of two vectors and adds the result to wider precision components in
// the accumulator.
//
// viz. DOT_PRODUCT([ a1, .. , am], [ x1, .. , xn ], [ y1, .. , yn ]) =
//                  [ a1 + sum(xi * yi), .. , am + sum(xj * yj) ],
//      for m <= n, non-overlapping sums,
//      for either both signed or both unsigned operands x, y.
//
// Notes:
//   - packed type reflects the type of sum reduction, not the type of the operands.
//   - IsZeroExtending() is used to determine the kind of signed/zero extension to be
//     performed for the operands.
//
// TODO: Support types other than kInt32 for packed type.
class HVecDotProd final : public HVecOperation {
 public:
  HVecDotProd(ArenaAllocator* allocator,
              HInstruction* accumulator,
              HInstruction* left,
              HInstruction* right,
              DataType::Type packed_type,
              bool is_zero_extending,
              size_t vector_length,
              uint32_t dex_pc)
    : HVecOperation(kVecDotProd,
                    allocator,
                    packed_type,
                    SideEffects::None(),
                    /* number_of_inputs= */ 3,
                    vector_length,
                    dex_pc) {
    DCHECK(HasConsistentPackedTypes(accumulator, packed_type));
    DCHECK(DataType::IsIntegralType(packed_type));
    DCHECK(left->IsVecOperation());
    DCHECK(right->IsVecOperation());
    DCHECK_EQ(ToSignedType(left->AsVecOperation()->GetPackedType()),
              ToSignedType(right->AsVecOperation()->GetPackedType()));
    SetRawInputAt(0, accumulator);
    SetRawInputAt(1, left);
    SetRawInputAt(2, right);
    SetPackedFlag<kFieldHDotProdIsZeroExtending>(is_zero_extending);
  }

  bool IsZeroExtending() const { return GetPackedFlag<kFieldHDotProdIsZeroExtending>(); }

  bool CanBeMoved() const override { return true; }

  DECLARE_INSTRUCTION(VecDotProd);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecDotProd);

 private:
  // Additional packed bits.
  static constexpr size_t kFieldHDotProdIsZeroExtending =
      HVecOperation::kNumberOfVectorOpPackedBits;
  static constexpr size_t kNumberOfHDotProdPackedBits = kFieldHDotProdIsZeroExtending + 1;
  static_assert(kNumberOfHDotProdPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
};

// Loads a vector from memory, viz. load(mem, 1)
// yield the vector [ mem(1), .. , mem(n) ].
class HVecLoad final : public HVecMemoryOperation {
 public:
  HVecLoad(ArenaAllocator* allocator,
           HInstruction* base,
           HInstruction* index,
           DataType::Type packed_type,
           SideEffects side_effects,
           size_t vector_length,
           bool is_string_char_at,
           uint32_t dex_pc)
      : HVecMemoryOperation(kVecLoad,
                            allocator,
                            packed_type,
                            side_effects,
                            /* number_of_inputs= */ 2,
                            vector_length,
                            dex_pc) {
    SetRawInputAt(0, base);
    SetRawInputAt(1, index);
    SetPackedFlag<kFieldIsStringCharAt>(is_string_char_at);
  }

  bool IsStringCharAt() const { return GetPackedFlag<kFieldIsStringCharAt>(); }

  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsVecLoad());
    const HVecLoad* o = other->AsVecLoad();
    return HVecMemoryOperation::InstructionDataEquals(o) && IsStringCharAt() == o->IsStringCharAt();
  }

  DECLARE_INSTRUCTION(VecLoad);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecLoad);

 private:
  // Additional packed bits.
  static constexpr size_t kFieldIsStringCharAt = HVecOperation::kNumberOfVectorOpPackedBits;
  static constexpr size_t kNumberOfVecLoadPackedBits = kFieldIsStringCharAt + 1;
  static_assert(kNumberOfVecLoadPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
};

// Stores a vector to memory, viz. store(m, 1, [x1, .. , xn] )
// sets mem(1) = x1, .. , mem(n) = xn.
class HVecStore final : public HVecMemoryOperation {
 public:
  HVecStore(ArenaAllocator* allocator,
            HInstruction* base,
            HInstruction* index,
            HInstruction* value,
            DataType::Type packed_type,
            SideEffects side_effects,
            size_t vector_length,
            uint32_t dex_pc)
      : HVecMemoryOperation(kVecStore,
                            allocator,
                            packed_type,
                            side_effects,
                            /* number_of_inputs= */ 3,
                            vector_length,
                            dex_pc) {
    DCHECK(HasConsistentPackedTypes(value, packed_type));
    SetRawInputAt(0, base);
    SetRawInputAt(1, index);
    SetRawInputAt(2, value);
  }

  // A store needs to stay in place.
  bool CanBeMoved() const override { return false; }

  HInstruction* GetValue() const { return InputAt(2); }

  DECLARE_INSTRUCTION(VecStore);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecStore)
};

//
// 'Predicate-setting' instructions.
//

// An abstract class for instructions for which the output value is a vector predicate -
// a special kind of vector value:
//
//    viz. [ p1, .. , pn ], where p_i is from { 0, 1 }.
//
// A VecOperation OP executes the same operation (e.g. ADD) on multiple elements of the vector.
// It can be either unpredicated (operation is done on ALL of the elements) or predicated (only
// on SOME elements, determined by a special extra input - vector predicate).
// Implementations can vary depending on the ISA; the general idea is that for each element of the
// regular vector a vector predicate has a corresponding element with either 0 or 1.
// The value determines whether a vector element will be involved in OP calculations or not
// (active or inactive). A vector predicate is referred as governing one if it is used to
// control the execution of a predicated instruction.
//
// Note: vector predicate value type is introduced alongside existing vectors of booleans and
// vectors of bytes to reflect their special semantics.
//
// TODO: we could introduce SIMD types in HIR.
class HVecPredSetOperation : public HVecOperation {
 public:
  // A vector predicate-setting operation looks like a Int64 location.
  // TODO: we could introduce vector types in HIR.
  static constexpr DataType::Type kSIMDPredType = DataType::Type::kInt64;

  HVecPredSetOperation(InstructionKind kind,
                       ArenaAllocator* allocator,
                       DataType::Type packed_type,
                       SideEffects side_effects,
                       size_t number_of_inputs,
                       size_t vector_length,
                       uint32_t dex_pc)
      : HVecOperation(kind,
                      allocator,
                      packed_type,
                      side_effects,
                      number_of_inputs,
                      vector_length,
                      dex_pc) {
    // Overrides the kSIMDType set by the VecOperation constructor.
    SetPackedField<TypeField>(kSIMDPredType);
  }

  bool CanBeMoved() const override { return true; }

  DECLARE_ABSTRACT_INSTRUCTION(VecPredSetOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecPredSetOperation);
};

// Sets all the vector predicate elements as active or inactive.
//
// viz. [ p1, .. , pn ]  = [ val, .. , val ] where val is from { 1, 0 }.
class HVecPredSetAll final : public HVecPredSetOperation {
 public:
  HVecPredSetAll(ArenaAllocator* allocator,
                 HInstruction* input,
                 DataType::Type packed_type,
                 size_t vector_length,
                 uint32_t dex_pc) :
      HVecPredSetOperation(kVecPredSetAll,
                           allocator,
                           packed_type,
                           SideEffects::None(),
                           /* number_of_inputs= */ 1,
                           vector_length,
                           dex_pc) {
    DCHECK(input->IsIntConstant());
    SetRawInputAt(0, input);
    MarkEmittedAtUseSite();
  }

  // Having governing predicate doesn't make sense for set all TRUE/FALSE instruction.
  bool MustBePredicatedInPredicatedSIMDMode() override { return false; }

  bool IsSetTrue() const { return InputAt(0)->AsIntConstant()->IsTrue(); }

  // Vector predicates are not kept alive across vector loop boundaries.
  bool CanBeMoved() const override { return false; }

  DECLARE_INSTRUCTION(VecPredSetAll);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecPredSetAll);
};

//
// Arm64 SVE-specific instructions.
//
// Classes of instructions which are specific to Arm64 SVE (though could be adopted
// by other targets, possibly being lowered to a number of ISA instructions) and
// implement SIMD loop predicated execution idiom.
//

// Takes two scalar values x and y, creates a vector S: s(n) = x + n, compares (OP) each s(n)
// with y and set the corresponding element of the predicate register to the result of the
// comparison.
//
// viz. [ p1, .. , pn ]  = [ x OP y , (x + 1) OP y, .. , (x + n) OP y ] where OP is CondKind
// condition.
class HVecPredWhile final : public HVecPredSetOperation {
 public:
  enum class CondKind {
    kLE,   // signed less than or equal.
    kLO,   // unsigned lower.
    kLS,   // unsigned lower or same.
    kLT,   // signed less.
    kLast = kLT,
  };

  HVecPredWhile(ArenaAllocator* allocator,
                HInstruction* left,
                HInstruction* right,
                CondKind cond,
                DataType::Type packed_type,
                size_t vector_length,
                uint32_t dex_pc) :
      HVecPredSetOperation(kVecPredWhile,
                           allocator,
                           packed_type,
                           SideEffects::None(),
                           /* number_of_inputs= */ 2,
                           vector_length,
                           dex_pc) {
    DCHECK(!left->IsVecOperation());
    DCHECK(!left->IsVecPredSetOperation());
    DCHECK(!right->IsVecOperation());
    DCHECK(!right->IsVecPredSetOperation());
    DCHECK(DataType::IsIntegralType(left->GetType()));
    DCHECK(DataType::IsIntegralType(right->GetType()));
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
    SetPackedField<CondKindField>(cond);
  }

  // This is a special loop control instruction which must not be predicated.
  bool MustBePredicatedInPredicatedSIMDMode() override { return false; }

  CondKind GetCondKind() const {
    return GetPackedField<CondKindField>();
  }

  DECLARE_INSTRUCTION(VecPredWhile);

 protected:
  // Additional packed bits.
  static constexpr size_t kCondKind = HVecOperation::kNumberOfVectorOpPackedBits;
  static constexpr size_t kCondKindSize =
      MinimumBitsToStore(static_cast<size_t>(CondKind::kLast));
  static constexpr size_t kNumberOfVecPredWhilePackedBits = kCondKind + kCondKindSize;
  static_assert(kNumberOfVecPredWhilePackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using CondKindField = BitField<CondKind, kCondKind, kCondKindSize>;

  DEFAULT_COPY_CONSTRUCTOR(VecPredWhile);
};

// Evaluates the predicate condition (PCondKind) for a vector predicate; outputs
// a scalar boolean value result.
//
// Note: as VecPredToBoolean can be also predicated, only active elements (determined by the
// instruction's governing predicate) of the input vector predicate are used for condition
// evaluation.
//
// Note: this instruction is currently used as a workaround for the fact that IR instructions
// can't have more than one output.
class HVecPredToBoolean final : public HVecOperation {
 public:
  // To get more info on the condition kinds please see "2.2 Process state, PSTATE" section of
  // "ARM Architecture Reference Manual Supplement. The Scalable Vector Extension (SVE),
  // for ARMv8-A".
  enum class PCondKind {
    kNone,    // No active elements were TRUE.
    kAny,     // An active element was TRUE.
    kNLast,   // The last active element was not TRUE.
    kLast,    // The last active element was TRUE.
    kFirst,   // The first active element was TRUE.
    kNFirst,  // The first active element was not TRUE.
    kPMore,   // An active element was TRUE but not the last active element.
    kPLast,   // The last active element was TRUE or no active elements were TRUE.
    kEnumLast = kPLast
  };

  HVecPredToBoolean(ArenaAllocator* allocator,
                    HInstruction* input,
                    PCondKind pred_cond,
                    DataType::Type packed_type,
                    size_t vector_length,
                    uint32_t dex_pc)
      : HVecOperation(kVecPredToBoolean,
                      allocator,
                      packed_type,
                      SideEffects::None(),
                      /* number_of_inputs */ 1,
                      vector_length,
                      dex_pc) {
    DCHECK(input->IsVecPredSetOperation());
    SetRawInputAt(0, input);
    // Overrides the kSIMDType set by the VecOperation constructor.
    SetPackedField<TypeField>(DataType::Type::kBool);
    SetPackedField<CondKindField>(pred_cond);
  }

  // This instruction is currently used only as a special loop control instruction
  // which must not be predicated.
  // TODO: Remove the constraint.
  bool MustBePredicatedInPredicatedSIMDMode() override { return false; }

  PCondKind GetPCondKind() const {
    return GetPackedField<CondKindField>();
  }

  DECLARE_INSTRUCTION(VecPredToBoolean);

 protected:
  // Additional packed bits.
  static constexpr size_t kCondKind = HVecOperation::kNumberOfVectorOpPackedBits;
  static constexpr size_t kCondKindSize =
      MinimumBitsToStore(static_cast<size_t>(PCondKind::kEnumLast));
  static constexpr size_t kNumberOfVecPredToBooleanPackedBits = kCondKind + kCondKindSize;
  static_assert(kNumberOfVecPredToBooleanPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using CondKindField = BitField<PCondKind, kCondKind, kCondKindSize>;

  DEFAULT_COPY_CONSTRUCTOR(VecPredToBoolean);
};

// Evaluates condition for pairwise elements in two input vectors and sets the result
// as an output predicate vector.
//
// viz. [ p1, .. , pn ]  = [ x1 OP y1 , x2 OP y2, .. , xn OP yn] where OP is CondKind
// condition.
class HVecCondition : public HVecPredSetOperation {
 public:
  HVecCondition(InstructionKind kind,
                ArenaAllocator* allocator,
                HInstruction* left,
                HInstruction* right,
                DataType::Type packed_type,
                size_t vector_length,
                uint32_t dex_pc = kNoDexPc) :
      HVecPredSetOperation(kind,
                           allocator,
                           packed_type,
                           SideEffects::None(),
                           /* number_of_inputs= */ 2,
                           vector_length,
                           dex_pc) {
    DCHECK(left->IsVecOperation());
    DCHECK(!left->IsVecPredSetOperation());
    DCHECK(right->IsVecOperation());
    DCHECK(!right->IsVecPredSetOperation());
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  DECLARE_ABSTRACT_INSTRUCTION(VecCondition);

  virtual IfCondition GetCondition() const = 0;

  static HVecCondition* Create(HGraph* graph,
                               IfCondition cond,
                               HInstruction* lhs,
                               HInstruction* rhs,
                               DataType::Type packed_type,
                               size_t vector_length,
                               uint32_t dex_pc = kNoDexPc);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecCondition);
};

// Instruction to check if two vector inputs are equal to each other.
class HVecEqual final : public HVecCondition {
 public:
  HVecEqual(ArenaAllocator* allocator,
            HInstruction* left,
            HInstruction* right,
            DataType::Type packed_type,
            size_t vector_length,
            uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecEqual, allocator, left, right, packed_type, vector_length, dex_pc) {}

  DECLARE_INSTRUCTION(VecEqual);

  IfCondition GetCondition() const override {
    return kCondEQ;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecEqual);
};

// Instruction to check if two vector inputs are not equal to each other.
class HVecNotEqual final : public HVecCondition {
 public:
  HVecNotEqual(ArenaAllocator* allocator,
               HInstruction* left,
               HInstruction* right,
               DataType::Type packed_type,
               size_t vector_length,
               uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecNotEqual, allocator, left, right, packed_type, vector_length, dex_pc) {}

  DECLARE_INSTRUCTION(VecNotEqual);

  IfCondition GetCondition() const override {
    return kCondNE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecNotEqual);
};

// Instruction to check if one vector input is less than the other.
class HVecLessThan final : public HVecCondition {
 public:
  HVecLessThan(ArenaAllocator* allocator,
               HInstruction* left,
               HInstruction* right,
               DataType::Type packed_type,
               size_t vector_length,
               uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecLessThan, allocator, left, right, packed_type, vector_length, dex_pc) {}

  DECLARE_INSTRUCTION(VecLessThan);

  IfCondition GetCondition() const override {
    return kCondLT;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecLessThan);
};

// Instruction to check if one vector input is less than or equal to the other.
class HVecLessThanOrEqual final : public HVecCondition {
 public:
  HVecLessThanOrEqual(ArenaAllocator* allocator,
                      HInstruction* left,
                      HInstruction* right,
                      DataType::Type packed_type,
                      size_t vector_length,
                      uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecLessThanOrEqual,
                      allocator,
                      left,
                      right,
                      packed_type,
                      vector_length,
                      dex_pc) {}

  DECLARE_INSTRUCTION(VecLessThanOrEqual);

  IfCondition GetCondition() const override {
    return kCondLE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecLessThanOrEqual);
};

// Instruction to check if one vector input is greater than the other.
class HVecGreaterThan final : public HVecCondition {
 public:
  HVecGreaterThan(ArenaAllocator* allocator,
                  HInstruction* left,
                  HInstruction* right,
                  DataType::Type packed_type,
                  size_t vector_length,
                  uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecGreaterThan,
                      allocator,
                      left,
                      right,
                      packed_type,
                      vector_length,
                      dex_pc) {}

  DECLARE_INSTRUCTION(VecGreaterThan);

  IfCondition GetCondition() const override {
    return kCondGT;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecGreaterThan);
};

// Instruction to check if one vector input is greater than or equal to the other.
class HVecGreaterThanOrEqual final : public HVecCondition {
 public:
  HVecGreaterThanOrEqual(ArenaAllocator* allocator,
                         HInstruction* left,
                         HInstruction* right,
                         DataType::Type packed_type,
                         size_t vector_length,
                         uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecGreaterThanOrEqual,
                      allocator,
                      left,
                      right,
                      packed_type,
                      vector_length,
                      dex_pc) {}

  DECLARE_INSTRUCTION(VecGreaterThanOrEqual);

  IfCondition GetCondition() const override {
    return kCondGE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecGreaterThanOrEqual);
};

// Instruction to check if one unsigned vector input is less than the other, using unsigned
// comparison.
class HVecBelow final : public HVecCondition {
 public:
  HVecBelow(ArenaAllocator* allocator,
            HInstruction* left,
            HInstruction* right,
            DataType::Type packed_type,
            size_t vector_length,
            uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecBelow, allocator, left, right, packed_type, vector_length, dex_pc) {}

  DECLARE_INSTRUCTION(VecBelow);

  IfCondition GetCondition() const override {
    return kCondB;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecBelow);
};

// Instruction to check if one unsigned vector input is less than or equal to the other, using
// unsigned comparison.
class HVecBelowOrEqual final : public HVecCondition {
 public:
  HVecBelowOrEqual(ArenaAllocator* allocator,
                   HInstruction* left,
                   HInstruction* right,
                   DataType::Type packed_type,
                   size_t vector_length,
                   uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecBelowOrEqual,
                      allocator,
                      left,
                      right,
                      packed_type,
                      vector_length,
                      dex_pc) {}

  DECLARE_INSTRUCTION(VecBelowOrEqual);

  IfCondition GetCondition() const override {
    return kCondBE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecBelowOrEqual);
};

// Instruction to check if one unsigned vector input is greater than the other, using unsigned
// comparison.
class HVecAbove final : public HVecCondition {
 public:
  HVecAbove(ArenaAllocator* allocator,
            HInstruction* left,
            HInstruction* right,
            DataType::Type packed_type,
            size_t vector_length,
            uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecAbove, allocator, left, right, packed_type, vector_length, dex_pc) {}

  DECLARE_INSTRUCTION(VecAbove);

  IfCondition GetCondition() const override {
    return kCondA;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecAbove);
};

// Instruction to check if one unsigned vector input is greater than or equal to the other, using
// unsigned comparison.
class HVecAboveOrEqual final : public HVecCondition {
 public:
  HVecAboveOrEqual(ArenaAllocator* allocator,
                   HInstruction* left,
                   HInstruction* right,
                   DataType::Type packed_type,
                   size_t vector_length,
                   uint32_t dex_pc = kNoDexPc)
      : HVecCondition(kVecAboveOrEqual,
                      allocator,
                      left,
                      right,
                      packed_type,
                      vector_length,
                      dex_pc) {}

  DECLARE_INSTRUCTION(VecAboveOrEqual);

  IfCondition GetCondition() const override {
    return kCondAE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecAboveOrEqual);
};

// Inverts every component in the predicate vector.
//
// viz. [ p1, .. , pn ]  = [ !px1 , !px2 , .. , !pxn ].
class HVecPredNot final : public HVecPredSetOperation {
 public:
  HVecPredNot(ArenaAllocator* allocator,
              HInstruction* input,
              DataType::Type packed_type,
              size_t vector_length,
              uint32_t dex_pc) :
      HVecPredSetOperation(kVecPredNot,
                           allocator,
                           packed_type,
                           SideEffects::None(),
                           /* number_of_inputs= */ 1,
                           vector_length,
                           dex_pc) {
    DCHECK(input->IsVecOperation());
    DCHECK(input->IsVecPredSetOperation());

    SetRawInputAt(0, input);
  }

  DECLARE_INSTRUCTION(VecPredNot);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(VecPredNot);
};

// Return the number of elements of the given type that will fit into a vector of given size.
inline size_t GetNumberOfElementsInVector(size_t vector_size_in_bytes, DataType::Type type) {
  return vector_size_in_bytes / DataType::Size(type);
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_VECTOR_H_
