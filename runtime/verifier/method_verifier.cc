/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "method_verifier-inl.h"

#include <ostream>

#include "android-base/stringprintf.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/aborting.h"
#include "base/leb128.h"
#include "base/indenter.h"
#include "base/logging.h"  // For VLOG.
#include "base/mutex-inl.h"
#include "base/pointer_size.h"
#include "base/sdk_version.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "dex/class_accessor-inl.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/dex_instruction-inl.h"
#include "dex/dex_instruction_list.h"
#include "dex/dex_instruction_utils.h"
#include "experimental_flags.h"
#include "gc/accounting/card_table-inl.h"
#include "handle_scope-inl.h"
#include "intern_table.h"
#include "mirror/class-inl.h"
#include "mirror/class.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/method_handle_impl.h"
#include "mirror/method_type.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/var_handle.h"
#include "obj_ptr-inl.h"
#include "reg_type-inl.h"
#include "reg_type_cache.h"
#include "register_line-inl.h"
#include "runtime.h"
#include "scoped_newline.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "vdex_file.h"
#include "verifier/method_verifier.h"
#include "verifier_deps.h"

namespace art HIDDEN {
namespace verifier {

using android::base::StringPrintf;

static constexpr bool kTimeVerifyMethod = !kIsDebugBuild;

PcToRegisterLineTable::PcToRegisterLineTable(ArenaAllocator& allocator)
    : register_lines_(allocator.Adapter(kArenaAllocVerifier)) {}

void PcToRegisterLineTable::Init(InstructionFlags* flags,
                                 uint32_t insns_size,
                                 uint16_t registers_size,
                                 ArenaAllocator& allocator,
                                 RegTypeCache* reg_types,
                                 uint32_t interesting_dex_pc) {
  DCHECK_GT(insns_size, 0U);
  register_lines_.resize(insns_size);
  for (uint32_t i = 0; i < insns_size; i++) {
    if ((i == interesting_dex_pc) || flags[i].IsBranchTarget()) {
      register_lines_[i].reset(RegisterLine::Create(registers_size, allocator, reg_types));
    }
  }
}

PcToRegisterLineTable::~PcToRegisterLineTable() {}

namespace impl {
namespace {

enum class CheckAccess {
  kNo,
  kOnResolvedClass,
  kYes,
};

enum class FieldAccessType {
  kAccGet,
  kAccPut
};

// Instruction types that are not marked as throwing (because they normally would not), but for
// historical reasons may do so. These instructions cannot be marked kThrow as that would introduce
// a general flow that is unwanted.
//
// Note: Not implemented as Instruction::Flags value as that set is full and we'd need to increase
//       the struct size (making it a non-power-of-two) for a single element.
//
// Note: This should eventually be removed.
constexpr bool IsCompatThrow(Instruction::Code opcode) {
  return opcode == Instruction::Code::RETURN_OBJECT || opcode == Instruction::Code::MOVE_EXCEPTION;
}

template <bool kVerifierDebug>
class MethodVerifier final : public ::art::verifier::MethodVerifier {
 public:
  bool IsInstanceConstructor() const {
    return IsConstructor() && !IsStatic();
  }

  void FindLocksAtDexPc() REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  MethodVerifier(Thread* self,
                 ArenaPool* arena_pool,
                 RegTypeCache* reg_types,
                 VerifierDeps* verifier_deps,
                 const dex::CodeItem* code_item,
                 uint32_t method_idx,
                 bool aot_mode,
                 Handle<mirror::DexCache> dex_cache,
                 const dex::ClassDef& class_def,
                 uint32_t access_flags,
                 bool verify_to_dump,
                 uint32_t api_level) REQUIRES_SHARED(Locks::mutator_lock_)
     : art::verifier::MethodVerifier(self,
                                     arena_pool,
                                     reg_types,
                                     verifier_deps,
                                     class_def,
                                     code_item,
                                     method_idx,
                                     aot_mode),
       method_access_flags_(access_flags),
       return_type_(nullptr),
       dex_cache_(dex_cache),
       class_loader_(reg_types->GetClassLoader()),
       declaring_class_(nullptr),
       interesting_dex_pc_(-1),
       monitor_enter_dex_pcs_(nullptr),
       verify_to_dump_(verify_to_dump),
       allow_thread_suspension_(reg_types->CanSuspend()),
       is_constructor_(false),
       api_level_(api_level == 0 ? std::numeric_limits<uint32_t>::max() : api_level) {
    DCHECK_EQ(dex_cache->GetDexFile(), reg_types->GetDexFile())
        << dex_cache->GetDexFile()->GetLocation() << " / "
        << reg_types->GetDexFile()->GetLocation();
  }

  void FinalAbstractClassError(ObjPtr<mirror::Class> klass) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Note: We reuse NO_CLASS as the instruction we're checking shall throw an exception at
    // runtime if executed. A final abstract class shall fail verification, so no instances can
    // be created and therefore instance field or method access can be reached only for a null
    // reference and throw NPE. All other instructions where we check for final abstract class
    // shall throw `VerifyError`. (But we can also hit OOME/SOE while creating the exception.)
    std::string temp;
    const char* descriptor = klass->GetDescriptor(&temp);
    Fail(VerifyError::VERIFY_ERROR_NO_CLASS)
        << "Final abstract class used in a context that requires a verified class: " << descriptor;
  }

  void CheckForFinalAbstractClass(ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (UNLIKELY(klass->IsFinal() &&
                 klass->IsAbstract() &&
                 !klass->IsInterface() &&
                 !klass->IsPrimitive() &&
                 !klass->IsArrayClass())) {
      FinalAbstractClassError(klass);
    }
  }

  // Is the method being verified a constructor? See the comment on the field.
  bool IsConstructor() const {
    return is_constructor_;
  }

  // Is the method verified static?
  bool IsStatic() const {
    return (method_access_flags_ & kAccStatic) != 0;
  }

  // Adds the given string to the beginning of the last failure message.
  void PrependToLastFailMessage(std::string prepend) {
    size_t failure_num = failure_messages_.size();
    DCHECK_NE(failure_num, 0U);
    std::ostringstream* last_fail_message = failure_messages_[failure_num - 1];
    prepend += last_fail_message->str();
    failure_messages_[failure_num - 1] = new std::ostringstream(prepend, std::ostringstream::ate);
    delete last_fail_message;
  }

  // Adds the given string to the end of the last failure message.
  void AppendToLastFailMessage(const std::string& append) {
    size_t failure_num = failure_messages_.size();
    DCHECK_NE(failure_num, 0U);
    std::ostringstream* last_fail_message = failure_messages_[failure_num - 1];
    (*last_fail_message) << append;
  }

  /*
   * Compute the width of the instruction at each address in the instruction stream, and store it in
   * insn_flags_. Addresses that are in the middle of an instruction, or that are part of switch
   * table data, are not touched (so the caller should probably initialize "insn_flags" to zero).
   *
   * The "new_instance_count_" and "monitor_enter_count_" fields in vdata are also set.
   *
   * Performs some static checks, notably:
   * - opcode of first instruction begins at index 0
   * - only documented instructions may appear
   * - each instruction follows the last
   * - last byte of last instruction is at (code_length-1)
   *
   * Logs an error and returns "false" on failure.
   */
  bool ComputeWidthsAndCountOps();

  /*
   * Set the "in try" flags for all instructions protected by "try" statements. Also sets the
   * "branch target" flags for exception handlers.
   *
   * Call this after widths have been set in "insn_flags".
   *
   * Returns "false" if something in the exception table looks fishy, but we're expecting the
   * exception table to be valid.
   */
  bool ScanTryCatchBlocks() REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Perform static verification on all instructions in a method.
   *
   * Walks through instructions in a method calling VerifyInstruction on each.
   */
  bool VerifyInstructions();

  /*
   * Perform static verification on an instruction.
   *
   * As a side effect, this sets the "branch target" flags in InsnFlags.
   *
   * "(CF)" items are handled during code-flow analysis.
   *
   * v3 4.10.1
   * - target of each jump and branch instruction must be valid
   * - targets of switch statements must be valid
   * - operands referencing constant pool entries must be valid
   * - (CF) operands of getfield, putfield, getstatic, putstatic must be valid
   * - (CF) operands of method invocation instructions must be valid
   * - (CF) only invoke-direct can call a method starting with '<'
   * - (CF) <clinit> must never be called explicitly
   * - operands of instanceof, checkcast, new (and variants) must be valid
   * - new-array[-type] limited to 255 dimensions
   * - can't use "new" on an array class
   * - (?) limit dimensions in multi-array creation
   * - local variable load/store register values must be in valid range
   *
   * v3 4.11.1.2
   * - branches must be within the bounds of the code array
   * - targets of all control-flow instructions are the start of an instruction
   * - register accesses fall within range of allocated registers
   * - (N/A) access to constant pool must be of appropriate type
   * - code does not end in the middle of an instruction
   * - execution cannot fall off the end of the code
   * - (earlier) for each exception handler, the "try" area must begin and
   *   end at the start of an instruction (end can be at the end of the code)
   * - (earlier) for each exception handler, the handler must start at a valid
   *   instruction
   */
  template <Instruction::Code kDispatchOpcode>
  ALWAYS_INLINE bool VerifyInstruction(const Instruction* inst,
                                       uint32_t code_offset,
                                       uint16_t inst_data);

  /* Ensure that the register index is valid for this code item. */
  bool CheckRegisterIndex(uint32_t idx) {
    if (UNLIKELY(idx >= code_item_accessor_.RegistersSize())) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "register index out of range (" << idx << " >= "
                                        << code_item_accessor_.RegistersSize() << ")";
      return false;
    }
    return true;
  }

  /* Ensure that the wide register index is valid for this code item. */
  bool CheckWideRegisterIndex(uint32_t idx) {
    if (UNLIKELY(idx + 1 >= code_item_accessor_.RegistersSize())) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "wide register index out of range (" << idx
                                        << "+1 >= " << code_item_accessor_.RegistersSize() << ")";
      return false;
    }
    return true;
  }

  // Perform static checks on an instruction referencing a CallSite. All we do here is ensure that
  // the call site index is in the valid range.
  bool CheckCallSiteIndex(uint32_t idx) {
    uint32_t limit = dex_file_->NumCallSiteIds();
    if (UNLIKELY(idx >= limit)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad call site index " << idx << " (max "
                                        << limit << ")";
      return false;
    }
    return true;
  }

  // Perform static checks on a field Get or set instruction. We ensure that the field index
  // is in the valid range and we check that the field descriptor matches the instruction.
  ALWAYS_INLINE bool CheckFieldIndex(const Instruction* inst,
                                     uint16_t inst_data,
                                     uint32_t field_idx) {
    if (UNLIKELY(field_idx >= dex_file_->NumFieldIds())) {
      FailBadFieldIndex(field_idx);
      return false;
    }

    // Prepare a table with permitted descriptors, evaluated at compile time.
    static constexpr uint32_t kVerifyFieldIndexFlags =
        Instruction::kVerifyRegBField | Instruction::kVerifyRegCField;
    static constexpr uint32_t kMinFieldAccessOpcode = []() constexpr {
      for (uint32_t opcode = 0u; opcode != 256u; ++opcode) {
        uint32_t verify_flags = Instruction::VerifyFlagsOf(enum_cast<Instruction::Code>(opcode));
        if ((verify_flags & kVerifyFieldIndexFlags) != 0u) {
          return opcode;
        }
      }
      LOG(FATAL) << "Compile time error if we reach this.";
      return 0u;
    }();
    static constexpr uint32_t kMaxFieldAccessOpcode = []() constexpr {
      for (uint32_t opcode = 256u; opcode != 0u; ) {
        --opcode;
        uint32_t verify_flags = Instruction::VerifyFlagsOf(enum_cast<Instruction::Code>(opcode));
        if ((verify_flags & kVerifyFieldIndexFlags) != 0u) {
          return opcode;
        }
      }
      LOG(FATAL) << "Compile time error if we reach this.";
      return 0u;
    }();
    static constexpr uint32_t kArraySize = kMaxFieldAccessOpcode + 1u - kMinFieldAccessOpcode;
    using PermittedDescriptorArray = std::array<std::pair<char, char>, kArraySize>;
    static constexpr PermittedDescriptorArray kPermittedDescriptors = []() constexpr {
      PermittedDescriptorArray result;
      for (uint32_t index = 0u; index != kArraySize; ++index) {
        Instruction::Code opcode = enum_cast<Instruction::Code>(index + kMinFieldAccessOpcode);
        DexMemAccessType access_type;
        if (IsInstructionIGet(opcode) || IsInstructionIPut(opcode)) {
          access_type = IGetOrIPutMemAccessType(opcode);
        } else {
          // `iget*`, `iput*`, `sget*` and `sput*` instructions form a contiguous range.
          CHECK(IsInstructionSGet(opcode) || IsInstructionSPut(opcode));
          access_type = SGetOrSPutMemAccessType(opcode);
        }
        switch (access_type) {
          case DexMemAccessType::kDexMemAccessWord:
            result[index] = { 'I', 'F' };
            break;
          case DexMemAccessType::kDexMemAccessWide:
            result[index] = { 'J', 'D' };
            break;
          case DexMemAccessType::kDexMemAccessObject:
            result[index] = { 'L', '[' };
            break;
          case DexMemAccessType::kDexMemAccessBoolean:
            result[index] = { 'Z', 'Z' };  // Only one character is permitted.
            break;
          case DexMemAccessType::kDexMemAccessByte:
            result[index] = { 'B', 'B' };  // Only one character is permitted.
            break;
          case DexMemAccessType::kDexMemAccessChar:
            result[index] = { 'C', 'C' };  // Only one character is permitted.
            break;
          case DexMemAccessType::kDexMemAccessShort:
            result[index] = { 'S', 'S' };  // Only one character is permitted.
            break;
          default:
            LOG(FATAL) << "Compile time error if we reach this.";
            break;
        }
      }
      return result;
    }();

    // Check the first character of the field type descriptor.
    Instruction::Code opcode = inst->Opcode(inst_data);
    DCHECK_GE(opcode, kMinFieldAccessOpcode);
    DCHECK_LE(opcode, kMaxFieldAccessOpcode);
    std::pair<char, char> permitted = kPermittedDescriptors[opcode - kMinFieldAccessOpcode];
    const char* descriptor = dex_file_->GetFieldTypeDescriptor(field_idx);
    if (UNLIKELY(descriptor[0] != permitted.first && descriptor[0] != permitted.second)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD)
          << "expected field " << dex_file_->PrettyField(field_idx)
          << " to have type descritor starting with '" << permitted.first
          << (permitted.second != permitted.first ? std::string("' or '") + permitted.second : "")
          << "' but found '" << descriptor[0] << "' in " << opcode;
      return false;
    }
    return true;
  }

  // Perform static checks on a method invocation instruction. All we do here is ensure that the
  // method index is in the valid range.
  ALWAYS_INLINE bool CheckMethodIndex(uint32_t method_idx) {
    if (UNLIKELY(method_idx >= dex_file_->NumMethodIds())) {
      FailBadMethodIndex(method_idx);
      return false;
    }
    return true;
  }

  // Perform static checks on an instruction referencing a constant method handle. All we do here
  // is ensure that the method index is in the valid range.
  bool CheckMethodHandleIndex(uint32_t idx) {
    uint32_t limit = dex_file_->NumMethodHandles();
    if (UNLIKELY(idx >= limit)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad method handle index " << idx << " (max "
                                        << limit << ")";
      return false;
    }
    return true;
  }

  // Perform static checks on a "new-instance" instruction. Specifically, make sure the class
  // reference isn't for an array class.
  bool CheckNewInstance(dex::TypeIndex idx);

  // Perform static checks on a prototype indexing instruction. All we do here is ensure that the
  // prototype index is in the valid range.
  bool CheckPrototypeIndex(uint32_t idx) {
    if (UNLIKELY(idx >= dex_file_->GetHeader().proto_ids_size_)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad prototype index " << idx << " (max "
                                        << dex_file_->GetHeader().proto_ids_size_ << ")";
      return false;
    }
    return true;
  }

  /* Ensure that the string index is in the valid range. */
  bool CheckStringIndex(uint32_t idx) {
    if (UNLIKELY(idx >= dex_file_->GetHeader().string_ids_size_)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad string index " << idx << " (max "
                                        << dex_file_->GetHeader().string_ids_size_ << ")";
      return false;
    }
    return true;
  }

  // Perform static checks on an instruction that takes a class constant. Ensure that the class
  // index is in the valid range.
  bool CheckTypeIndex(dex::TypeIndex idx) {
    if (UNLIKELY(idx.index_ >= dex_file_->GetHeader().type_ids_size_)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad type index " << idx.index_ << " (max "
                                        << dex_file_->GetHeader().type_ids_size_ << ")";
      return false;
    }
    return true;
  }

  // Perform static checks on a "new-array" instruction. Specifically, make sure they aren't
  // creating an array of arrays that causes the number of dimensions to exceed 255.
  bool CheckNewArray(dex::TypeIndex idx);

  // Verify an array data table. "cur_offset" is the offset of the fill-array-data instruction.
  bool CheckArrayData(uint32_t cur_offset);

  // Verify that the target of a branch instruction is valid. We don't expect code to jump directly
  // into an exception handler, but it's valid to do so as long as the target isn't a
  // "move-exception" instruction. We verify that in a later stage.
  // The dex format forbids certain instructions from branching to themselves.
  // Updates "insn_flags_", setting the "branch target" flag.
  bool CheckBranchTarget(uint32_t cur_offset);

  // Verify a switch table. "cur_offset" is the offset of the switch instruction.
  // Updates "insn_flags_", setting the "branch target" flag.
  bool CheckSwitchTargets(uint32_t cur_offset);

  // Check the register indices used in a "vararg" instruction, such as invoke-virtual or
  // filled-new-array.
  // - inst is the instruction from which we retrieve the arguments
  // - vA holds the argument count (0-5)
  // There are some tests we don't do here, e.g. we don't try to verify that invoking a method that
  // takes a double is done with consecutive registers. This requires parsing the target method
  // signature, which we will be doing later on during the code flow analysis.
  bool CheckVarArgRegs(const Instruction* inst, uint32_t vA) {
    uint16_t registers_size = code_item_accessor_.RegistersSize();
    // All args are 4-bit and therefore under 16. We do not need to check args for
    // `registers_size >= 16u` but let's check them anyway in debug builds.
    if (registers_size < 16u || kIsDebugBuild) {
      uint32_t args[Instruction::kMaxVarArgRegs];
      inst->GetVarArgs(args);
      for (uint32_t idx = 0; idx < vA; idx++) {
        DCHECK_LT(args[idx], 16u);
        if (UNLIKELY(args[idx] >= registers_size)) {
          DCHECK_LT(registers_size, 16u);
          Fail(VERIFY_ERROR_BAD_CLASS_HARD)
              << "invalid reg index (" << args[idx]
              << ") in non-range invoke (>= " << registers_size << ")";
          return false;
        }
      }
    }
    return true;
  }

  // Check the register indices used in a "vararg/range" instruction, such as invoke-virtual/range
  // or filled-new-array/range.
  // - vA holds word count, vC holds index of first reg.
  bool CheckVarArgRangeRegs(uint32_t vA, uint32_t vC) {
    uint16_t registers_size = code_item_accessor_.RegistersSize();
    // vA/vC are unsigned 8-bit/16-bit quantities for /range instructions, so there's no risk of
    // integer overflow when adding them here.
    if (UNLIKELY(vA + vC > registers_size)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid reg index " << vA << "+" << vC
                                        << " in range invoke (> " << registers_size << ")";
      return false;
    }
    return true;
  }

  // Checks the method matches the expectations required to be signature polymorphic.
  bool CheckSignaturePolymorphicMethod(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  // Checks the invoked receiver matches the expectations for signature polymorphic methods.
  bool CheckSignaturePolymorphicReceiver(const Instruction* inst) REQUIRES_SHARED(Locks::mutator_lock_);

  // Extract the relative offset from a branch instruction.
  // Returns "false" on failure (e.g. this isn't a branch instruction).
  bool GetBranchOffset(uint32_t cur_offset, int32_t* pOffset, bool* pConditional,
                       bool* selfOkay);

  /* Perform detailed code-flow analysis on a single method. */
  bool VerifyCodeFlow() REQUIRES_SHARED(Locks::mutator_lock_);

  // Set the register types for the first instruction in the method based on the method signature.
  // This has the side-effect of validating the signature.
  bool SetTypesFromSignature() REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Perform code flow on a method.
   *
   * The basic strategy is as outlined in v3 4.11.1.2: set the "changed" bit on the first
   * instruction, process it (setting additional "changed" bits), and repeat until there are no
   * more.
   *
   * v3 4.11.1.1
   * - (N/A) operand stack is always the same size
   * - operand stack [registers] contain the correct types of values
   * - local variables [registers] contain the correct types of values
   * - methods are invoked with the appropriate arguments
   * - fields are assigned using values of appropriate types
   * - opcodes have the correct type values in operand registers
   * - there is never an uninitialized class instance in a local variable in code protected by an
   *   exception handler (operand stack is okay, because the operand stack is discarded when an
   *   exception is thrown) [can't know what's a local var w/o the debug info -- should fall out of
   *   register typing]
   *
   * v3 4.11.1.2
   * - execution cannot fall off the end of the code
   *
   * (We also do many of the items described in the "static checks" sections, because it's easier to
   * do them here.)
   *
   * We need an array of RegType values, one per register, for every instruction. If the method uses
   * monitor-enter, we need extra data for every register, and a stack for every "interesting"
   * instruction. In theory this could become quite large -- up to several megabytes for a monster
   * function.
   *
   * NOTE:
   * The spec forbids backward branches when there's an uninitialized reference in a register. The
   * idea is to prevent something like this:
   *   loop:
   *     move r1, r0
   *     new-instance r0, MyClass
   *     ...
   *     if-eq rN, loop  // once
   *   initialize r0
   *
   * This leaves us with two different instances, both allocated by the same instruction, but only
   * one is initialized. The scheme outlined in v3 4.11.1.4 wouldn't catch this, so they work around
   * it by preventing backward branches. We achieve identical results without restricting code
   * reordering by specifying that you can't execute the new-instance instruction if a register
   * contains an uninitialized instance created by that same instruction.
   */
  template <bool kMonitorDexPCs>
  bool CodeFlowVerifyMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Perform verification for a single instruction.
   *
   * This requires fully decoding the instruction to determine the effect it has on registers.
   *
   * Finds zero or more following instructions and sets the "changed" flag if execution at that
   * point needs to be (re-)evaluated. Register changes are merged into "reg_types_" at the target
   * addresses. Does not set or clear any other flags in "insn_flags_".
   */
  bool CodeFlowVerifyInstruction(uint32_t* start_guess)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Perform verification of a new array instruction
  void VerifyNewArray(const Instruction* inst, bool is_filled, bool is_range)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Helper to perform verification on puts of primitive type.
  void VerifyPrimitivePut(const RegType& target_type, uint32_t vregA)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Perform verification of an aget instruction. The destination register's type will be set to
  // be that of component type of the array unless the array type is unknown, in which case a
  // bottom type inferred from the type of instruction is used. is_primitive is false for an
  // aget-object.
  void VerifyAGet(const Instruction* inst, const RegType& insn_type,
                  bool is_primitive) REQUIRES_SHARED(Locks::mutator_lock_);

  // Perform verification of an aput instruction.
  void VerifyAPut(const Instruction* inst, const RegType& insn_type,
                  bool is_primitive) REQUIRES_SHARED(Locks::mutator_lock_);

  // Lookup instance field and fail for resolution violations
  ArtField* GetInstanceField(uint32_t vregB, uint32_t field_idx, bool is_put)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Lookup static field and fail for resolution violations
  ArtField* GetStaticField(uint32_t field_idx, bool is_put) REQUIRES_SHARED(Locks::mutator_lock_);

  // Common checks for `GetInstanceField()` and `GetStaticField()`.
  ArtField* GetISFieldCommon(ArtField* field, bool is_put) REQUIRES_SHARED(Locks::mutator_lock_);

  // Perform verification of an iget/sget/iput/sput instruction.
  template <FieldAccessType kAccType>
  void VerifyISFieldAccess(const Instruction* inst, bool is_primitive, bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolves a class based on an index and, if C is kYes, performs access checks to ensure
  // the referrer can access the resolved class.
  template <CheckAccess C>
  const RegType& ResolveClass(dex::TypeIndex class_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * For the "move-exception" instruction at "work_insn_idx_", which must be at an exception handler
   * address, determine the Join of all exceptions that can land here. Fails if no matching
   * exception handler can be found or if the Join of exception types fails.
   */
  const RegType& GetCaughtExceptionType()
      REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Resolves a method based on an index and performs access checks to ensure
   * the referrer can access the resolved method.
   * Does not throw exceptions.
   */
  ArtMethod* ResolveMethodAndCheckAccess(uint32_t method_idx, MethodType method_type)
      REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Verify the arguments to a method. We're executing in "method", making
   * a call to the method reference in vB.
   *
   * If this is a "direct" invoke, we allow calls to <init>. For calls to
   * <init>, the first argument may be an uninitialized reference. Otherwise,
   * calls to anything starting with '<' will be rejected, as will any
   * uninitialized reference arguments.
   *
   * For non-static method calls, this will verify that the method call is
   * appropriate for the "this" argument.
   *
   * The method reference is in vBBBB. The "is_range" parameter determines
   * whether we use 0-4 "args" values or a range of registers defined by
   * vAA and vCCCC.
   *
   * Widening conversions on integers and references are allowed, but
   * narrowing conversions are not.
   *
   * Returns the resolved method on success, null on failure (with *failure
   * set appropriately).
   */
  ArtMethod* VerifyInvocationArgs(const Instruction* inst, MethodType method_type, bool is_range)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Similar checks to the above, but on the proto. Will be used when the method cannot be
  // resolved.
  void VerifyInvocationArgsUnresolvedMethod(const Instruction* inst, MethodType method_type,
                                            bool is_range)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <class T>
  ArtMethod* VerifyInvocationArgsFromIterator(T* it, const Instruction* inst,
                                                      MethodType method_type, bool is_range,
                                                      ArtMethod* res_method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Verify the arguments present for a call site. Returns "true" if all is well, "false" otherwise.
   */
  bool CheckCallSite(uint32_t call_site_idx);

  /*
   * Verify that the target instruction is not "move-exception". It's important that the only way
   * to execute a move-exception is as the first instruction of an exception handler.
   * Returns "true" if all is well, "false" if the target instruction is move-exception.
   */
  bool CheckNotMoveException(const uint16_t* insns, int insn_idx) {
    if ((insns[insn_idx] & 0xff) == Instruction::MOVE_EXCEPTION) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid use of move-exception";
      return false;
    }
    return true;
  }

  /*
   * Verify that the target instruction is not "move-result". It is important that we cannot
   * branch to move-result instructions, but we have to make this a distinct check instead of
   * adding it to CheckNotMoveException, because it is legal to continue into "move-result"
   * instructions - as long as the previous instruction was an invoke, which is checked elsewhere.
   */
  bool CheckNotMoveResult(const uint16_t* insns, int insn_idx) {
    if (((insns[insn_idx] & 0xff) >= Instruction::MOVE_RESULT) &&
        ((insns[insn_idx] & 0xff) <= Instruction::MOVE_RESULT_OBJECT)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid use of move-result*";
      return false;
    }
    return true;
  }

  /*
   * Verify that the target instruction is not "move-result" or "move-exception". This is to
   * be used when checking branch and switch instructions, but not instructions that can
   * continue.
   */
  bool CheckNotMoveExceptionOrMoveResult(const uint16_t* insns, int insn_idx) {
    return (CheckNotMoveException(insns, insn_idx) && CheckNotMoveResult(insns, insn_idx));
  }

  /*
  * Control can transfer to "next_insn". Merge the registers from merge_line into the table at
  * next_insn, and set the changed flag on the target address if any of the registers were changed.
  * In the case of fall-through, update the merge line on a change as its the working line for the
  * next instruction.
  * Returns "false" if an error is encountered.
  */
  bool UpdateRegisters(uint32_t next_insn, RegisterLine* merge_line, bool update_merge_line)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Return the register type for the method.
  const RegType& GetMethodReturnType() REQUIRES_SHARED(Locks::mutator_lock_);

  // Get a type representing the declaring class of the method.
  const RegType& GetDeclaringClass() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (declaring_class_ == nullptr) {
      const dex::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx_);
      declaring_class_ = &reg_types_.FromTypeIndex(method_id.class_idx_);
    }
    return *declaring_class_;
  }

  ObjPtr<mirror::Class> GetRegTypeClass(const RegType& reg_type)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(reg_type.IsJavaLangObject() || reg_type.IsReference()) << reg_type;
    return reg_type.IsJavaLangObject() ? GetClassRoot<mirror::Object>(GetClassLinker())
                                       : reg_type.GetClass();
  }

  bool CanAccess(const RegType& other) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(other.IsJavaLangObject() || other.IsReference() || other.IsUnresolvedReference());
    const RegType& declaring_class = GetDeclaringClass();
    if (declaring_class.Equals(other)) {
      return true;  // Trivial accessibility.
    } else if (other.IsUnresolvedReference()) {
      return false;  // More complicated test not possible on unresolved types, be conservative.
    } else if (declaring_class.IsUnresolvedReference()) {
      // Be conservative, only allow if `other` is public.
      return other.IsJavaLangObject() || (other.IsReference() && other.GetClass()->IsPublic());
    } else {
      return GetRegTypeClass(declaring_class)->CanAccess(GetRegTypeClass(other));
    }
  }

  bool CanAccessMember(ObjPtr<mirror::Class> klass, uint32_t access_flags)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const RegType& declaring_class = GetDeclaringClass();
    if (declaring_class.IsUnresolvedReference()) {
      return false;  // More complicated test not possible on unresolved types, be conservative.
    } else {
      return GetRegTypeClass(declaring_class)->CanAccessMember(klass, access_flags);
    }
  }

  NO_INLINE void FailInvalidArgCount(const Instruction* inst, uint32_t arg_count) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "invalid arg count (" << arg_count << ") in " << inst->Name();
  }

  NO_INLINE void FailUnexpectedOpcode(const Instruction* inst) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected opcode " << inst->Name();
  }

  NO_INLINE void FailBadFieldIndex(uint32_t field_idx) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "bad field index " << field_idx << " (max " << dex_file_->NumFieldIds() << ")";
  }

  NO_INLINE void FailBadMethodIndex(uint32_t method_idx) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "bad method index " << method_idx << " (max " << dex_file_->NumMethodIds() << ")";
  }

  NO_INLINE void FailForRegisterType(uint32_t vsrc,
                                     const RegType& check_type,
                                     const RegType& src_type,
                                     VerifyError fail_type = VERIFY_ERROR_BAD_CLASS_HARD)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Fail(fail_type)
        << "register v" << vsrc << " has type " << src_type << " but expected " << check_type;
  }

  NO_INLINE void FailForRegisterType(uint32_t vsrc,
                                     RegType::Kind check_kind,
                                     uint16_t src_type_id)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    FailForRegisterType(
        vsrc, reg_types_.GetFromRegKind(check_kind), reg_types_.GetFromId(src_type_id));
  }

  NO_INLINE void FailForRegisterTypeWide(uint32_t vsrc,
                                         const RegType& src_type,
                                         const RegType& src_type_h)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "wide register v" << vsrc << " has type " << src_type << "/" << src_type_h;
  }

  NO_INLINE void FailForRegisterTypeWide(uint32_t vsrc,
                                         uint16_t src_type_id,
                                         uint16_t src_type_id_h)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    FailForRegisterTypeWide(
        vsrc, reg_types_.GetFromId(src_type_id), reg_types_.GetFromId(src_type_id_h));
  }

  ALWAYS_INLINE inline bool VerifyRegisterType(uint32_t vsrc, const RegType& check_type)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Verify the src register type against the check type refining the type of the register
    const RegType& src_type = work_line_->GetRegisterType(this, vsrc);
    if (UNLIKELY(!IsAssignableFrom(check_type, src_type))) {
      enum VerifyError fail_type;
      if (!check_type.IsNonZeroReferenceTypes() || !src_type.IsNonZeroReferenceTypes()) {
        // Hard fail if one of the types is primitive, since they are concretely known.
        fail_type = VERIFY_ERROR_BAD_CLASS_HARD;
      } else if (check_type.IsUninitializedTypes() || src_type.IsUninitializedTypes()) {
        // Hard fail for uninitialized types, which don't match anything but themselves.
        fail_type = VERIFY_ERROR_BAD_CLASS_HARD;
      } else if (check_type.IsUnresolvedTypes() || src_type.IsUnresolvedTypes()) {
        fail_type = VERIFY_ERROR_UNRESOLVED_TYPE_CHECK;
      } else {
        fail_type = VERIFY_ERROR_BAD_CLASS_HARD;
      }
      FailForRegisterType(vsrc, check_type, src_type, fail_type);
      return false;
    }
    if (check_type.IsLowHalf()) {
      const RegType& src_type_h = work_line_->GetRegisterType(this, vsrc + 1);
      if (UNLIKELY(!src_type.CheckWidePair(src_type_h))) {
        FailForRegisterTypeWide(vsrc, src_type, src_type_h);
        return false;
      }
    }
    // The register at vsrc has a defined type, we know the lower-upper-bound, but this is less
    // precise than the subtype in vsrc so leave it for reference types. For primitive types if
    // they are a defined type then they are as precise as we can get, however, for constant types
    // we may wish to refine them. Unfortunately constant propagation has rendered this useless.
    return true;
  }

  ALWAYS_INLINE inline bool VerifyRegisterType(uint32_t vsrc, RegType::Kind check_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(check_kind == RegType::Kind::kInteger || check_kind == RegType::Kind::kFloat);
    // Verify the src register type against the check type refining the type of the register
    uint16_t src_type_id = work_line_->GetRegisterTypeId(vsrc);
    if (UNLIKELY(src_type_id >= RegTypeCache::NumberOfRegKindCacheIds()) ||
        UNLIKELY(RegType::AssignabilityFrom(check_kind, RegTypeCache::RegKindForId(src_type_id)) !=
                     RegType::Assignability::kAssignable)) {
      // Integer or float assignability is never a `kNarrowingConversion` or `kReference`.
      DCHECK_EQ(
          RegType::AssignabilityFrom(check_kind, reg_types_.GetFromId(src_type_id).GetKind()),
          RegType::Assignability::kNotAssignable);
      FailForRegisterType(vsrc, check_kind, src_type_id);
      return false;
    }
    return true;
  }

  bool VerifyRegisterTypeWide(uint32_t vsrc, RegType::Kind check_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(check_kind == RegType::Kind::kLongLo || check_kind == RegType::Kind::kDoubleLo);
    // Verify the src register type against the check type refining the type of the register
    uint16_t src_type_id = work_line_->GetRegisterTypeId(vsrc);
    if (UNLIKELY(src_type_id >= RegTypeCache::NumberOfRegKindCacheIds()) ||
        UNLIKELY(RegType::AssignabilityFrom(check_kind, RegTypeCache::RegKindForId(src_type_id)) !=
                     RegType::Assignability::kAssignable)) {
      // Wide assignability is never a `kNarrowingConversion` or `kReference`.
      DCHECK_EQ(
          RegType::AssignabilityFrom(check_kind, reg_types_.GetFromId(src_type_id).GetKind()),
          RegType::Assignability::kNotAssignable);
      FailForRegisterType(vsrc, check_kind, src_type_id);
      return false;
    }
    uint16_t src_type_id_h = work_line_->GetRegisterTypeId(vsrc + 1);
    uint16_t expected_src_type_id_h =
        RegTypeCache::IdForRegKind(RegType::ToHighHalf(RegTypeCache::RegKindForId(src_type_id)));
    DCHECK_EQ(src_type_id_h == expected_src_type_id_h,
              reg_types_.GetFromId(src_type_id).CheckWidePair(reg_types_.GetFromId(src_type_id_h)));
    if (UNLIKELY(src_type_id_h != expected_src_type_id_h)) {
      FailForRegisterTypeWide(vsrc, src_type_id, src_type_id_h);
      return false;
    }
    // The register at vsrc has a defined type, we know the lower-upper-bound, but this is less
    // precise than the subtype in vsrc so leave it for reference types. For primitive types if
    // they are a defined type then they are as precise as we can get, however, for constant types
    // we may wish to refine them. Unfortunately constant propagation has rendered this useless.
    return true;
  }

  /*
   * Verify types for a simple two-register instruction (e.g. "neg-int").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   */
  void CheckUnaryOp(const Instruction* inst, RegType::Kind dst_kind, RegType::Kind src_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterType(inst->VRegB_12x(), src_kind)) {
      work_line_->SetRegisterType(inst->VRegA_12x(), dst_kind);
    }
  }

  void CheckUnaryOpWide(const Instruction* inst,
                        RegType::Kind dst_kind,
                        RegType::Kind src_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterTypeWide(inst->VRegB_12x(), src_kind)) {
      work_line_->SetRegisterTypeWide(inst->VRegA_12x(), dst_kind, RegType::ToHighHalf(dst_kind));
    }
  }

  void CheckUnaryOpToWide(const Instruction* inst,
                          RegType::Kind dst_kind,
                          RegType::Kind src_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterType(inst->VRegB_12x(), src_kind)) {
      work_line_->SetRegisterTypeWide(inst->VRegA_12x(), dst_kind, RegType::ToHighHalf(dst_kind));
    }
  }

  void CheckUnaryOpFromWide(const Instruction* inst,
                            RegType::Kind dst_kind,
                            RegType::Kind src_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterTypeWide(inst->VRegB_12x(), src_kind)) {
      work_line_->SetRegisterType(inst->VRegA_12x(), dst_kind);
    }
  }

  /*
   * Verify types for a simple three-register instruction (e.g. "add-int").
   * "dst_type" is stored into vA, and "src_type1"/"src_type2" are verified
   * against vB/vC.
   */
  void CheckBinaryOp(const Instruction* inst,
                     RegType::Kind dst_kind,
                     RegType::Kind src_kind1,
                     RegType::Kind src_kind2,
                     bool check_boolean_op)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint32_t vregA = inst->VRegA_23x();
    const uint32_t vregB = inst->VRegB_23x();
    const uint32_t vregC = inst->VRegC_23x();
    if (VerifyRegisterType(vregB, src_kind1) &&
        VerifyRegisterType(vregC, src_kind2)) {
      if (check_boolean_op) {
        DCHECK_EQ(dst_kind, RegType::Kind::kInteger);
        if (RegType::IsBooleanTypes(
                RegTypeCache::RegKindForId(work_line_->GetRegisterTypeId(vregB))) &&
            RegType::IsBooleanTypes(
                RegTypeCache::RegKindForId(work_line_->GetRegisterTypeId(vregC)))) {
          work_line_->SetRegisterType(vregA, RegType::Kind::kBoolean);
          return;
        }
      }
      work_line_->SetRegisterType(vregA, dst_kind);
    }
  }

  void CheckBinaryOpWide(const Instruction* inst,
                         RegType::Kind dst_kind,
                         RegType::Kind src_kind1,
                         RegType::Kind src_kind2)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterTypeWide(inst->VRegB_23x(), src_kind1) &&
        VerifyRegisterTypeWide(inst->VRegC_23x(), src_kind2)) {
      work_line_->SetRegisterTypeWide(inst->VRegA_23x(), dst_kind, RegType::ToHighHalf(dst_kind));
    }
  }

  void CheckBinaryOpWideCmp(const Instruction* inst,
                            RegType::Kind dst_kind,
                            RegType::Kind src_kind1,
                            RegType::Kind src_kind2)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterTypeWide(inst->VRegB_23x(), src_kind1) &&
        VerifyRegisterTypeWide(inst->VRegC_23x(), src_kind2)) {
      work_line_->SetRegisterType(inst->VRegA_23x(), dst_kind);
    }
  }

  void CheckBinaryOpWideShift(const Instruction* inst,
                              RegType::Kind long_lo_kind,
                              RegType::Kind int_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (VerifyRegisterTypeWide(inst->VRegB_23x(), long_lo_kind) &&
        VerifyRegisterType(inst->VRegC_23x(), int_kind)) {
      RegType::Kind long_hi_kind = RegType::ToHighHalf(long_lo_kind);
      work_line_->SetRegisterTypeWide(inst->VRegA_23x(), long_lo_kind, long_hi_kind);
    }
  }

  /*
   * Verify types for a binary "2addr" operation. "src_type1"/"src_type2"
   * are verified against vA/vB, then "dst_type" is stored into vA.
   */
  void CheckBinaryOp2addr(const Instruction* inst,
                          RegType::Kind dst_kind,
                          RegType::Kind src_kind1,
                          RegType::Kind src_kind2,
                          bool check_boolean_op)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint32_t vregA = inst->VRegA_12x();
    const uint32_t vregB = inst->VRegB_12x();
    if (VerifyRegisterType(vregA, src_kind1) &&
        VerifyRegisterType(vregB, src_kind2)) {
      if (check_boolean_op) {
        DCHECK_EQ(dst_kind, RegType::Kind::kInteger);
        if (RegType::IsBooleanTypes(
                RegTypeCache::RegKindForId(work_line_->GetRegisterTypeId(vregA))) &&
            RegType::IsBooleanTypes(
                RegTypeCache::RegKindForId(work_line_->GetRegisterTypeId(vregB)))) {
          work_line_->SetRegisterType(vregA, RegType::Kind::kBoolean);
          return;
        }
      }
      work_line_->SetRegisterType(vregA, dst_kind);
    }
  }

  void CheckBinaryOp2addrWide(const Instruction* inst,
                              RegType::Kind dst_kind,
                              RegType::Kind src_kind1,
                              RegType::Kind src_kind2)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint32_t vregA = inst->VRegA_12x();
    const uint32_t vregB = inst->VRegB_12x();
    if (VerifyRegisterTypeWide(vregA, src_kind1) &&
        VerifyRegisterTypeWide(vregB, src_kind2)) {
      work_line_->SetRegisterTypeWide(vregA, dst_kind, RegType::ToHighHalf(dst_kind));
    }
  }

  void CheckBinaryOp2addrWideShift(const Instruction* inst,
                                   RegType::Kind long_lo_kind,
                                   RegType::Kind int_kind)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint32_t vregA = inst->VRegA_12x();
    const uint32_t vregB = inst->VRegB_12x();
    if (VerifyRegisterTypeWide(vregA, long_lo_kind) &&
        VerifyRegisterType(vregB, int_kind)) {
      RegType::Kind long_hi_kind = RegType::ToHighHalf(long_lo_kind);
      work_line_->SetRegisterTypeWide(vregA, long_lo_kind, long_hi_kind);
    }
  }

  /*
   * Verify types for A two-register instruction with a literal constant (e.g. "add-int/lit8").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   *
   * If "check_boolean_op" is set, we use the constant value in vC.
   */
  void CheckLiteralOp(const Instruction* inst,
                      RegType::Kind dst_kind,
                      RegType::Kind src_kind,
                      bool check_boolean_op,
                      bool is_lit16)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint32_t vregA = is_lit16 ? inst->VRegA_22s() : inst->VRegA_22b();
    const uint32_t vregB = is_lit16 ? inst->VRegB_22s() : inst->VRegB_22b();
    if (VerifyRegisterType(vregB, src_kind)) {
      if (check_boolean_op) {
        DCHECK_EQ(dst_kind, RegType::Kind::kInteger);
        /* check vB with the call, then check the constant manually */
        const uint32_t val = is_lit16 ? inst->VRegC_22s() : inst->VRegC_22b();
        if (work_line_->GetRegisterType(this, vregB).IsBooleanTypes() && (val == 0 || val == 1)) {
          work_line_->SetRegisterType(vregA, RegType::Kind::kBoolean);
          return;
        }
      }
      work_line_->SetRegisterType(vregA, dst_kind);
    }
  }

  InstructionFlags* CurrentInsnFlags() {
    return &GetModifiableInstructionFlags(work_insn_idx_);
  }

  RegType::Kind DetermineCat1Constant(int32_t value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE bool FailOrAbort(bool condition, const char* error_msg, uint32_t work_insn_idx);

  ALWAYS_INLINE InstructionFlags& GetModifiableInstructionFlags(size_t index) {
    return insn_flags_[index];
  }

  // Returns the method index of an invoke instruction.
  static uint16_t GetMethodIdxOfInvoke(const Instruction* inst)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Note: This is compiled to a single load in release mode.
    Instruction::Code opcode = inst->Opcode();
    if (opcode == Instruction::INVOKE_VIRTUAL ||
        opcode == Instruction::INVOKE_SUPER ||
        opcode == Instruction::INVOKE_DIRECT ||
        opcode == Instruction::INVOKE_STATIC ||
        opcode == Instruction::INVOKE_INTERFACE ||
        opcode == Instruction::INVOKE_CUSTOM) {
      return inst->VRegB_35c();
    } else if (opcode == Instruction::INVOKE_VIRTUAL_RANGE ||
               opcode == Instruction::INVOKE_SUPER_RANGE ||
               opcode == Instruction::INVOKE_DIRECT_RANGE ||
               opcode == Instruction::INVOKE_STATIC_RANGE ||
               opcode == Instruction::INVOKE_INTERFACE_RANGE ||
               opcode == Instruction::INVOKE_CUSTOM_RANGE) {
      return inst->VRegB_3rc();
    } else if (opcode == Instruction::INVOKE_POLYMORPHIC) {
      return inst->VRegB_45cc();
    } else {
      DCHECK_EQ(opcode, Instruction::INVOKE_POLYMORPHIC_RANGE);
      return inst->VRegB_4rcc();
    }
  }
  // Returns the field index of a field access instruction.
  uint16_t GetFieldIdxOfFieldAccess(const Instruction* inst, bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (is_static) {
      return inst->VRegB_21c();
    } else {
      return inst->VRegC_22c();
    }
  }

  // Run verification on the method. Returns true if verification completes and false if the input
  // has an irrecoverable corruption.
  bool Verify() override REQUIRES_SHARED(Locks::mutator_lock_);

  // For app-compatibility, code after a runtime throw is treated as dead code
  // for apps targeting <= S.
  // Returns whether the current instruction was marked as throwing.
  bool PotentiallyMarkRuntimeThrow() override;

  // Dump the failures encountered by the verifier.
  std::ostream& DumpFailures(std::ostream& os) {
    DCHECK_EQ(failures_.size(), failure_messages_.size());
    for (const auto* stream : failure_messages_) {
        os << stream->str() << "\n";
    }
    return os;
  }

  // Dump the state of the verifier, namely each instruction, what flags are set on it, register
  // information
  void Dump(std::ostream& os) REQUIRES_SHARED(Locks::mutator_lock_) {
    VariableIndentationOutputStream vios(&os);
    Dump(&vios);
  }
  void Dump(VariableIndentationOutputStream* vios) REQUIRES_SHARED(Locks::mutator_lock_);

  bool HandleMoveException(const Instruction* inst) REQUIRES_SHARED(Locks::mutator_lock_);

  const uint32_t method_access_flags_;  // Method's access flags.
  const RegType* return_type_;  // Lazily computed return type of the method.
  // The dex_cache for the declaring class of the method.
  Handle<mirror::DexCache> dex_cache_ GUARDED_BY(Locks::mutator_lock_);
  // The class loader for the declaring class of the method.
  Handle<mirror::ClassLoader> class_loader_ GUARDED_BY(Locks::mutator_lock_);
  const RegType* declaring_class_;  // Lazily computed reg type of the method's declaring class.

  // The dex PC of a FindLocksAtDexPc request, -1 otherwise.
  uint32_t interesting_dex_pc_;
  // The container into which FindLocksAtDexPc should write the registers containing held locks,
  // null if we're not doing FindLocksAtDexPc.
  std::vector<DexLockInfo>* monitor_enter_dex_pcs_;

  // Indicates whether we verify to dump the info. In that case we accept quickened instructions
  // even though we might detect to be a compiler. Should only be set when running
  // VerifyMethodAndDump.
  const bool verify_to_dump_;

  // Whether or not we call AllowThreadSuspension periodically, we want a way to disable this for
  // thread dumping checkpoints since we may get thread suspension at an inopportune time due to
  // FindLocksAtDexPC, resulting in deadlocks.
  const bool allow_thread_suspension_;

  // Whether the method seems to be a constructor. Note that this field exists as we can't trust
  // the flags in the dex file. Some older code does not mark methods named "<init>" and "<clinit>"
  // correctly.
  //
  // Note: this flag is only valid once Verify() has started.
  bool is_constructor_;

  // API level, for dependent checks. Note: we do not use '0' for unset here, to simplify checks.
  // Instead, unset level should correspond to max().
  const uint32_t api_level_;

  friend class ::art::verifier::MethodVerifier;

  DISALLOW_COPY_AND_ASSIGN(MethodVerifier);
};

// Note: returns true on failure.
template <bool kVerifierDebug>
inline bool MethodVerifier<kVerifierDebug>::FailOrAbort(bool condition,
                                                        const char* error_msg,
                                                        uint32_t work_insn_idx) {
  if (kIsDebugBuild) {
    // In a debug build, abort if the error condition is wrong. Only warn if
    // we are already aborting (as this verification is likely run to print
    // lock information).
    if (LIKELY(gAborting == 0)) {
      DCHECK(condition) << error_msg << work_insn_idx << " "
                        << dex_file_->PrettyMethod(dex_method_idx_);
    } else {
      if (!condition) {
        LOG(ERROR) << error_msg << work_insn_idx;
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << error_msg << work_insn_idx;
        return true;
      }
    }
  } else {
    // In a non-debug build, just fail the class.
    if (!condition) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << error_msg << work_insn_idx;
      return true;
    }
  }

  return false;
}

static bool IsLargeMethod(const CodeItemDataAccessor& accessor) {
  if (!accessor.HasCodeItem()) {
    return false;
  }

  uint16_t registers_size = accessor.RegistersSize();
  uint32_t insns_size = accessor.InsnsSizeInCodeUnits();

  return registers_size * insns_size > 4*1024*1024;
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::FindLocksAtDexPc() {
  CHECK(monitor_enter_dex_pcs_ != nullptr);
  CHECK(code_item_accessor_.HasCodeItem());  // This only makes sense for methods with code.

  // Quick check whether there are any monitor_enter instructions before verifying.
  for (const DexInstructionPcPair& inst : code_item_accessor_) {
    if (inst->Opcode() == Instruction::MONITOR_ENTER) {
      // Strictly speaking, we ought to be able to get away with doing a subset of the full method
      // verification. In practice, the phase we want relies on data structures set up by all the
      // earlier passes, so we just run the full method verification and bail out early when we've
      // got what we wanted.
      Verify();
      return;
    }
  }
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::Verify() {
  // Some older code doesn't correctly mark constructors as such, so we need look
  // at the name if the constructor flag is not present.
  if ((method_access_flags_ & kAccConstructor) != 0) {
    // `DexFileVerifier` rejects methods with the constructor flag without a constructor name.
    DCHECK(dex_file_->GetMethodNameView(dex_method_idx_) == "<init>" ||
           dex_file_->GetMethodNameView(dex_method_idx_) == "<clinit>");
    is_constructor_ = true;
  } else if (dex_file_->GetMethodName(dex_method_idx_)[0] == '<') {
    // `DexFileVerifier` rejects method names starting with '<' other than constructors.
    DCHECK(dex_file_->GetMethodNameView(dex_method_idx_) == "<init>" ||
           dex_file_->GetMethodNameView(dex_method_idx_) == "<clinit>");
    LOG(WARNING) << "Method " << dex_file_->PrettyMethod(dex_method_idx_)
                 << " not marked as constructor.";
    is_constructor_ = true;
  }
  // If it's a constructor, check whether IsStatic() matches the name for newer dex files.
  // This should be rejected by the `DexFileVerifier` but it's  accepted for older dex files.
  if (kIsDebugBuild && IsConstructor() && dex_file_->SupportsDefaultMethods()) {
    CHECK_EQ(IsStatic(), dex_file_->GetMethodNameView(dex_method_idx_) == "<clinit>");
  }

  // Methods may only have one of public/protected/private.
  // This should have been rejected by the dex file verifier. Only do in debug build.
  constexpr uint32_t kAccPublicProtectedPrivate = kAccPublic | kAccProtected | kAccPrivate;
  DCHECK_IMPLIES((method_access_flags_ & kAccPublicProtectedPrivate) != 0u,
                 IsPowerOfTwo(method_access_flags_ & kAccPublicProtectedPrivate));

  // If there aren't any instructions, make sure that's expected, then exit successfully.
  if (!code_item_accessor_.HasCodeItem()) {
    // Only native or abstract methods may not have code.
    if ((method_access_flags_ & (kAccNative | kAccAbstract)) == 0) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "zero-length code in concrete non-native method";
      return false;
    }

    // Test FastNative and CriticalNative annotations. We do this in the
    // verifier for convenience.
    if ((method_access_flags_ & kAccNative) != 0) {
      // Fetch the flags from the annotations: the class linker hasn't processed
      // them yet.
      uint32_t native_access_flags = annotations::GetNativeMethodAnnotationAccessFlags(
          *dex_file_, class_def_, dex_method_idx_);
      if ((native_access_flags & kAccFastNative) != 0) {
        if ((method_access_flags_ & kAccSynchronized) != 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "fast native methods cannot be synchronized";
          return false;
        }
      }
      if ((native_access_flags & kAccCriticalNative) != 0) {
        if ((method_access_flags_ & kAccSynchronized) != 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "critical native methods cannot be synchronized";
          return false;
        }
        if ((method_access_flags_ & kAccStatic) == 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "critical native methods must be static";
          return false;
        }
        const char* shorty = dex_file_->GetMethodShorty(dex_method_idx_);
        for (size_t i = 0, len = strlen(shorty); i < len; ++i) {
          if (Primitive::GetType(shorty[i]) == Primitive::kPrimNot) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) <<
                "critical native methods must not have references as arguments or return type";
            return false;
          }
        }
      }
    }

    // This should have been rejected by the dex file verifier. Only do in debug build.
    // Note: the above will also be rejected in the dex file verifier, starting in dex version 37.
    if (kIsDebugBuild) {
      if ((method_access_flags_ & kAccAbstract) != 0) {
        // Abstract methods are not allowed to have the following flags.
        static constexpr uint32_t kForbidden =
            kAccPrivate |
            kAccStatic |
            kAccFinal |
            kAccNative |
            kAccStrict |
            kAccSynchronized;
        if ((method_access_flags_ & kForbidden) != 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD)
                << "method can't be abstract and private/static/final/native/strict/synchronized";
          return false;
        }
      }
      if ((class_def_.GetJavaAccessFlags() & kAccInterface) != 0) {
        // Interface methods must be public and abstract (if default methods are disabled).
        uint32_t kRequired = kAccPublic;
        if ((method_access_flags_ & kRequired) != kRequired) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "interface methods must be public";
          return false;
        }
        // In addition to the above, interface methods must not be protected.
        static constexpr uint32_t kForbidden = kAccProtected;
        if ((method_access_flags_ & kForbidden) != 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "interface methods can't be protected";
          return false;
        }
      }
      // We also don't allow constructors to be abstract or native.
      if (IsConstructor()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "constructors can't be abstract or native";
        return false;
      }
    }
    return true;
  }

  // This should have been rejected by the dex file verifier. Only do in debug build.
  if (kIsDebugBuild) {
    // When there's code, the method must not be native or abstract.
    if ((method_access_flags_ & (kAccNative | kAccAbstract)) != 0) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "non-zero-length code in abstract or native method";
      return false;
    }

    if ((class_def_.GetJavaAccessFlags() & kAccInterface) != 0) {
      // Interfaces may always have static initializers for their fields. If we are running with
      // default methods enabled we also allow other public, static, non-final methods to have code.
      // Otherwise that is the only type of method allowed.
      if (!(IsConstructor() && IsStatic())) {
        if (IsInstanceConstructor()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "interfaces may not have non-static constructor";
          return false;
        } else if (method_access_flags_ & kAccFinal) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "interfaces may not have final methods";
          return false;
        } else {
          uint32_t access_flag_options = kAccPublic;
          if (dex_file_->SupportsDefaultMethods()) {
            access_flag_options |= kAccPrivate;
          }
          if (!(method_access_flags_ & access_flag_options)) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD)
                << "interfaces may not have protected or package-private members";
            return false;
          }
        }
      }
    }

    // Instance constructors must not be synchronized.
    if (IsInstanceConstructor()) {
      static constexpr uint32_t kForbidden = kAccSynchronized;
      if ((method_access_flags_ & kForbidden) != 0) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "constructors can't be synchronized";
        return false;
      }
    }
  }

  // Consistency-check of the register counts.
  // ins + locals = registers, so make sure that ins <= registers.
  if (code_item_accessor_.InsSize() > code_item_accessor_.RegistersSize()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad register counts (ins="
                                      << code_item_accessor_.InsSize()
                                      << " regs=" << code_item_accessor_.RegistersSize();
    return false;
  }

  // Allocate and initialize an array to hold instruction data.
  insn_flags_.reset(allocator_.AllocArray<InstructionFlags>(
      code_item_accessor_.InsnsSizeInCodeUnits()));
  DCHECK(insn_flags_ != nullptr);
  // `ArenaAllocator` guarantees zero-initialization.
  static_assert(std::is_same_v<decltype(allocator_), ArenaAllocator>);
  DCHECK(std::all_of(
      insn_flags_.get(),
      insn_flags_.get() + code_item_accessor_.InsnsSizeInCodeUnits(),
      [](const InstructionFlags& flags) { return flags.Equals(InstructionFlags()); }));
  // Run through the instructions and see if the width checks out.
  bool result = ComputeWidthsAndCountOps();
  // Flag instructions guarded by a "try" block and check exception handlers.
  result = result && ScanTryCatchBlocks();
  // Perform static instruction verification.
  result = result && VerifyInstructions();
  // Perform code-flow analysis and return.
  result = result && VerifyCodeFlow();

  return result;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::ComputeWidthsAndCountOps() {
  // We can't assume the instruction is well formed, handle the case where calculating the size
  // goes past the end of the code item.
  SafeDexInstructionIterator it(code_item_accessor_.begin(), code_item_accessor_.end());
  if (it == code_item_accessor_.end()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "code item has no opcode";
    return false;
  }
  for ( ; !it.IsErrorState() && it < code_item_accessor_.end(); ++it) {
    // In case the instruction goes past the end of the code item, make sure to not process it.
    SafeDexInstructionIterator next = it;
    ++next;
    if (next.IsErrorState()) {
      break;
    }
    GetModifiableInstructionFlags(it.DexPc()).SetIsOpcode();
  }

  if (it != code_item_accessor_.end()) {
    const size_t insns_size = code_item_accessor_.InsnsSizeInCodeUnits();
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "code did not end where expected ("
                                      << it.DexPc() << " vs. " << insns_size << ")";
    return false;
  }
  DCHECK(GetInstructionFlags(0).IsOpcode());

  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::ScanTryCatchBlocks() {
  const uint32_t tries_size = code_item_accessor_.TriesSize();
  if (tries_size == 0) {
    return true;
  }
  const uint32_t insns_size = code_item_accessor_.InsnsSizeInCodeUnits();
  for (const dex::TryItem& try_item : code_item_accessor_.TryItems()) {
    const uint32_t start = try_item.start_addr_;
    const uint32_t end = start + try_item.insn_count_;
    if ((start >= end) || (start >= insns_size) || (end > insns_size)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad exception entry: startAddr=" << start
                                        << " endAddr=" << end << " (size=" << insns_size << ")";
      return false;
    }
    if (!GetInstructionFlags(start).IsOpcode()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD)
          << "'try' block starts inside an instruction (" << start << ")";
      return false;
    }
    DexInstructionIterator end_it(code_item_accessor_.Insns(), end);
    for (DexInstructionIterator it(code_item_accessor_.Insns(), start); it < end_it; ++it) {
      GetModifiableInstructionFlags(it.DexPc()).SetInTry();
    }
  }
  // Iterate over each of the handlers to verify target addresses.
  const uint8_t* handlers_ptr = code_item_accessor_.GetCatchHandlerData();
  const uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  ClassLinker* linker = GetClassLinker();
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      uint32_t dex_pc = iterator.GetHandlerAddress();
      if (!GetInstructionFlags(dex_pc).IsOpcode()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD)
            << "exception handler starts at bad address (" << dex_pc << ")";
        return false;
      }
      if (!CheckNotMoveResult(code_item_accessor_.Insns(), dex_pc)) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD)
            << "exception handler begins with move-result* (" << dex_pc << ")";
        return false;
      }
      GetModifiableInstructionFlags(dex_pc).SetBranchTarget();
      // Ensure exception types are resolved so that they don't need resolution to be delivered,
      // unresolved exception types will be ignored by exception delivery
      if (iterator.GetHandlerTypeIndex().IsValid()) {
        ObjPtr<mirror::Class> exception_type =
            linker->ResolveType(iterator.GetHandlerTypeIndex(), dex_cache_, class_loader_);
        if (exception_type == nullptr) {
          DCHECK(self_->IsExceptionPending());
          self_->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::VerifyInstructions() {
  // Flag the start of the method as a branch target.
  GetModifiableInstructionFlags(0).SetBranchTarget();
  const Instruction* inst = Instruction::At(code_item_accessor_.Insns());
  uint32_t dex_pc = 0u;
  const uint32_t end_dex_pc = code_item_accessor_.InsnsSizeInCodeUnits();
  while (dex_pc != end_dex_pc) {
    auto find_dispatch_opcode = [](Instruction::Code opcode) constexpr {
      // NOP needs its own dipatch because it needs special code for instruction size.
      if (opcode == Instruction::NOP) {
        return opcode;
      }
      DCHECK_GT(Instruction::SizeInCodeUnits(Instruction::FormatOf(opcode)), 0u);
      for (uint32_t raw_other = 0; raw_other != opcode; ++raw_other) {
        Instruction::Code other = enum_cast<Instruction::Code>(raw_other);
        if (other == Instruction::NOP) {
          continue;
        }
        // We dispatch to `VerifyInstruction()` based on the format and verify flags but
        // we also treat return instructions separately to update instruction flags.
        if (Instruction::FormatOf(opcode) == Instruction::FormatOf(other) &&
            Instruction::VerifyFlagsOf(opcode) == Instruction::VerifyFlagsOf(other) &&
            Instruction::IsReturn(opcode) == Instruction::IsReturn(other)) {
          return other;
        }
      }
      return opcode;
    };

    uint16_t inst_data = inst->Fetch16(0);
    Instruction::Code dispatch_opcode = Instruction::NOP;
    switch (inst->Opcode(inst_data)) {
#define DEFINE_CASE(opcode, c, p, format, index, flags, eflags, vflags) \
      case opcode: {                                                    \
        /* Enforce compile-time evaluation. */                          \
        constexpr Instruction::Code kDispatchOpcode =                   \
            find_dispatch_opcode(enum_cast<Instruction::Code>(opcode)); \
        dispatch_opcode = kDispatchOpcode;                              \
        break;                                                          \
      }
      DEX_INSTRUCTION_LIST(DEFINE_CASE)
#undef DEFINE_CASE
    }
    bool is_return = false;
    uint32_t instruction_size = 0u;
    switch (dispatch_opcode) {
#define DEFINE_CASE(opcode, c, p, format, index, flags, eflags, vflags)             \
      case opcode: {                                                                \
        constexpr Instruction::Code kOpcode = enum_cast<Instruction::Code>(opcode); \
        if (!VerifyInstruction<kOpcode>(inst, dex_pc, inst_data)) {                 \
          DCHECK_NE(failures_.size(), 0U);                                          \
          return false;                                                             \
        }                                                                           \
        is_return = Instruction::IsReturn(kOpcode);                                 \
        instruction_size = (opcode == Instruction::NOP)                             \
            ? inst->SizeInCodeUnitsComplexOpcode()                                  \
            : Instruction::SizeInCodeUnits(Instruction::FormatOf(kOpcode));         \
        DCHECK_EQ(instruction_size, inst->SizeInCodeUnits());                       \
        break;                                                                      \
      }
      DEX_INSTRUCTION_LIST(DEFINE_CASE)
#undef DEFINE_CASE
    }
    // Flag some interesting instructions.
    if (is_return) {
      GetModifiableInstructionFlags(dex_pc).SetReturn();
    }
    DCHECK_NE(instruction_size, 0u);
    DCHECK_LE(instruction_size, end_dex_pc - dex_pc);
    dex_pc += instruction_size;
    inst = inst->RelativeAt(instruction_size);
  }
  return true;
}

template <bool kVerifierDebug>
template <Instruction::Code kDispatchOpcode>
inline bool MethodVerifier<kVerifierDebug>::VerifyInstruction(const Instruction* inst,
                                                              uint32_t code_offset,
                                                              uint16_t inst_data) {
  // The `kDispatchOpcode` may differ from the actual opcode but it shall have the
  // same verification flags and format. We explicitly `DCHECK` these below and
  // the format is also `DCHECK`ed in VReg getters that take it as an argument.
  constexpr Instruction::Format kFormat = Instruction::FormatOf(kDispatchOpcode);
  DCHECK_EQ(kFormat, Instruction::FormatOf(inst->Opcode()));

  bool result = true;
  constexpr uint32_t kVerifyA = Instruction::GetVerifyTypeArgumentAOf(kDispatchOpcode);
  DCHECK_EQ(kVerifyA, inst->GetVerifyTypeArgumentA());
  switch (kVerifyA) {
    case Instruction::kVerifyRegA:
      result = result && CheckRegisterIndex(inst->VRegA(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegAWide:
      result = result && CheckWideRegisterIndex(inst->VRegA(kFormat, inst_data));
      break;
    case Instruction::kVerifyNothing:
      break;
  }
  constexpr uint32_t kVerifyB = Instruction::GetVerifyTypeArgumentBOf(kDispatchOpcode);
  DCHECK_EQ(kVerifyB, inst->GetVerifyTypeArgumentB());
  switch (kVerifyB) {
    case Instruction::kVerifyRegB:
      result = result && CheckRegisterIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBField:
      result = result && CheckFieldIndex(inst, inst_data, inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBMethod:
      result = result && CheckMethodIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBNewInstance:
      result = result && CheckNewInstance(dex::TypeIndex(inst->VRegB(kFormat, inst_data)));
      break;
    case Instruction::kVerifyRegBString:
      result = result && CheckStringIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBType:
      result = result && CheckTypeIndex(dex::TypeIndex(inst->VRegB(kFormat, inst_data)));
      break;
    case Instruction::kVerifyRegBWide:
      result = result && CheckWideRegisterIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBCallSite:
      result = result && CheckCallSiteIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBMethodHandle:
      result = result && CheckMethodHandleIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyRegBPrototype:
      result = result && CheckPrototypeIndex(inst->VRegB(kFormat, inst_data));
      break;
    case Instruction::kVerifyNothing:
      break;
  }
  constexpr uint32_t kVerifyC = Instruction::GetVerifyTypeArgumentCOf(kDispatchOpcode);
  DCHECK_EQ(kVerifyC, inst->GetVerifyTypeArgumentC());
  switch (kVerifyC) {
    case Instruction::kVerifyRegC:
      result = result && CheckRegisterIndex(inst->VRegC(kFormat));
      break;
    case Instruction::kVerifyRegCField:
      result = result && CheckFieldIndex(inst, inst_data, inst->VRegC(kFormat));
      break;
    case Instruction::kVerifyRegCNewArray:
      result = result && CheckNewArray(dex::TypeIndex(inst->VRegC(kFormat)));
      break;
    case Instruction::kVerifyRegCType:
      result = result && CheckTypeIndex(dex::TypeIndex(inst->VRegC(kFormat)));
      break;
    case Instruction::kVerifyRegCWide:
      result = result && CheckWideRegisterIndex(inst->VRegC(kFormat));
      break;
    case Instruction::kVerifyNothing:
      break;
  }
  constexpr uint32_t kVerifyH = Instruction::GetVerifyTypeArgumentHOf(kDispatchOpcode);
  DCHECK_EQ(kVerifyH, inst->GetVerifyTypeArgumentH());
  switch (kVerifyH) {
    case Instruction::kVerifyRegHPrototype:
      result = result && CheckPrototypeIndex(inst->VRegH(kFormat));
      break;
    case Instruction::kVerifyNothing:
      break;
  }
  constexpr uint32_t kVerifyExtra = Instruction::GetVerifyExtraFlagsOf(kDispatchOpcode);
  DCHECK_EQ(kVerifyExtra, inst->GetVerifyExtraFlags());
  switch (kVerifyExtra) {
    case Instruction::kVerifyArrayData:
      result = result && CheckArrayData(code_offset);
      break;
    case Instruction::kVerifyBranchTarget:
      result = result && CheckBranchTarget(code_offset);
      break;
    case Instruction::kVerifySwitchTargets:
      result = result && CheckSwitchTargets(code_offset);
      break;
    case Instruction::kVerifyVarArgNonZero:
      // Fall-through.
    case Instruction::kVerifyVarArg: {
      // Instructions that can actually return a negative value shouldn't have this flag.
      uint32_t v_a = dchecked_integral_cast<uint32_t>(inst->VRegA(kFormat, inst_data));
      if ((kVerifyExtra == Instruction::kVerifyVarArgNonZero && v_a == 0) ||
          v_a > Instruction::kMaxVarArgRegs) {
        FailInvalidArgCount(inst, v_a);
        return false;
      }

      result = result && CheckVarArgRegs(inst, v_a);
      break;
    }
    case Instruction::kVerifyVarArgRangeNonZero:
      // Fall-through.
    case Instruction::kVerifyVarArgRange: {
      uint32_t v_a = inst->VRegA(kFormat, inst_data);
      if (inst->GetVerifyExtraFlags() == Instruction::kVerifyVarArgRangeNonZero && v_a == 0) {
        FailInvalidArgCount(inst, v_a);
        return false;
      }
      result = result && CheckVarArgRangeRegs(v_a, inst->VRegC(kFormat));
      break;
    }
    case Instruction::kVerifyError:
      FailUnexpectedOpcode(inst);
      result = false;
      break;
    case Instruction::kVerifyNothing:
      break;
  }
  return result;
}

template <bool kVerifierDebug>
inline bool MethodVerifier<kVerifierDebug>::CheckNewInstance(dex::TypeIndex idx) {
  if (UNLIKELY(idx.index_ >= dex_file_->GetHeader().type_ids_size_)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad type index " << idx.index_ << " (max "
                                      << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  // We don't need the actual class, just a pointer to the class name.
  const std::string_view descriptor = dex_file_->GetTypeDescriptorView(idx);
  if (UNLIKELY(descriptor[0] != 'L')) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "can't call new-instance on type '" << descriptor << "'";
    return false;
  } else if (UNLIKELY(descriptor == "Ljava/lang/Class;")) {
    // An unlikely new instance on Class is not allowed. Fall back to interpreter to ensure an
    // exception is thrown when this statement is executed (compiled code would not do that).
    Fail(VERIFY_ERROR_INSTANTIATION);
  }
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckNewArray(dex::TypeIndex idx) {
  if (UNLIKELY(idx.index_ >= dex_file_->GetHeader().type_ids_size_)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad type index " << idx.index_ << " (max "
                                      << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  int bracket_count = 0;
  const char* descriptor = dex_file_->GetTypeDescriptor(idx);
  const char* cp = descriptor;
  while (*cp++ == '[') {
    bracket_count++;
  }
  if (UNLIKELY(bracket_count == 0)) {
    /* The given class must be an array type. */
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "can't new-array class '" << descriptor << "' (not an array)";
    return false;
  } else if (UNLIKELY(bracket_count > 255)) {
    /* It is illegal to create an array of more than 255 dimensions. */
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "can't new-array class '" << descriptor << "' (exceeds limit)";
    return false;
  }
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckArrayData(uint32_t cur_offset) {
  const uint32_t insn_count = code_item_accessor_.InsnsSizeInCodeUnits();
  const uint16_t* insns = code_item_accessor_.Insns() + cur_offset;
  const uint16_t* array_data;
  int32_t array_data_offset;

  DCHECK_LT(cur_offset, insn_count);
  /* make sure the start of the array data table is in range */
  array_data_offset = insns[1] | (static_cast<int32_t>(insns[2]) << 16);
  if (UNLIKELY(static_cast<int32_t>(cur_offset) + array_data_offset < 0 ||
               cur_offset + array_data_offset + 2 >= insn_count)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid array data start: at " << cur_offset
                                      << ", data offset " << array_data_offset
                                      << ", count " << insn_count;
    return false;
  }
  /* offset to array data table is a relative branch-style offset */
  array_data = insns + array_data_offset;
  // Make sure the table is at an even dex pc, that is, 32-bit aligned.
  if (UNLIKELY(!IsAligned<4>(array_data))) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unaligned array data table: at " << cur_offset
                                      << ", data offset " << array_data_offset;
    return false;
  }
  // Make sure the array-data is marked as an opcode. This ensures that it was reached when
  // traversing the code item linearly. It is an approximation for a by-spec padding value.
  if (UNLIKELY(!GetInstructionFlags(cur_offset + array_data_offset).IsOpcode())) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array data table at " << cur_offset
                                      << ", data offset " << array_data_offset
                                      << " not correctly visited, probably bad padding.";
    return false;
  }

  uint32_t value_width = array_data[1];
  uint32_t value_count = *reinterpret_cast<const uint32_t*>(&array_data[2]);
  uint32_t table_size = 4 + (value_width * value_count + 1) / 2;
  /* make sure the end of the switch is in range */
  if (UNLIKELY(cur_offset + array_data_offset + table_size > insn_count)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid array data end: at " << cur_offset
                                      << ", data offset " << array_data_offset << ", end "
                                      << cur_offset + array_data_offset + table_size
                                      << ", count " << insn_count;
    return false;
  }
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckBranchTarget(uint32_t cur_offset) {
  int32_t offset;
  bool isConditional, selfOkay;
  if (!GetBranchOffset(cur_offset, &offset, &isConditional, &selfOkay)) {
    return false;
  }
  if (UNLIKELY(!selfOkay && offset == 0)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "branch offset of zero not allowed at"
                                      << reinterpret_cast<void*>(cur_offset);
    return false;
  }
  // Check for 32-bit overflow. This isn't strictly necessary if we can depend on the runtime
  // to have identical "wrap-around" behavior, but it's unwise to depend on that.
  if (UNLIKELY(((int64_t) cur_offset + (int64_t) offset) != (int64_t) (cur_offset + offset))) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "branch target overflow "
                                      << reinterpret_cast<void*>(cur_offset) << " +" << offset;
    return false;
  }
  int32_t abs_offset = cur_offset + offset;
  if (UNLIKELY(abs_offset < 0 ||
               (uint32_t) abs_offset >= code_item_accessor_.InsnsSizeInCodeUnits()  ||
               !GetInstructionFlags(abs_offset).IsOpcode())) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid branch target " << offset << " (-> "
                                      << reinterpret_cast<void*>(abs_offset) << ") at "
                                      << reinterpret_cast<void*>(cur_offset);
    return false;
  }
  GetModifiableInstructionFlags(abs_offset).SetBranchTarget();
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::GetBranchOffset(uint32_t cur_offset,
                                                     int32_t* pOffset,
                                                     bool* pConditional,
                                                     bool* selfOkay) {
  const uint16_t* insns = code_item_accessor_.Insns() + cur_offset;
  *pConditional = false;
  *selfOkay = false;
  switch (*insns & 0xff) {
    case Instruction::GOTO:
      *pOffset = ((int16_t) *insns) >> 8;
      break;
    case Instruction::GOTO_32:
      *pOffset = insns[1] | (((uint32_t) insns[2]) << 16);
      *selfOkay = true;
      break;
    case Instruction::GOTO_16:
      *pOffset = (int16_t) insns[1];
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      *pOffset = (int16_t) insns[1];
      *pConditional = true;
      break;
    default:
      return false;
  }
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckSwitchTargets(uint32_t cur_offset) {
  const uint32_t insn_count = code_item_accessor_.InsnsSizeInCodeUnits();
  DCHECK_LT(cur_offset, insn_count);
  const uint16_t* insns = code_item_accessor_.Insns() + cur_offset;
  /* make sure the start of the switch is in range */
  int32_t switch_offset = insns[1] | (static_cast<int32_t>(insns[2]) << 16);
  if (UNLIKELY(static_cast<int32_t>(cur_offset) + switch_offset < 0 ||
               cur_offset + switch_offset + 2 > insn_count)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch start: at " << cur_offset
                                      << ", switch offset " << switch_offset
                                      << ", count " << insn_count;
    return false;
  }
  /* offset to switch table is a relative branch-style offset */
  const uint16_t* switch_insns = insns + switch_offset;
  // Make sure the table is at an even dex pc, that is, 32-bit aligned.
  if (UNLIKELY(!IsAligned<4>(switch_insns))) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unaligned switch table: at " << cur_offset
                                      << ", switch offset " << switch_offset;
    return false;
  }
  // Make sure the switch data is marked as an opcode. This ensures that it was reached when
  // traversing the code item linearly. It is an approximation for a by-spec padding value.
  if (UNLIKELY(!GetInstructionFlags(cur_offset + switch_offset).IsOpcode())) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "switch table at " << cur_offset
                                      << ", switch offset " << switch_offset
                                      << " not correctly visited, probably bad padding.";
    return false;
  }

  bool is_packed_switch = (*insns & 0xff) == Instruction::PACKED_SWITCH;

  uint32_t switch_count = switch_insns[1];
  int32_t targets_offset;
  uint16_t expected_signature;
  if (is_packed_switch) {
    /* 0=sig, 1=count, 2/3=firstKey */
    targets_offset = 4;
    expected_signature = Instruction::kPackedSwitchSignature;
  } else {
    /* 0=sig, 1=count, 2..count*2 = keys */
    targets_offset = 2 + 2 * switch_count;
    expected_signature = Instruction::kSparseSwitchSignature;
  }
  uint32_t table_size = targets_offset + switch_count * 2;
  if (UNLIKELY(switch_insns[0] != expected_signature)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << StringPrintf("wrong signature for switch table (%x, wanted %x)",
                        switch_insns[0], expected_signature);
    return false;
  }
  /* make sure the end of the switch is in range */
  if (UNLIKELY(cur_offset + switch_offset + table_size > (uint32_t) insn_count)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch end: at " << cur_offset
                                      << ", switch offset " << switch_offset
                                      << ", end " << (cur_offset + switch_offset + table_size)
                                      << ", count " << insn_count;
    return false;
  }

  constexpr int32_t keys_offset = 2;
  if (switch_count > 1) {
    if (is_packed_switch) {
      /* for a packed switch, verify that keys do not overflow int32 */
      int32_t first_key = switch_insns[keys_offset] | (switch_insns[keys_offset + 1] << 16);
      int32_t max_first_key =
          std::numeric_limits<int32_t>::max() - (static_cast<int32_t>(switch_count) - 1);
      if (UNLIKELY(first_key > max_first_key)) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid packed switch: first_key=" << first_key
                                          << ", switch_count=" << switch_count;
        return false;
      }
    } else {
      /* for a sparse switch, verify the keys are in ascending order */
      int32_t last_key = switch_insns[keys_offset] | (switch_insns[keys_offset + 1] << 16);
      for (uint32_t targ = 1; targ < switch_count; targ++) {
        int32_t key =
            static_cast<int32_t>(switch_insns[keys_offset + targ * 2]) |
            static_cast<int32_t>(switch_insns[keys_offset + targ * 2 + 1] << 16);
        if (UNLIKELY(key <= last_key)) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid sparse switch: last key=" << last_key
                                            << ", this=" << key;
          return false;
        }
        last_key = key;
      }
    }
  }
  /* verify each switch target */
  for (uint32_t targ = 0; targ < switch_count; targ++) {
    int32_t offset = static_cast<int32_t>(switch_insns[targets_offset + targ * 2]) |
                     static_cast<int32_t>(switch_insns[targets_offset + targ * 2 + 1] << 16);
    int32_t abs_offset = cur_offset + offset;
    if (UNLIKELY(abs_offset < 0 ||
                 abs_offset >= static_cast<int32_t>(insn_count) ||
                 !GetInstructionFlags(abs_offset).IsOpcode())) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch target " << offset
                                        << " (-> " << reinterpret_cast<void*>(abs_offset) << ") at "
                                        << reinterpret_cast<void*>(cur_offset)
                                        << "[" << targ << "]";
      return false;
    }
    GetModifiableInstructionFlags(abs_offset).SetBranchTarget();
  }
  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::VerifyCodeFlow() {
  const uint16_t registers_size = code_item_accessor_.RegistersSize();

  /* Create and initialize table holding register status */
  reg_table_.Init(insn_flags_.get(),
                  code_item_accessor_.InsnsSizeInCodeUnits(),
                  registers_size,
                  allocator_,
                  GetRegTypeCache(),
                  interesting_dex_pc_);

  work_line_.reset(RegisterLine::Create(registers_size, allocator_, GetRegTypeCache()));
  saved_line_.reset(RegisterLine::Create(registers_size, allocator_, GetRegTypeCache()));

  /* Initialize register types of method arguments. */
  if (!SetTypesFromSignature()) {
    DCHECK_NE(failures_.size(), 0U);
    std::string prepend("Bad signature in ");
    prepend += dex_file_->PrettyMethod(dex_method_idx_);
    PrependToLastFailMessage(prepend);
    return false;
  }
  // We may have a runtime failure here, clear.
  flags_.have_pending_runtime_throw_failure_ = false;

  /* Perform code flow verification. */
  bool res = LIKELY(monitor_enter_dex_pcs_ == nullptr)
                 ? CodeFlowVerifyMethod</*kMonitorDexPCs=*/ false>()
                 : CodeFlowVerifyMethod</*kMonitorDexPCs=*/ true>();
  if (UNLIKELY(!res)) {
    DCHECK_NE(failures_.size(), 0U);
    return false;
  }
  return true;
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::Dump(VariableIndentationOutputStream* vios) {
  if (!code_item_accessor_.HasCodeItem()) {
    vios->Stream() << "Native method\n";
    return;
  }
  {
    vios->Stream() << "Register Types:\n";
    ScopedIndentation indent1(vios);
    reg_types_.Dump(vios->Stream());
  }
  vios->Stream() << "Dumping instructions and register lines:\n";
  ScopedIndentation indent1(vios);

  for (const DexInstructionPcPair& inst : code_item_accessor_) {
    const size_t dex_pc = inst.DexPc();

    // Might be asked to dump before the table is initialized.
    if (reg_table_.IsInitialized()) {
      RegisterLine* reg_line = reg_table_.GetLine(dex_pc);
      if (reg_line != nullptr) {
        vios->Stream() << reg_line->Dump(this) << "\n";
      }
    }

    vios->Stream()
        << StringPrintf("0x%04zx", dex_pc) << ": " << GetInstructionFlags(dex_pc).ToString() << " ";
    const bool kDumpHexOfInstruction = false;
    if (kDumpHexOfInstruction) {
      vios->Stream() << inst->DumpHex(5) << " ";
    }
    vios->Stream() << inst->DumpString(dex_file_) << "\n";
  }
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::SetTypesFromSignature() {
  RegisterLine* reg_line = reg_table_.GetLine(0);

  // Should have been verified earlier.
  DCHECK_GE(code_item_accessor_.RegistersSize(), code_item_accessor_.InsSize());

  uint32_t arg_start = code_item_accessor_.RegistersSize() - code_item_accessor_.InsSize();
  size_t expected_args = code_item_accessor_.InsSize();   /* long/double count as two */

  // Include the "this" pointer.
  size_t cur_arg = 0;
  if (!IsStatic()) {
    if (expected_args == 0) {
      // Expect at least a receiver.
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected 0 args, but method is not static";
      return false;
    }

    // If this is a constructor for a class other than java.lang.Object, mark the first ("this")
    // argument as uninitialized. This restricts field access until the superclass constructor is
    // called.
    const RegType& declaring_class = GetDeclaringClass();
    if (IsConstructor()) {
      if (declaring_class.IsJavaLangObject()) {
        // "this" is implicitly initialized.
        reg_line->SetThisInitialized();
        reg_line->SetRegisterType<LockOp::kClear>(arg_start + cur_arg, declaring_class);
      } else {
        reg_line->SetRegisterType<LockOp::kClear>(
            arg_start + cur_arg,
            reg_types_.UninitializedThisArgument(declaring_class));
      }
    } else {
      reg_line->SetRegisterType<LockOp::kClear>(arg_start + cur_arg, declaring_class);
    }
    cur_arg++;
  }

  const dex::ProtoId& proto_id =
      dex_file_->GetMethodPrototype(dex_file_->GetMethodId(dex_method_idx_));
  DexFileParameterIterator iterator(*dex_file_, proto_id);

  for (; iterator.HasNext(); iterator.Next()) {
    const char* descriptor = iterator.GetDescriptor();
    if (descriptor == nullptr) {
      LOG(FATAL) << "Null descriptor";
    }
    if (cur_arg >= expected_args) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args
                                        << " args, found more (" << descriptor << ")";
      return false;
    }
    switch (descriptor[0]) {
      case 'L':
      case '[':
        // We assume that reference arguments are initialized. The only way it could be otherwise
        // (assuming the caller was verified) is if the current method is <init>, but in that case
        // it's effectively considered initialized the instant we reach here (in the sense that we
        // can return without doing anything or call virtual methods).
        {
          // Note: don't check access. No error would be thrown for declaring or passing an
          //       inaccessible class. Only actual accesses to fields or methods will.
          const RegType& reg_type = ResolveClass<CheckAccess::kNo>(iterator.GetTypeIdx());
          if (!reg_type.IsNonZeroReferenceTypes()) {
            DCHECK(HasFailures());
            return false;
          }
          reg_line->SetRegisterType<LockOp::kClear>(arg_start + cur_arg, reg_type);
        }
        break;
      case 'Z':
        reg_line->SetRegisterType(arg_start + cur_arg, RegType::Kind::kBoolean);
        break;
      case 'C':
        reg_line->SetRegisterType(arg_start + cur_arg, RegType::Kind::kChar);
        break;
      case 'B':
        reg_line->SetRegisterType(arg_start + cur_arg, RegType::Kind::kByte);
        break;
      case 'I':
        reg_line->SetRegisterType(arg_start + cur_arg, RegType::Kind::kInteger);
        break;
      case 'S':
        reg_line->SetRegisterType(arg_start + cur_arg, RegType::Kind::kShort);
        break;
      case 'F':
        reg_line->SetRegisterType(arg_start + cur_arg, RegType::Kind::kFloat);
        break;
      case 'J':
      case 'D': {
        if (cur_arg + 1 >= expected_args) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args
              << " args, found more (" << descriptor << ")";
          return false;
        }

        const RegType* lo_half;
        const RegType* hi_half;
        if (descriptor[0] == 'J') {
          lo_half = &reg_types_.LongLo();
          hi_half = &reg_types_.LongHi();
        } else {
          lo_half = &reg_types_.DoubleLo();
          hi_half = &reg_types_.DoubleHi();
        }
        reg_line->SetRegisterTypeWide(arg_start + cur_arg, *lo_half, *hi_half);
        cur_arg++;
        break;
      }
      default:
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected signature type char '"
                                          << descriptor << "'";
        return false;
    }
    cur_arg++;
  }
  if (cur_arg != expected_args) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args
                                      << " arguments, found " << cur_arg;
    return false;
  }
  // Dex file verifier ensures that all valid type indexes reference valid descriptors.
  DCHECK(IsValidDescriptor(dex_file_->GetReturnTypeDescriptor(proto_id)));
  return true;
}

COLD_ATTR
void HandleMonitorDexPcsWorkLine(
    std::vector<::art::verifier::MethodVerifier::DexLockInfo>* monitor_enter_dex_pcs,
    RegisterLine* work_line) {
  monitor_enter_dex_pcs->clear();  // The new work line is more accurate than the previous one.

  std::map<uint32_t, ::art::verifier::MethodVerifier::DexLockInfo> depth_to_lock_info;
  auto collector = [&](uint32_t dex_reg, uint32_t depth) {
    auto insert_pair = depth_to_lock_info.emplace(
        depth, ::art::verifier::MethodVerifier::DexLockInfo(depth));
    auto it = insert_pair.first;
    auto set_insert_pair = it->second.dex_registers.insert(dex_reg);
    DCHECK(set_insert_pair.second);
  };
  work_line->IterateRegToLockDepths(collector);
  for (auto& pair : depth_to_lock_info) {
    monitor_enter_dex_pcs->push_back(pair.second);
    // Map depth to dex PC.
    monitor_enter_dex_pcs->back().dex_pc = work_line->GetMonitorEnterDexPc(pair.second.dex_pc);
  }
}

template <bool kVerifierDebug>
template <bool kMonitorDexPCs>
bool MethodVerifier<kVerifierDebug>::CodeFlowVerifyMethod() {
  const uint16_t* insns = code_item_accessor_.Insns();
  const uint32_t insns_size = code_item_accessor_.InsnsSizeInCodeUnits();

  /* Begin by marking the first instruction as "changed". */
  GetModifiableInstructionFlags(0).SetChanged();
  uint32_t start_guess = 0;

  /* Continue until no instructions are marked "changed". */
  while (true) {
    if (allow_thread_suspension_) {
      self_->AllowThreadSuspension();
    }
    // Find the first marked one. Use "start_guess" as a way to find one quickly.
    uint32_t insn_idx = start_guess;
    for (; insn_idx < insns_size; insn_idx++) {
      if (GetInstructionFlags(insn_idx).IsChanged())
        break;
    }
    if (insn_idx == insns_size) {
      if (start_guess != 0) {
        /* try again, starting from the top */
        start_guess = 0;
        continue;
      } else {
        /* all flags are clear */
        break;
      }
    }
    // We carry the working set of registers from instruction to instruction. If this address can
    // be the target of a branch (or throw) instruction, or if we're skipping around chasing
    // "changed" flags, we need to load the set of registers from the table.
    // Because we always prefer to continue on to the next instruction, we should never have a
    // situation where we have a stray "changed" flag set on an instruction that isn't a branch
    // target.
    work_insn_idx_ = insn_idx;
    if (GetInstructionFlags(insn_idx).IsBranchTarget()) {
      work_line_->CopyFromLine(reg_table_.GetLine(insn_idx));
    } else if (kIsDebugBuild) {
      /*
       * Consistency check: retrieve the stored register line (assuming
       * a full table) and make sure it actually matches.
       */
      RegisterLine* register_line = reg_table_.GetLine(insn_idx);
      if (register_line != nullptr) {
        if (work_line_->CompareLine(register_line) != 0) {
          Dump(LOG_STREAM(FATAL_WITHOUT_ABORT));
          LOG(FATAL_WITHOUT_ABORT) << InfoMessages().str();
          LOG(FATAL) << "work_line diverged in " << dex_file_->PrettyMethod(dex_method_idx_)
                     << "@" << reinterpret_cast<void*>(work_insn_idx_) << "\n"
                     << " work_line=" << work_line_->Dump(this) << "\n"
                     << "  expected=" << register_line->Dump(this);
        }
      }
    }

    // If we're doing FindLocksAtDexPc, check whether we're at the dex pc we care about.
    // We want the state _before_ the instruction, for the case where the dex pc we're
    // interested in is itself a monitor-enter instruction (which is a likely place
    // for a thread to be suspended).
    if (kMonitorDexPCs && UNLIKELY(work_insn_idx_ == interesting_dex_pc_)) {
      HandleMonitorDexPcsWorkLine(monitor_enter_dex_pcs_, work_line_.get());
    }

    if (!CodeFlowVerifyInstruction(&start_guess)) {
      std::string prepend(dex_file_->PrettyMethod(dex_method_idx_));
      prepend += " failed to verify: ";
      PrependToLastFailMessage(prepend);
      return false;
    }
    /* Clear "changed" and mark as visited. */
    GetModifiableInstructionFlags(insn_idx).SetVisited();
    GetModifiableInstructionFlags(insn_idx).ClearChanged();
  }

  if (kVerifierDebug) {
    /*
     * Scan for dead code. There's nothing "evil" about dead code
     * (besides the wasted space), but it indicates a flaw somewhere
     * down the line, possibly in the verifier.
     *
     * If we've substituted "always throw" instructions into the stream,
     * we are almost certainly going to have some dead code.
     */
    int dead_start = -1;

    for (const DexInstructionPcPair& inst : code_item_accessor_) {
      const uint32_t insn_idx = inst.DexPc();
      /*
       * Switch-statement data doesn't get "visited" by scanner. It
       * may or may not be preceded by a padding NOP (for alignment).
       */
      if (insns[insn_idx] == Instruction::kPackedSwitchSignature ||
          insns[insn_idx] == Instruction::kSparseSwitchSignature ||
          insns[insn_idx] == Instruction::kArrayDataSignature ||
          (insns[insn_idx] == Instruction::NOP && (insn_idx + 1 < insns_size) &&
           (insns[insn_idx + 1] == Instruction::kPackedSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kSparseSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kArrayDataSignature))) {
        GetModifiableInstructionFlags(insn_idx).SetVisited();
      }

      if (!GetInstructionFlags(insn_idx).IsVisited()) {
        if (dead_start < 0) {
          dead_start = insn_idx;
        }
      } else if (dead_start >= 0) {
        LogVerifyInfo() << "dead code " << reinterpret_cast<void*>(dead_start)
                        << "-" << reinterpret_cast<void*>(insn_idx - 1);
        dead_start = -1;
      }
    }
    if (dead_start >= 0) {
      LogVerifyInfo()
          << "dead code " << reinterpret_cast<void*>(dead_start)
          << "-" << reinterpret_cast<void*>(code_item_accessor_.InsnsSizeInCodeUnits() - 1);
    }
    // To dump the state of the verify after a method, do something like:
    // if (dex_file_->PrettyMethod(dex_method_idx_) ==
    //     "boolean java.lang.String.equals(java.lang.Object)") {
    //   LOG(INFO) << InfoMessages().str();
    // }
  }
  return true;
}

// Setup a register line for the given return instruction.
template <bool kVerifierDebug>
static void AdjustReturnLine(MethodVerifier<kVerifierDebug>* verifier,
                             const Instruction* ret_inst,
                             RegisterLine* line) {
  Instruction::Code opcode = ret_inst->Opcode();

  switch (opcode) {
    case Instruction::RETURN_VOID:
      if (verifier->IsInstanceConstructor()) {
        // Before we mark all regs as conflicts, check that we don't have an uninitialized this.
        line->CheckConstructorReturn(verifier);
      }
      line->MarkAllRegistersAsConflicts(verifier);
      break;

    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
      line->MarkAllRegistersAsConflictsExcept(verifier, ret_inst->VRegA_11x());
      break;

    case Instruction::RETURN_WIDE:
      line->MarkAllRegistersAsConflictsExceptWide(verifier, ret_inst->VRegA_11x());
      break;

    default:
      LOG(FATAL) << "Unknown return opcode " << opcode;
      UNREACHABLE();
  }
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CodeFlowVerifyInstruction(uint32_t* start_guess) {
  /*
   * Once we finish decoding the instruction, we need to figure out where
   * we can go from here. There are three possible ways to transfer
   * control to another statement:
   *
   * (1) Continue to the next instruction. Applies to all but
   *     unconditional branches, method returns, and exception throws.
   * (2) Branch to one or more possible locations. Applies to branches
   *     and switch statements.
   * (3) Exception handlers. Applies to any instruction that can
   *     throw an exception that is handled by an encompassing "try"
   *     block.
   *
   * We can also return, in which case there is no successor instruction
   * from this point.
   *
   * The behavior can be determined from the opcode flags.
   */
  const uint16_t* insns = code_item_accessor_.Insns() + work_insn_idx_;
  const Instruction* inst = Instruction::At(insns);
  int opcode_flags = Instruction::FlagsOf(inst->Opcode());

  int32_t branch_target = 0;
  bool just_set_result = false;
  if (kVerifierDebug) {
    // Generate processing back trace to debug verifier
    LogVerifyInfo() << "Processing " << inst->DumpString(dex_file_) << std::endl
                    << work_line_->Dump(this);
  }

  /*
   * Make a copy of the previous register state. If the instruction
   * can throw an exception, we will copy/merge this into the "catch"
   * address rather than work_line, because we don't want the result
   * from the "successful" code path (e.g. a check-cast that "improves"
   * a type) to be visible to the exception handler.
   */
  if (((opcode_flags & Instruction::kThrow) != 0 || IsCompatThrow(inst->Opcode())) &&
      CurrentInsnFlags()->IsInTry()) {
    saved_line_->CopyFromLine(work_line_.get());
  } else if (kIsDebugBuild) {
    saved_line_->FillWithGarbage();
  }
  // Per-instruction flag, should not be set here.
  DCHECK(!flags_.have_pending_runtime_throw_failure_);
  bool exc_handler_unreachable = false;


  // We need to ensure the work line is consistent while performing validation. When we spot a
  // peephole pattern we compute a new line for either the fallthrough instruction or the
  // branch target.
  RegisterLineArenaUniquePtr branch_line;
  RegisterLineArenaUniquePtr fallthrough_line;

  using enum RegType::Kind;
  switch (inst->Opcode()) {
    case Instruction::NOP:
      /*
       * A "pure" NOP has no effect on anything. Data tables start with
       * a signature that looks like a NOP; if we see one of these in
       * the course of executing code then we have a problem.
       */
      if (inst->VRegA_10x() != 0) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "encountered data table in instruction stream";
      }
      break;

    case Instruction::MOVE:
      work_line_->CopyRegister1(this, inst->VRegA_12x(), inst->VRegB_12x(), kTypeCategory1nr);
      break;
    case Instruction::MOVE_FROM16:
      work_line_->CopyRegister1(this, inst->VRegA_22x(), inst->VRegB_22x(), kTypeCategory1nr);
      break;
    case Instruction::MOVE_16:
      work_line_->CopyRegister1(this, inst->VRegA_32x(), inst->VRegB_32x(), kTypeCategory1nr);
      break;
    case Instruction::MOVE_WIDE:
      work_line_->CopyRegister2(this, inst->VRegA_12x(), inst->VRegB_12x());
      break;
    case Instruction::MOVE_WIDE_FROM16:
      work_line_->CopyRegister2(this, inst->VRegA_22x(), inst->VRegB_22x());
      break;
    case Instruction::MOVE_WIDE_16:
      work_line_->CopyRegister2(this, inst->VRegA_32x(), inst->VRegB_32x());
      break;
    case Instruction::MOVE_OBJECT:
      work_line_->CopyRegister1(this, inst->VRegA_12x(), inst->VRegB_12x(), kTypeCategoryRef);
      break;
    case Instruction::MOVE_OBJECT_FROM16:
      work_line_->CopyRegister1(this, inst->VRegA_22x(), inst->VRegB_22x(), kTypeCategoryRef);
      break;
    case Instruction::MOVE_OBJECT_16:
      work_line_->CopyRegister1(this, inst->VRegA_32x(), inst->VRegB_32x(), kTypeCategoryRef);
      break;

    /*
     * The move-result instructions copy data out of a "pseudo-register"
     * with the results from the last method invocation. In practice we
     * might want to hold the result in an actual CPU register, so the
     * Dalvik spec requires that these only appear immediately after an
     * invoke or filled-new-array.
     *
     * These calls invalidate the "result" register. (This is now
     * redundant with the reset done below, but it can make the debug info
     * easier to read in some cases.)
     */
    case Instruction::MOVE_RESULT:
      work_line_->CopyResultRegister1(this, inst->VRegA_11x(), false);
      break;
    case Instruction::MOVE_RESULT_WIDE:
      work_line_->CopyResultRegister2(this, inst->VRegA_11x());
      break;
    case Instruction::MOVE_RESULT_OBJECT:
      work_line_->CopyResultRegister1(this, inst->VRegA_11x(), true);
      break;

    case Instruction::MOVE_EXCEPTION:
      if (!HandleMoveException(inst)) {
        exc_handler_unreachable = true;
      }
      break;

    case Instruction::RETURN_VOID:
      if (!IsInstanceConstructor() || work_line_->CheckConstructorReturn(this)) {
        if (!GetMethodReturnType().IsConflict()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-void not expected";
        }
      }
      break;
    case Instruction::RETURN:
      if (!IsInstanceConstructor() || work_line_->CheckConstructorReturn(this)) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory1Types()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected non-category 1 return type "
                                            << return_type;
        } else {
          // Compilers may generate synthetic functions that write byte values into boolean fields.
          // Also, it may use integer values for boolean, byte, short, and character return types.
          const uint32_t vregA = inst->VRegA_11x();
          const RegType& src_type = work_line_->GetRegisterType(this, vregA);
          bool use_src = ((return_type.IsBoolean() && src_type.IsByte()) ||
                          ((return_type.IsBoolean() || return_type.IsByte() ||
                           return_type.IsShort() || return_type.IsChar()) &&
                           src_type.IsInteger()));
          /* check the register contents */
          bool success = VerifyRegisterType(vregA, use_src ? src_type : return_type);
          if (!success) {
            AppendToLastFailMessage(StringPrintf(" return-1nr on invalid register v%d", vregA));
          }
        }
      }
      break;
    case Instruction::RETURN_WIDE:
      if (!IsInstanceConstructor() || work_line_->CheckConstructorReturn(this)) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory2Types()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-wide not expected";
        } else {
          /* check the register contents */
          const uint32_t vregA = inst->VRegA_11x();
          bool success = VerifyRegisterTypeWide(vregA, return_type.GetKind());
          if (!success) {
            AppendToLastFailMessage(StringPrintf(" return-wide on invalid register v%d", vregA));
          }
        }
      }
      break;
    case Instruction::RETURN_OBJECT:
      if (!IsInstanceConstructor() || work_line_->CheckConstructorReturn(this)) {
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-object not expected";
        } else {
          /* return_type is the *expected* return type, not register value */
          DCHECK(!return_type.IsZeroOrNull());
          DCHECK(!return_type.IsUninitializedReference());
          const uint32_t vregA = inst->VRegA_11x();
          const RegType& reg_type = work_line_->GetRegisterType(this, vregA);
          // Disallow returning undefined, conflict & uninitialized values and verify that the
          // reference in vAA is an instance of the "return_type."
          if (reg_type.IsUndefined()) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "returning undefined register";
          } else if (reg_type.IsConflict()) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "returning register with conflict";
          } else if (reg_type.IsUninitializedTypes()) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "returning uninitialized object '"
                                              << reg_type << "'";
          } else if (!reg_type.IsReferenceTypes()) {
            // We really do expect a reference here.
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-object returns a non-reference type "
                                              << reg_type;
          } else if (!IsAssignableFrom(return_type, reg_type)) {
            if (reg_type.IsUnresolvedTypes() || return_type.IsUnresolvedTypes()) {
              Fail(VERIFY_ERROR_UNRESOLVED_TYPE_CHECK)
                  << " can't resolve returned type '" << return_type << "' or '" << reg_type << "'";
            } else {
              Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "returning '" << reg_type
                  << "', but expected from declaration '" << return_type << "'";
            }
          }
        }
      }
      break;

      /* could be boolean, int, float, or a null reference */
    case Instruction::CONST_4: {
      int32_t val = static_cast<int32_t>(inst->VRegB_11n() << 28) >> 28;
      work_line_->SetRegisterType(inst->VRegA_11n(), DetermineCat1Constant(val));
      break;
    }
    case Instruction::CONST_16: {
      int16_t val = static_cast<int16_t>(inst->VRegB_21s());
      work_line_->SetRegisterType(inst->VRegA_21s(), DetermineCat1Constant(val));
      break;
    }
    case Instruction::CONST: {
      int32_t val = inst->VRegB_31i();
      work_line_->SetRegisterType(inst->VRegA_31i(), DetermineCat1Constant(val));
      break;
    }
    case Instruction::CONST_HIGH16: {
      int32_t val = static_cast<int32_t>(inst->VRegB_21h() << 16);
      work_line_->SetRegisterType(inst->VRegA_21h(), DetermineCat1Constant(val));
      break;
    }
      /* could be long or double; resolved upon use */
    case Instruction::CONST_WIDE_16: {
      int64_t val = static_cast<int16_t>(inst->VRegB_21s());
      const RegType& lo = reg_types_.ConstantLo();
      const RegType& hi = reg_types_.ConstantHi();
      work_line_->SetRegisterTypeWide(inst->VRegA_21s(), lo, hi);
      break;
    }
    case Instruction::CONST_WIDE_32: {
      int64_t val = static_cast<int32_t>(inst->VRegB_31i());
      const RegType& lo = reg_types_.ConstantLo();
      const RegType& hi = reg_types_.ConstantHi();
      work_line_->SetRegisterTypeWide(inst->VRegA_31i(), lo, hi);
      break;
    }
    case Instruction::CONST_WIDE: {
      int64_t val = inst->VRegB_51l();
      const RegType& lo = reg_types_.ConstantLo();
      const RegType& hi = reg_types_.ConstantHi();
      work_line_->SetRegisterTypeWide(inst->VRegA_51l(), lo, hi);
      break;
    }
    case Instruction::CONST_WIDE_HIGH16: {
      int64_t val = static_cast<uint64_t>(inst->VRegB_21h()) << 48;
      const RegType& lo = reg_types_.ConstantLo();
      const RegType& hi = reg_types_.ConstantHi();
      work_line_->SetRegisterTypeWide(inst->VRegA_21h(), lo, hi);
      break;
    }
    case Instruction::CONST_STRING:
      work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_21c(), reg_types_.JavaLangString());
      break;
    case Instruction::CONST_STRING_JUMBO:
      work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_31c(), reg_types_.JavaLangString());
      break;
    case Instruction::CONST_CLASS: {
      // Get type from instruction if unresolved then we need an access check
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      const RegType& res_type = ResolveClass<CheckAccess::kYes>(dex::TypeIndex(inst->VRegB_21c()));
      // Register holds class, ie its type is class, on error it will hold Conflict.
      work_line_->SetRegisterType<LockOp::kClear>(
          inst->VRegA_21c(),
          res_type.IsConflict() ? res_type : reg_types_.JavaLangClass());
      break;
    }
    case Instruction::CONST_METHOD_HANDLE:
      work_line_->SetRegisterType<LockOp::kClear>(
          inst->VRegA_21c(), reg_types_.JavaLangInvokeMethodHandle());
      break;
    case Instruction::CONST_METHOD_TYPE:
      work_line_->SetRegisterType<LockOp::kClear>(
          inst->VRegA_21c(), reg_types_.JavaLangInvokeMethodType());
      break;
    case Instruction::MONITOR_ENTER:
      work_line_->PushMonitor(this, inst->VRegA_11x(), work_insn_idx_);
      // Check whether the previous instruction is a move-object with vAA as a source, creating
      // untracked lock aliasing.
      if (0 != work_insn_idx_ && !GetInstructionFlags(work_insn_idx_).IsBranchTarget()) {
        uint32_t prev_idx = work_insn_idx_ - 1;
        while (0 != prev_idx && !GetInstructionFlags(prev_idx).IsOpcode()) {
          prev_idx--;
        }
        const Instruction& prev_inst = code_item_accessor_.InstructionAt(prev_idx);
        switch (prev_inst.Opcode()) {
          case Instruction::MOVE_OBJECT:
          case Instruction::MOVE_OBJECT_16:
          case Instruction::MOVE_OBJECT_FROM16:
            if (prev_inst.VRegB() == inst->VRegA_11x()) {
              // Redo the copy. This won't change the register types, but update the lock status
              // for the aliased register.
              work_line_->CopyRegister1(this,
                                        prev_inst.VRegA(),
                                        prev_inst.VRegB(),
                                        kTypeCategoryRef);
            }
            break;

          // Catch a case of register aliasing when two registers are linked to the same
          // java.lang.Class object via two consequent const-class instructions immediately
          // preceding monitor-enter called on one of those registers.
          case Instruction::CONST_CLASS: {
            // Get the second previous instruction.
            if (prev_idx == 0 || GetInstructionFlags(prev_idx).IsBranchTarget()) {
              break;
            }
            prev_idx--;
            while (0 != prev_idx && !GetInstructionFlags(prev_idx).IsOpcode()) {
              prev_idx--;
            }
            const Instruction& prev2_inst = code_item_accessor_.InstructionAt(prev_idx);

            // Match the pattern "const-class; const-class; monitor-enter;"
            if (prev2_inst.Opcode() != Instruction::CONST_CLASS) {
              break;
            }

            // Ensure both const-classes are called for the same type_idx.
            if (prev_inst.VRegB_21c() != prev2_inst.VRegB_21c()) {
              break;
            }

            // Update the lock status for the aliased register.
            if (prev_inst.VRegA() == inst->VRegA_11x()) {
              work_line_->CopyRegister1(this,
                                        prev2_inst.VRegA(),
                                        inst->VRegA_11x(),
                                        kTypeCategoryRef);
            } else if (prev2_inst.VRegA() == inst->VRegA_11x()) {
              work_line_->CopyRegister1(this,
                                        prev_inst.VRegA(),
                                        inst->VRegA_11x(),
                                        kTypeCategoryRef);
            }
            break;
          }

          default:  // Other instruction types ignored.
            break;
        }
      }
      break;
    case Instruction::MONITOR_EXIT:
      /*
       * monitor-exit instructions are odd. They can throw exceptions,
       * but when they do they act as if they succeeded and the PC is
       * pointing to the following instruction. (This behavior goes back
       * to the need to handle asynchronous exceptions, a now-deprecated
       * feature that Dalvik doesn't support.)
       *
       * In practice we don't need to worry about this. The only
       * exceptions that can be thrown from monitor-exit are for a
       * null reference and -exit without a matching -enter. If the
       * structured locking checks are working, the former would have
       * failed on the -enter instruction, and the latter is impossible.
       *
       * This is fortunate, because issue 3221411 prevents us from
       * chasing the "can throw" path when monitor verification is
       * enabled. If we can fully verify the locking we can ignore
       * some catch blocks (which will show up as "dead" code when
       * we skip them here); if we can't, then the code path could be
       * "live" so we still need to check it.
       */
      opcode_flags &= ~Instruction::kThrow;
      work_line_->PopMonitor(this, inst->VRegA_11x());
      break;
    case Instruction::CHECK_CAST:
    case Instruction::INSTANCE_OF: {
      /*
       * If this instruction succeeds, we will "downcast" register vA to the type in vB. (This
       * could be a "upcast" -- not expected, so we don't try to address it.)
       *
       * If it fails, an exception is thrown, which we deal with later by ignoring the update to
       * dec_insn.vA when branching to a handler.
       */
      const bool is_checkcast = (inst->Opcode() == Instruction::CHECK_CAST);
      const dex::TypeIndex type_idx((is_checkcast) ? inst->VRegB_21c() : inst->VRegC_22c());
      const RegType& res_type = ResolveClass<CheckAccess::kYes>(type_idx);
      if (res_type.IsConflict()) {
        // If this is a primitive type, fail HARD.
        ObjPtr<mirror::Class> klass = GetClassLinker()->LookupResolvedType(
            type_idx, dex_cache_.Get(), class_loader_.Get());
        if (klass != nullptr && klass->IsPrimitive()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "using primitive type "
              << dex_file_->GetTypeDescriptorView(type_idx) << " in instanceof in "
              << GetDeclaringClass();
          break;
        }

        DCHECK_NE(failures_.size(), 0U);
        if (!is_checkcast) {
          work_line_->SetRegisterType(inst->VRegA_22c(), kBoolean);
        }
        break;  // bad class
      }
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      uint32_t orig_type_reg = (is_checkcast) ? inst->VRegA_21c() : inst->VRegB_22c();
      const RegType& orig_type = work_line_->GetRegisterType(this, orig_type_reg);
      if (!res_type.IsNonZeroReferenceTypes()) {
        if (is_checkcast) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on unexpected class " << res_type;
        } else {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "instance-of on unexpected class " << res_type;
        }
      } else if (!orig_type.IsReferenceTypes()) {
        if (is_checkcast) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on non-reference in v" << orig_type_reg;
        } else {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "instance-of on non-reference in v" << orig_type_reg;
        }
      } else if (orig_type.IsUninitializedTypes()) {
        if (is_checkcast) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on uninitialized reference in v"
                                            << orig_type_reg;
        } else {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "instance-of on uninitialized reference in v"
                                            << orig_type_reg;
        }
      } else {
        if (is_checkcast) {
          work_line_->SetRegisterType<LockOp::kKeep>(inst->VRegA_21c(), res_type);
        } else {
          work_line_->SetRegisterType(inst->VRegA_22c(), kBoolean);
        }
      }
      break;
    }
    case Instruction::ARRAY_LENGTH: {
      const RegType& res_type = work_line_->GetRegisterType(this, inst->VRegB_12x());
      if (res_type.IsReferenceTypes()) {
        if (!res_type.IsArrayTypes() && !res_type.IsZeroOrNull()) {
          // ie not an array or null
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-length on non-array " << res_type;
        } else {
          work_line_->SetRegisterType(inst->VRegA_12x(), kInteger);
        }
      } else {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-length on non-array " << res_type;
      }
      break;
    }
    case Instruction::NEW_INSTANCE: {
      const RegType& res_type = ResolveClass<CheckAccess::kYes>(dex::TypeIndex(inst->VRegB_21c()));
      // Dex file verifier ensures that all valid type indexes reference valid descriptors and the
      // `CheckNewInstance()` ensures that the descriptor starts with an `L` before we get to the
      // code flow verification. So, we should not see a conflict (void) or a primitive type here.
      DCHECK(res_type.IsJavaLangObject() ||
             res_type.IsReference() ||
             res_type.IsUnresolvedReference()) << res_type;
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      // can't create an instance of an interface or abstract class */
      if (!res_type.IsInstantiableTypes()) {
        Fail(VERIFY_ERROR_INSTANTIATION)
            << "new-instance on primitive, interface or abstract class" << res_type;
        // Soft failure so carry on to set register type.
      }
      const RegType& uninit_type = reg_types_.Uninitialized(res_type);
      // Add the new uninitialized reference to the register state and record the allocation dex pc.
      uint32_t vA = inst->VRegA_21c();
      work_line_->DCheckUniqueNewInstanceDexPc(this, work_insn_idx_);
      work_line_->SetRegisterTypeForNewInstance(vA, uninit_type, work_insn_idx_);
      break;
    }
    case Instruction::NEW_ARRAY:
      VerifyNewArray(inst, false, false);
      break;
    case Instruction::FILLED_NEW_ARRAY:
      VerifyNewArray(inst, true, false);
      just_set_result = true;  // Filled new array sets result register
      break;
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      VerifyNewArray(inst, true, true);
      just_set_result = true;  // Filled new array range sets result register
      break;
    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
      CheckBinaryOp(inst, kInteger, kFloat, kFloat, /*check_boolean_op=*/ false);
      break;
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      CheckBinaryOpWideCmp(inst, kInteger, kDoubleLo, kDoubleLo);
      break;
    case Instruction::CMP_LONG:
      CheckBinaryOpWideCmp(inst, kInteger, kLongLo, kLongLo);
      break;
    case Instruction::THROW: {
      const RegType& res_type = work_line_->GetRegisterType(this, inst->VRegA_11x());
      if (!IsAssignableFrom(reg_types_.JavaLangThrowable(), res_type)) {
        if (res_type.IsUninitializedTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "thrown exception not initialized";
        } else if (!res_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "thrown value of non-reference type " << res_type;
        } else {
          Fail(res_type.IsUnresolvedTypes()
                  ? VERIFY_ERROR_UNRESOLVED_TYPE_CHECK : VERIFY_ERROR_BAD_CLASS_HARD)
                << "thrown class " << res_type << " not instanceof Throwable";
        }
      }
      break;
    }
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      /* no effect on or use of registers */
      break;

    case Instruction::PACKED_SWITCH:
    case Instruction::SPARSE_SWITCH:
      /* verify that vAA is an integer, or can be converted to one */
      VerifyRegisterType(inst->VRegA_31t(), kInteger);
      break;

    case Instruction::FILL_ARRAY_DATA: {
      /* Similar to the verification done for APUT */
      const RegType& array_type = work_line_->GetRegisterType(this, inst->VRegA_31t());
      /* array_type can be null if the reg type is Zero */
      if (!array_type.IsZeroOrNull()) {
        if (!array_type.IsArrayTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid fill-array-data with array type "
                                            << array_type;
        } else if (array_type.IsUnresolvedTypes()) {
          // If it's an unresolved array type, it must be non-primitive.
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid fill-array-data for array of type "
                                            << array_type;
        } else {
          const RegType& component_type = reg_types_.GetComponentType(array_type);
          DCHECK(!component_type.IsConflict());
          if (component_type.IsNonZeroReferenceTypes()) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid fill-array-data with component type "
                                              << component_type;
          } else {
            // Now verify if the element width in the table matches the element width declared in
            // the array
            const uint16_t* array_data =
                insns + (insns[1] | (static_cast<int32_t>(insns[2]) << 16));
            if (array_data[0] != Instruction::kArrayDataSignature) {
              Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid magic for array-data";
            } else {
              size_t elem_width = Primitive::ComponentSize(component_type.GetPrimitiveType());
              // Since we don't compress the data in Dex, expect to see equal width of data stored
              // in the table and expected from the array class.
              if (array_data[1] != elem_width) {
                Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-data size mismatch (" << array_data[1]
                                                  << " vs " << elem_width << ")";
              }
            }
          }
        }
      }
      break;
    }
    case Instruction::IF_EQ:
    case Instruction::IF_NE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(this, inst->VRegA_22t());
      const RegType& reg_type2 = work_line_->GetRegisterType(this, inst->VRegB_22t());
      bool mismatch = false;
      if (reg_type1.IsZeroOrNull()) {  // zero then integral or reference expected
        mismatch = !reg_type2.IsReferenceTypes() && !reg_type2.IsIntegralTypes();
      } else if (reg_type1.IsReferenceTypes()) {  // both references?
        mismatch = !reg_type2.IsReferenceTypes();
      } else {  // both integral?
        mismatch = !reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes();
      }
      if (mismatch) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "args to if-eq/if-ne (" << reg_type1 << ","
                                          << reg_type2 << ") must both be references or integral";
      }
      break;
    }
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(this, inst->VRegA_22t());
      const RegType& reg_type2 = work_line_->GetRegisterType(this, inst->VRegB_22t());
      if (!reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "args to 'if' (" << reg_type1 << ","
                                          << reg_type2 << ") must be integral";
      }
      break;
    }
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(this, inst->VRegA_21t());
      if (!reg_type.IsReferenceTypes() && !reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "type " << reg_type
                                          << " unexpected as arg to if-eqz/if-nez";
      }

      // Find previous instruction - its existence is a precondition to peephole optimization.
      if (UNLIKELY(0 == work_insn_idx_)) {
        break;
      }
      uint32_t instance_of_idx = work_insn_idx_ - 1;
      while (0 != instance_of_idx && !GetInstructionFlags(instance_of_idx).IsOpcode()) {
        instance_of_idx--;
      }
      // Dex index 0 must be an opcode.
      DCHECK(GetInstructionFlags(instance_of_idx).IsOpcode());

      const Instruction& instance_of_inst = code_item_accessor_.InstructionAt(instance_of_idx);

      /* Check for peep-hole pattern of:
       *    ...;
       *    instance-of vX, vY, T;
       *    ifXXX vX, label ;
       *    ...;
       * label:
       *    ...;
       * and sharpen the type of vY to be type T.
       * Note, this pattern can't be if:
       *  - if there are other branches to this branch,
       *  - when vX == vY.
       */
      if (!CurrentInsnFlags()->IsBranchTarget() &&
          (Instruction::INSTANCE_OF == instance_of_inst.Opcode()) &&
          (inst->VRegA_21t() == instance_of_inst.VRegA_22c()) &&
          (instance_of_inst.VRegA_22c() != instance_of_inst.VRegB_22c())) {
        // Check the type of the instance-of is different than that of registers type, as if they
        // are the same there is no work to be done here. Check that the conversion is not to or
        // from an unresolved type as type information is imprecise. If the instance-of is to an
        // interface then ignore the type information as interfaces can only be treated as Objects
        // and we don't want to disallow field and other operations on the object. If the value
        // being instance-of checked against is known null (zero) then allow the optimization as
        // we didn't have type information. If the merge of the instance-of type with the original
        // type is assignable to the original then allow optimization. This check is performed to
        // ensure that subsequent merges don't lose type information - such as becoming an
        // interface from a class that would lose information relevant to field checks.
        //
        // Note: do not do an access check. This may mark this with a runtime throw that actually
        //       happens at the instanceof, not the branch (and branches aren't flagged to throw).
        const RegType& orig_type = work_line_->GetRegisterType(this, instance_of_inst.VRegB_22c());
        const RegType& cast_type = ResolveClass<CheckAccess::kNo>(
            dex::TypeIndex(instance_of_inst.VRegC_22c()));

        if (!orig_type.Equals(cast_type) &&
            !cast_type.IsUnresolvedTypes() && !orig_type.IsUnresolvedTypes() &&
            cast_type.HasClass() &&             // Could be conflict type, make sure it has a class.
            !cast_type.GetClass()->IsInterface() &&
            !orig_type.IsZeroOrNull() &&
            IsStrictlyAssignableFrom(orig_type, cast_type.Merge(orig_type, &reg_types_, this))) {
          RegisterLine* update_line = RegisterLine::Create(code_item_accessor_.RegistersSize(),
                                                           allocator_,
                                                           GetRegTypeCache());
          if (inst->Opcode() == Instruction::IF_EQZ) {
            fallthrough_line.reset(update_line);
          } else {
            branch_line.reset(update_line);
          }
          update_line->CopyFromLine(work_line_.get());
          update_line->SetRegisterType<LockOp::kKeep>(instance_of_inst.VRegB_22c(), cast_type);
          if (!GetInstructionFlags(instance_of_idx).IsBranchTarget() && 0 != instance_of_idx) {
            // See if instance-of was preceded by a move-object operation, common due to the small
            // register encoding space of instance-of, and propagate type information to the source
            // of the move-object.
            // Note: this is only valid if the move source was not clobbered.
            uint32_t move_idx = instance_of_idx - 1;
            while (0 != move_idx && !GetInstructionFlags(move_idx).IsOpcode()) {
              move_idx--;
            }
            DCHECK(GetInstructionFlags(move_idx).IsOpcode());
            auto maybe_update_fn = [&instance_of_inst, update_line, &cast_type](
                uint16_t move_src,
                uint16_t move_trg)
                REQUIRES_SHARED(Locks::mutator_lock_) {
              if (move_trg == instance_of_inst.VRegB_22c() &&
                  move_src != instance_of_inst.VRegA_22c()) {
                update_line->SetRegisterType<LockOp::kKeep>(move_src, cast_type);
              }
            };
            const Instruction& move_inst = code_item_accessor_.InstructionAt(move_idx);
            switch (move_inst.Opcode()) {
              case Instruction::MOVE_OBJECT:
                maybe_update_fn(move_inst.VRegB_12x(), move_inst.VRegA_12x());
                break;
              case Instruction::MOVE_OBJECT_FROM16:
                maybe_update_fn(move_inst.VRegB_22x(), move_inst.VRegA_22x());
                break;
              case Instruction::MOVE_OBJECT_16:
                maybe_update_fn(move_inst.VRegB_32x(), move_inst.VRegA_32x());
                break;
              default:
                break;
            }
          }
        }
      }

      break;
    }
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(this, inst->VRegA_21t());
      if (!reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "type " << reg_type
                                          << " unexpected as arg to if-ltz/if-gez/if-gtz/if-lez";
      }
      break;
    }
    case Instruction::AGET_BOOLEAN:
      VerifyAGet(inst, reg_types_.Boolean(), true);
      break;
    case Instruction::AGET_BYTE:
      VerifyAGet(inst, reg_types_.Byte(), true);
      break;
    case Instruction::AGET_CHAR:
      VerifyAGet(inst, reg_types_.Char(), true);
      break;
    case Instruction::AGET_SHORT:
      VerifyAGet(inst, reg_types_.Short(), true);
      break;
    case Instruction::AGET:
      VerifyAGet(inst, reg_types_.Integer(), true);
      break;
    case Instruction::AGET_WIDE:
      VerifyAGet(inst, reg_types_.LongLo(), true);
      break;
    case Instruction::AGET_OBJECT:
      VerifyAGet(inst, reg_types_.JavaLangObject(), false);
      break;

    case Instruction::APUT_BOOLEAN:
      VerifyAPut(inst, reg_types_.Boolean(), true);
      break;
    case Instruction::APUT_BYTE:
      VerifyAPut(inst, reg_types_.Byte(), true);
      break;
    case Instruction::APUT_CHAR:
      VerifyAPut(inst, reg_types_.Char(), true);
      break;
    case Instruction::APUT_SHORT:
      VerifyAPut(inst, reg_types_.Short(), true);
      break;
    case Instruction::APUT:
      VerifyAPut(inst, reg_types_.Integer(), true);
      break;
    case Instruction::APUT_WIDE:
      VerifyAPut(inst, reg_types_.LongLo(), true);
      break;
    case Instruction::APUT_OBJECT:
      VerifyAPut(inst, reg_types_.JavaLangObject(), false);
      break;

    case Instruction::IGET_BOOLEAN:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, false);
      break;
    case Instruction::IGET_BYTE:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, false);
      break;
    case Instruction::IGET_CHAR:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, false);
      break;
    case Instruction::IGET_SHORT:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, false);
      break;
    case Instruction::IGET:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, false);
      break;
    case Instruction::IGET_WIDE:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, false);
      break;
    case Instruction::IGET_OBJECT:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, false);
      break;
    case Instruction::IPUT_BYTE:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, false);
      break;
    case Instruction::IPUT_CHAR:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, false);
      break;
    case Instruction::IPUT_SHORT:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, false);
      break;
    case Instruction::IPUT:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, false);
      break;
    case Instruction::IPUT_WIDE:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, false);
      break;
    case Instruction::IPUT_OBJECT:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, false, false);
      break;

    case Instruction::SGET_BOOLEAN:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, true);
      break;
    case Instruction::SGET_BYTE:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, true);
      break;
    case Instruction::SGET_CHAR:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, true);
      break;
    case Instruction::SGET_SHORT:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, true);
      break;
    case Instruction::SGET:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, true);
      break;
    case Instruction::SGET_WIDE:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, true, true);
      break;
    case Instruction::SGET_OBJECT:
      VerifyISFieldAccess<FieldAccessType::kAccGet>(inst, false, true);
      break;

    case Instruction::SPUT_BOOLEAN:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, true);
      break;
    case Instruction::SPUT_BYTE:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, true);
      break;
    case Instruction::SPUT_CHAR:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, true);
      break;
    case Instruction::SPUT_SHORT:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, true);
      break;
    case Instruction::SPUT:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, true);
      break;
    case Instruction::SPUT_WIDE:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, true, true);
      break;
    case Instruction::SPUT_OBJECT:
      VerifyISFieldAccess<FieldAccessType::kAccPut>(inst, false, true);
      break;

    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_VIRTUAL_RANGE ||
                       inst->Opcode() == Instruction::INVOKE_SUPER_RANGE);
      bool is_super = (inst->Opcode() == Instruction::INVOKE_SUPER ||
                       inst->Opcode() == Instruction::INVOKE_SUPER_RANGE);
      MethodType type = is_super ? METHOD_SUPER : METHOD_VIRTUAL;
      ArtMethod* called_method = VerifyInvocationArgs(inst, type, is_range);
      uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
      const dex::MethodId& method_id = dex_file_->GetMethodId(method_idx);
      dex::TypeIndex return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
      DCHECK_IMPLIES(called_method != nullptr,
                     called_method->GetReturnTypeDescriptorView() ==
                         dex_file_->GetTypeDescriptorView(return_type_idx));
      const RegType& return_type = reg_types_.FromTypeIndex(return_type_idx);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_DIRECT_RANGE);
      ArtMethod* called_method = VerifyInvocationArgs(inst, METHOD_DIRECT, is_range);
      uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
      const dex::MethodId& method_id = dex_file_->GetMethodId(method_idx);
      dex::TypeIndex return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
      DCHECK_IMPLIES(called_method != nullptr,
                     called_method->GetReturnTypeDescriptorView() ==
                         dex_file_->GetTypeDescriptorView(return_type_idx));
      bool is_constructor = (called_method != nullptr)
          ? called_method->IsConstructor()
          : dex_file_->GetStringView(method_id.name_idx_) == "<init>";
      if (is_constructor) {
        /*
         * Some additional checks when calling a constructor. We know from the invocation arg check
         * that the "this" argument is an instance of called_method->klass. Now we further restrict
         * that to require that called_method->klass is the same as this->klass or this->super,
         * allowing the latter only if the "this" argument is the same as the "this" argument to
         * this method (which implies that we're in a constructor ourselves).
         */
        const RegType& this_type = GetInvocationThis(inst);
        if (this_type.IsConflict())  // failure.
          break;

        /* no null refs allowed (?) */
        if (this_type.IsZeroOrNull()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unable to initialize null ref";
          break;
        }

        /* arg must be an uninitialized reference */
        if (!this_type.IsUninitializedTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Expected initialization on uninitialized reference "
              << this_type;
          break;
        }

        // Note: According to JLS, constructors are never inherited. Therefore the target
        // constructor should be defined exactly by the `this_type`, or by the direct
        // superclass in the case of a constructor calling the superclass constructor.
        // However, ART had this check commented out for a very long time and this has
        // allowed bytecode optimizers such as R8 to inline constructors, often calling
        // `j.l.Object.<init>` directly without any intermediate constructor. Since this
        // optimization allows eliminating constructor methods, this often results in a
        // significant dex size reduction. Therefore it is undesirable to reinstate this
        // check and ART deliberately remains permissive here and diverges from the RI.

        /*
         * Replace the uninitialized reference with an initialized one. We need to do this for all
         * registers that have the same object instance in them, not just the "this" register.
         */
        work_line_->MarkRefsAsInitialized(this, inst->VRegC());
      }
      const RegType& return_type = reg_types_.FromTypeIndex(return_type_idx);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_STATIC_RANGE);
      ArtMethod* called_method = VerifyInvocationArgs(inst, METHOD_STATIC, is_range);
      uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
      const dex::MethodId& method_id = dex_file_->GetMethodId(method_idx);
      dex::TypeIndex return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
      DCHECK_IMPLIES(called_method != nullptr,
                     called_method->GetReturnTypeDescriptorView() ==
                         dex_file_->GetTypeDescriptorView(return_type_idx));
      const RegType& return_type = reg_types_.FromTypeIndex(return_type_idx);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE: {
      bool is_range =  (inst->Opcode() == Instruction::INVOKE_INTERFACE_RANGE);
      ArtMethod* abs_method = VerifyInvocationArgs(inst, METHOD_INTERFACE, is_range);
      if (abs_method != nullptr) {
        ObjPtr<mirror::Class> called_interface = abs_method->GetDeclaringClass();
        if (!called_interface->IsInterface() && !called_interface->IsObjectClass()) {
          Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected interface class in invoke-interface '"
              << abs_method->PrettyMethod() << "'";
          break;
        }
      }
      /* Get the type of the "this" arg, which should either be a sub-interface of called
       * interface or Object (see comments in RegType::JoinClass).
       */
      const RegType& this_type = GetInvocationThis(inst);
      if (this_type.IsZeroOrNull()) {
        /* null pointer always passes (and always fails at runtime) */
      } else {
        if (this_type.IsUninitializedTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "interface call on uninitialized object "
              << this_type;
          break;
        }
        // In the past we have tried to assert that "called_interface" is assignable
        // from "this_type.GetClass()", however, as we do an imprecise Join
        // (RegType::JoinClass) we don't have full information on what interfaces are
        // implemented by "this_type". For example, two classes may implement the same
        // interfaces and have a common parent that doesn't implement the interface. The
        // join will set "this_type" to the parent class and a test that this implements
        // the interface will incorrectly fail.
      }
      /*
       * We don't have an object instance, so we can't find the concrete method. However, all of
       * the type information is in the abstract method, so we're good.
       */
      uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
      const dex::MethodId& method_id = dex_file_->GetMethodId(method_idx);
      dex::TypeIndex return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
      DCHECK_IMPLIES(abs_method != nullptr,
                     abs_method->GetReturnTypeDescriptorView() ==
                         dex_file_->GetTypeDescriptorView(return_type_idx));
      const RegType& return_type = reg_types_.FromTypeIndex(return_type_idx);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_POLYMORPHIC:
    case Instruction::INVOKE_POLYMORPHIC_RANGE: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_POLYMORPHIC_RANGE);
      ArtMethod* called_method = VerifyInvocationArgs(inst, METHOD_POLYMORPHIC, is_range);
      if (called_method == nullptr) {
        // Convert potential soft failures in VerifyInvocationArgs() to hard errors.
        if (failure_messages_.size() > 0) {
          std::string message = failure_messages_.back()->str();
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << message;
        } else {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invoke-polymorphic verification failure.";
        }
        break;
      }
      if (!CheckSignaturePolymorphicMethod(called_method) ||
          !CheckSignaturePolymorphicReceiver(inst)) {
        DCHECK(HasFailures());
        break;
      }
      const dex::ProtoIndex proto_idx((is_range) ? inst->VRegH_4rcc() : inst->VRegH_45cc());
      const RegType& return_type =
          reg_types_.FromTypeIndex(dex_file_->GetProtoId(proto_idx).return_type_idx_);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_CUSTOM:
    case Instruction::INVOKE_CUSTOM_RANGE: {
      // Verify registers based on method_type in the call site.
      bool is_range = (inst->Opcode() == Instruction::INVOKE_CUSTOM_RANGE);

      // Step 1. Check the call site that produces the method handle for invocation
      const uint32_t call_site_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
      if (!CheckCallSite(call_site_idx)) {
        DCHECK(HasFailures());
        break;
      }

      // Step 2. Check the register arguments correspond to the expected arguments for the
      // method handle produced by step 1. The dex file verifier has checked ranges for
      // the first three arguments and CheckCallSite has checked the method handle type.
      const dex::ProtoIndex proto_idx = dex_file_->GetProtoIndexForCallSite(call_site_idx);
      const dex::ProtoId& proto_id = dex_file_->GetProtoId(proto_idx);
      DexFileParameterIterator param_it(*dex_file_, proto_id);
      // Treat method as static as it has yet to be determined.
      VerifyInvocationArgsFromIterator(&param_it, inst, METHOD_STATIC, is_range, nullptr);

      // Step 3. Propagate return type information
      const RegType& return_type = reg_types_.FromTypeIndex(proto_id.return_type_idx_);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      CheckUnaryOp(inst, kInteger, kInteger);
      break;
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      CheckUnaryOpWide(inst, kLongLo, kLongLo);
      break;
    case Instruction::NEG_FLOAT:
      CheckUnaryOp(inst, kFloat, kFloat);
      break;
    case Instruction::NEG_DOUBLE:
      CheckUnaryOpWide(inst, kDoubleLo, kDoubleLo);
      break;
    case Instruction::INT_TO_LONG:
      CheckUnaryOpToWide(inst, kLongLo, kInteger);
      break;
    case Instruction::INT_TO_FLOAT:
      CheckUnaryOp(inst, kFloat, kInteger);
      break;
    case Instruction::INT_TO_DOUBLE:
      CheckUnaryOpToWide(inst, kDoubleLo, kInteger);
      break;
    case Instruction::LONG_TO_INT:
      CheckUnaryOpFromWide(inst, kInteger, kLongLo);
      break;
    case Instruction::LONG_TO_FLOAT:
      CheckUnaryOpFromWide(inst, kFloat, kLongLo);
      break;
    case Instruction::LONG_TO_DOUBLE:
      CheckUnaryOpWide(inst, kDoubleLo, kLongLo);
      break;
    case Instruction::FLOAT_TO_INT:
      CheckUnaryOp(inst, kInteger, kFloat);
      break;
    case Instruction::FLOAT_TO_LONG:
      CheckUnaryOpToWide(inst, kLongLo, kFloat);
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      CheckUnaryOpToWide(inst, kDoubleLo, kFloat);
      break;
    case Instruction::DOUBLE_TO_INT:
      CheckUnaryOpFromWide(inst, kInteger, kDoubleLo);
      break;
    case Instruction::DOUBLE_TO_LONG:
      CheckUnaryOpWide(inst, kLongLo, kDoubleLo);
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      CheckUnaryOpFromWide(inst, kFloat, kDoubleLo);
      break;
    case Instruction::INT_TO_BYTE:
      CheckUnaryOp(inst, kByte, kInteger);
      break;
    case Instruction::INT_TO_CHAR:
      CheckUnaryOp(inst, kChar, kInteger);
      break;
    case Instruction::INT_TO_SHORT:
      CheckUnaryOp(inst, kShort, kInteger);
      break;

    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::REM_INT:
    case Instruction::DIV_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
      CheckBinaryOp(inst, kInteger, kInteger, kInteger, /*check_boolean_op=*/ false);
      break;
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
      CheckBinaryOp(inst, kInteger, kInteger, kInteger, /*check_boolean_op=*/ true);
      break;
    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      CheckBinaryOpWide(inst, kLongLo, kLongLo, kLongLo);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
      /* shift distance is Int, making these different from other binary operations */
      CheckBinaryOpWideShift(inst, kLongLo, kInteger);
      break;
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
      CheckBinaryOp(inst, kFloat, kFloat, kFloat, /*check_boolean_op=*/ false);
      break;
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
      CheckBinaryOpWide(inst, kDoubleLo, kDoubleLo, kDoubleLo);
      break;
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
      CheckBinaryOp2addr(inst, kInteger, kInteger, kInteger, /*check_boolean_op=*/ false);
      break;
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
      CheckBinaryOp2addr(inst, kInteger, kInteger, kInteger, /*check_boolean_op=*/ true);
      break;
    case Instruction::DIV_INT_2ADDR:
      CheckBinaryOp2addr(inst, kInteger, kInteger, kInteger, /*check_boolean_op=*/ false);
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      CheckBinaryOp2addrWide(inst, kLongLo, kLongLo, kLongLo);
      break;
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      CheckBinaryOp2addrWideShift(inst, kLongLo, kInteger);
      break;
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      CheckBinaryOp2addr(inst, kFloat, kFloat, kFloat, /*check_boolean_op=*/ false);
      break;
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      CheckBinaryOp2addrWide(inst, kDoubleLo, kDoubleLo, kDoubleLo);
      break;
    case Instruction::ADD_INT_LIT16:
    case Instruction::RSUB_INT_LIT16:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
      CheckLiteralOp(inst, kInteger, kInteger, /*check_boolean_op=*/ false, /*is_lit16=*/ true);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
      CheckLiteralOp(inst, kInteger, kInteger, /*check_boolean_op=*/ true, /*is_lit16=*/ true);
      break;
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      CheckLiteralOp(inst, kInteger, kInteger, /*check_boolean_op=*/ false, /*is_lit16=*/ false);
      break;
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
      CheckLiteralOp(inst, kInteger, kInteger, /*check_boolean_op=*/ true, /*is_lit16=*/ false);
      break;

    /* These should never appear during verification. */
    case Instruction::UNUSED_3E ... Instruction::UNUSED_43:
    case Instruction::UNUSED_E3 ... Instruction::UNUSED_F9:
    case Instruction::UNUSED_73:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Unexpected opcode " << inst->DumpString(dex_file_);
      break;

    /*
     * DO NOT add a "default" clause here. Without it the compiler will
     * complain if an instruction is missing (which is desirable).
     */
  }  // end - switch (dec_insn.opcode)

  if (flags_.have_pending_hard_failure_) {
    if (IsAotMode()) {
      /* When AOT compiling, check that the last failure is a hard failure */
      if (failures_[failures_.size() - 1] != VERIFY_ERROR_BAD_CLASS_HARD) {
        LOG(ERROR) << "Pending failures:";
        for (auto& error : failures_) {
          LOG(ERROR) << error;
        }
        for (auto& error_msg : failure_messages_) {
          LOG(ERROR) << error_msg->str();
        }
        LOG(FATAL) << "Pending hard failure, but last failure not hard.";
      }
    }
    /* immediate failure, reject class */
    InfoMessages() << "Rejecting opcode " << inst->DumpString(dex_file_);
    return false;
  } else if (flags_.have_pending_runtime_throw_failure_) {
    LogVerifyInfo() << "Elevating opcode flags from " << opcode_flags << " to Throw";
    /* checking interpreter will throw, mark following code as unreachable */
    opcode_flags = Instruction::kThrow;
    // Note: the flag must be reset as it is only global to decouple Fail and is semantically per
    //       instruction. However, RETURN checking may throw LOCKING errors, so we clear at the
    //       very end.
  }
  /*
   * If we didn't just set the result register, clear it out. This ensures that you can only use
   * "move-result" immediately after the result is set. (We could check this statically, but it's
   * not expensive and it makes our debugging output cleaner.)
   */
  if (!just_set_result) {
    work_line_->SetResultTypeToUnknown(GetRegTypeCache());
  }

  /*
   * Handle "branch". Tag the branch target.
   *
   * NOTE: instructions like Instruction::EQZ provide information about the
   * state of the register when the branch is taken or not taken. For example,
   * somebody could get a reference field, check it for zero, and if the
   * branch is taken immediately store that register in a boolean field
   * since the value is known to be zero. We do not currently account for
   * that, and will reject the code.
   *
   * TODO: avoid re-fetching the branch target
   */
  if ((opcode_flags & Instruction::kBranch) != 0) {
    bool isConditional, selfOkay;
    if (!GetBranchOffset(work_insn_idx_, &branch_target, &isConditional, &selfOkay)) {
      /* should never happen after static verification */
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad branch";
      return false;
    }
    DCHECK_EQ(isConditional, (opcode_flags & Instruction::kContinue) != 0);
    if (!CheckNotMoveExceptionOrMoveResult(code_item_accessor_.Insns(),
                                           work_insn_idx_ + branch_target)) {
      return false;
    }
    /* update branch target, set "changed" if appropriate */
    if (nullptr != branch_line) {
      if (!UpdateRegisters(work_insn_idx_ + branch_target, branch_line.get(), false)) {
        return false;
      }
    } else {
      if (!UpdateRegisters(work_insn_idx_ + branch_target, work_line_.get(), false)) {
        return false;
      }
    }
  }

  /*
   * Handle "switch". Tag all possible branch targets.
   *
   * We've already verified that the table is structurally sound, so we
   * just need to walk through and tag the targets.
   */
  if ((opcode_flags & Instruction::kSwitch) != 0) {
    int offset_to_switch = insns[1] | (static_cast<int32_t>(insns[2]) << 16);
    const uint16_t* switch_insns = insns + offset_to_switch;
    int switch_count = switch_insns[1];
    int offset_to_targets, targ;

    if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
      /* 0 = sig, 1 = count, 2/3 = first key */
      offset_to_targets = 4;
    } else {
      /* 0 = sig, 1 = count, 2..count * 2 = keys */
      DCHECK((*insns & 0xff) == Instruction::SPARSE_SWITCH);
      offset_to_targets = 2 + 2 * switch_count;
    }

    /* verify each switch target */
    for (targ = 0; targ < switch_count; targ++) {
      int offset;
      uint32_t abs_offset;

      /* offsets are 32-bit, and only partly endian-swapped */
      offset = switch_insns[offset_to_targets + targ * 2] |
         (static_cast<int32_t>(switch_insns[offset_to_targets + targ * 2 + 1]) << 16);
      abs_offset = work_insn_idx_ + offset;
      DCHECK_LT(abs_offset, code_item_accessor_.InsnsSizeInCodeUnits());
      if (!CheckNotMoveExceptionOrMoveResult(code_item_accessor_.Insns(), abs_offset)) {
        return false;
      }
      if (!UpdateRegisters(abs_offset, work_line_.get(), false)) {
        return false;
      }
    }
  }

  /*
   * Handle instructions that can throw and that are sitting in a "try" block. (If they're not in a
   * "try" block when they throw, control transfers out of the method.)
   */
  if ((opcode_flags & Instruction::kThrow) != 0 && GetInstructionFlags(work_insn_idx_).IsInTry()) {
    bool has_catch_all_handler = false;
    const dex::TryItem* try_item = code_item_accessor_.FindTryItem(work_insn_idx_);
    CHECK(try_item != nullptr);
    CatchHandlerIterator iterator(code_item_accessor_, *try_item);

    // Need the linker to try and resolve the handled class to check if it's Throwable.
    ClassLinker* linker = GetClassLinker();

    for (; iterator.HasNext(); iterator.Next()) {
      dex::TypeIndex handler_type_idx = iterator.GetHandlerTypeIndex();
      if (!handler_type_idx.IsValid()) {
        has_catch_all_handler = true;
      } else {
        // It is also a catch-all if it is java.lang.Throwable.
        ObjPtr<mirror::Class> klass =
            linker->ResolveType(handler_type_idx, dex_cache_, class_loader_);
        if (klass != nullptr) {
          if (klass == GetClassRoot<mirror::Throwable>()) {
            has_catch_all_handler = true;
          }
        } else {
          // Clear exception.
          DCHECK(self_->IsExceptionPending());
          self_->ClearException();
        }
      }
      /*
       * Merge registers into the "catch" block. We want to use the "savedRegs" rather than
       * "work_regs", because at runtime the exception will be thrown before the instruction
       * modifies any registers.
       */
      if (kVerifierDebug) {
        LogVerifyInfo() << "Updating exception handler 0x"
                        << std::hex << iterator.GetHandlerAddress();
      }
      if (!UpdateRegisters(iterator.GetHandlerAddress(), saved_line_.get(), false)) {
        return false;
      }
    }

    /*
     * If the monitor stack depth is nonzero, there must be a "catch all" handler for this
     * instruction. This does apply to monitor-exit because of async exception handling.
     */
    if (work_line_->MonitorStackDepth() > 0 && !has_catch_all_handler) {
      /*
       * The state in work_line reflects the post-execution state. If the current instruction is a
       * monitor-enter and the monitor stack was empty, we don't need a catch-all (if it throws,
       * it will do so before grabbing the lock).
       */
      if (inst->Opcode() != Instruction::MONITOR_ENTER || work_line_->MonitorStackDepth() != 1) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD)
            << "expected to be within a catch-all for an instruction where a monitor is held";
        return false;
      }
    }
  }

  /* Handle "continue". Tag the next consecutive instruction.
   *  Note: Keep the code handling "continue" case below the "branch" and "switch" cases,
   *        because it changes work_line_ when performing peephole optimization
   *        and this change should not be used in those cases.
   */
  if ((opcode_flags & Instruction::kContinue) != 0 && !exc_handler_unreachable) {
    DCHECK_EQ(&code_item_accessor_.InstructionAt(work_insn_idx_), inst);
    uint32_t next_insn_idx = work_insn_idx_ + inst->SizeInCodeUnits();
    if (next_insn_idx >= code_item_accessor_.InsnsSizeInCodeUnits()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Execution can walk off end of code area";
      return false;
    }
    // The only way to get to a move-exception instruction is to get thrown there. Make sure the
    // next instruction isn't one.
    if (!CheckNotMoveException(code_item_accessor_.Insns(), next_insn_idx)) {
      return false;
    }
    if (nullptr != fallthrough_line) {
      // Make workline consistent with fallthrough computed from peephole optimization.
      work_line_->CopyFromLine(fallthrough_line.get());
    }
    if (GetInstructionFlags(next_insn_idx).IsReturn()) {
      // For returns we only care about the operand to the return, all other registers are dead.
      const Instruction* ret_inst = &code_item_accessor_.InstructionAt(next_insn_idx);
      AdjustReturnLine(this, ret_inst, work_line_.get());
    }
    RegisterLine* next_line = reg_table_.GetLine(next_insn_idx);
    if (next_line != nullptr) {
      // Merge registers into what we have for the next instruction, and set the "changed" flag if
      // needed. If the merge changes the state of the registers then the work line will be
      // updated.
      if (!UpdateRegisters(next_insn_idx, work_line_.get(), true)) {
        return false;
      }
    } else {
      /*
       * We're not recording register data for the next instruction, so we don't know what the
       * prior state was. We have to assume that something has changed and re-evaluate it.
       */
      GetModifiableInstructionFlags(next_insn_idx).SetChanged();
    }
  }

  /* If we're returning from the method, make sure monitor stack is empty. */
  if ((opcode_flags & Instruction::kReturn) != 0) {
    work_line_->VerifyMonitorStackEmpty(this);
  }

  /*
   * Update start_guess. Advance to the next instruction of that's
   * possible, otherwise use the branch target if one was found. If
   * neither of those exists we're in a return or throw; leave start_guess
   * alone and let the caller sort it out.
   */
  if ((opcode_flags & Instruction::kContinue) != 0) {
    DCHECK_EQ(&code_item_accessor_.InstructionAt(work_insn_idx_), inst);
    *start_guess = work_insn_idx_ + inst->SizeInCodeUnits();
  } else if ((opcode_flags & Instruction::kBranch) != 0) {
    /* we're still okay if branch_target is zero */
    *start_guess = work_insn_idx_ + branch_target;
  }

  DCHECK_LT(*start_guess, code_item_accessor_.InsnsSizeInCodeUnits());
  DCHECK(GetInstructionFlags(*start_guess).IsOpcode());

  if (flags_.have_pending_runtime_throw_failure_) {
    Fail(VERIFY_ERROR_RUNTIME_THROW, /* pending_exc= */ false);
    // Reset the pending_runtime_throw flag now.
    flags_.have_pending_runtime_throw_failure_ = false;
  }

  return true;
}  // NOLINT(readability/fn_size)

template <bool kVerifierDebug>
template <CheckAccess C>
const RegType& MethodVerifier<kVerifierDebug>::ResolveClass(dex::TypeIndex class_idx) {
  // FIXME: `RegTypeCache` can currently return a few fundamental classes such as j.l.Object
  // or j.l.Class without resolving them using the current class loader and recording them
  // in the corresponding `ClassTable`. The subsequent method and field lookup by callers of
  // `ResolveClass<>()` can then put their methods and fields to the `DexCache` which should
  // not be done for classes that are not in the `ClassTable`, potentially leading to crashes.
  // For now, we force the class resolution here to avoid the inconsistency.
  // Note that there's nothing we can do if we cannot load classes. (The only code path that
  // does not allow loading classes is `FindLocksAtDexPc()` which should really need only to
  // distinguish between reference and non-reference types and track locking. All the other
  // work, including class lookup, is unnecessary as the class has already been verified.)
  if (CanLoadClasses()) {
    ClassLinker* linker = GetClassLinker();
    ObjPtr<mirror::Class> klass =  linker->ResolveType(class_idx, dex_cache_, class_loader_);
    if (klass == nullptr) {
      DCHECK(self_->IsExceptionPending());
      self_->ClearException();
    }
  }

  const RegType& result = reg_types_.FromTypeIndex(class_idx);
  if (result.IsConflict()) {
    const char* descriptor = dex_file_->GetTypeDescriptor(class_idx);
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "accessing broken descriptor '" << descriptor
        << "' in " << GetDeclaringClass();
    return result;
  }

  // If requested, check if access is allowed. Unresolved types are included in this check, as the
  // interpreter only tests whether access is allowed when a class is not pre-verified and runs in
  // the access-checks interpreter. If result is primitive, skip the access check.
  //
  // Note: we do this for unresolved classes to trigger re-verification at runtime.
  if (C != CheckAccess::kNo &&
      result.IsNonZeroReferenceTypes() &&
      ((C == CheckAccess::kYes && IsSdkVersionSetAndAtLeast(api_level_, SdkVersion::kP))
          || !result.IsUnresolvedTypes())) {
    const RegType& referrer = GetDeclaringClass();
    if ((IsSdkVersionSetAndAtLeast(api_level_, SdkVersion::kP) || !referrer.IsUnresolvedTypes()) &&
        !CanAccess(result)) {
      if (IsAotMode()) {
        Fail(VERIFY_ERROR_ACCESS_CLASS);
        VLOG(verifier)
            << "(possibly) illegal class access: '" << referrer << "' -> '" << result << "'";
      } else {
        Fail(VERIFY_ERROR_ACCESS_CLASS)
            << "(possibly) illegal class access: '" << referrer << "' -> '" << result << "'";
      }
    }
  }
  return result;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::HandleMoveException(const Instruction* inst)  {
  // We do not allow MOVE_EXCEPTION as the first instruction in a method. This is a simple case
  // where one entrypoint to the catch block is not actually an exception path.
  if (work_insn_idx_ == 0) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "move-exception at pc 0x0";
    return true;
  }
  /*
   * This statement can only appear as the first instruction in an exception handler. We verify
   * that as part of extracting the exception type from the catch block list.
   */
  auto caught_exc_type_fn = [&]() REQUIRES_SHARED(Locks::mutator_lock_) ->
      std::pair<bool, const RegType*> {
    const RegType* common_super = nullptr;
    if (code_item_accessor_.TriesSize() != 0) {
      const uint8_t* handlers_ptr = code_item_accessor_.GetCatchHandlerData();
      uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
      const RegType* unresolved = nullptr;
      for (uint32_t i = 0; i < handlers_size; i++) {
        CatchHandlerIterator iterator(handlers_ptr);
        for (; iterator.HasNext(); iterator.Next()) {
          if (iterator.GetHandlerAddress() == (uint32_t) work_insn_idx_) {
            if (!iterator.GetHandlerTypeIndex().IsValid()) {
              common_super = &reg_types_.JavaLangThrowable();
            } else {
              // Do access checks only on resolved exception classes.
              const RegType& exception =
                  ResolveClass<CheckAccess::kOnResolvedClass>(iterator.GetHandlerTypeIndex());
              if (!IsAssignableFrom(reg_types_.JavaLangThrowable(), exception)) {
                DCHECK(!exception.IsUninitializedTypes());  // Comes from dex, shouldn't be uninit.
                if (exception.IsUnresolvedTypes()) {
                  if (unresolved == nullptr) {
                    unresolved = &exception;
                  } else {
                    unresolved = &unresolved->SafeMerge(exception, &reg_types_, this);
                  }
                } else {
                  Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected non-throwable class "
                                                    << exception;
                  return std::make_pair(true, &reg_types_.Conflict());
                }
              } else if (common_super == nullptr) {
                common_super = &exception;
              } else if (common_super->Equals(exception)) {
                // odd case, but nothing to do
              } else {
                common_super = &common_super->Merge(exception, &reg_types_, this);
                if (FailOrAbort(IsAssignableFrom(reg_types_.JavaLangThrowable(), *common_super),
                                "java.lang.Throwable is not assignable-from common_super at ",
                                work_insn_idx_)) {
                  break;
                }
              }
            }
          }
        }
        handlers_ptr = iterator.EndDataPointer();
      }
      if (unresolved != nullptr) {
        // Soft-fail, but do not handle this with a synthetic throw.
        Fail(VERIFY_ERROR_UNRESOLVED_TYPE_CHECK, /*pending_exc=*/ false)
            << "Unresolved catch handler";
        bool should_continue = true;
        if (common_super != nullptr) {
          unresolved = &unresolved->Merge(*common_super, &reg_types_, this);
        } else {
          should_continue = !PotentiallyMarkRuntimeThrow();
        }
        return std::make_pair(should_continue, unresolved);
      }
    }
    if (common_super == nullptr) {
      /* No catch block */
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unable to find exception handler";
      return std::make_pair(true, &reg_types_.Conflict());
    }
    DCHECK(common_super->HasClass());
    CheckForFinalAbstractClass(common_super->GetClass());
    return std::make_pair(true, common_super);
  };
  auto result = caught_exc_type_fn();
  work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_11x(), *result.second);
  return result.first;
}

template <bool kVerifierDebug>
ArtMethod* MethodVerifier<kVerifierDebug>::ResolveMethodAndCheckAccess(
    uint32_t dex_method_idx, MethodType method_type) {
  const dex::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx);
  const RegType& klass_type = ResolveClass<CheckAccess::kYes>(method_id.class_idx_);
  if (klass_type.IsConflict()) {
    std::string append(" in attempt to access method ");
    append += dex_file_->GetMethodName(method_id);
    AppendToLastFailMessage(append);
    return nullptr;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return nullptr;  // Can't resolve Class so no more to do here
  }
  ClassLinker* class_linker = GetClassLinker();
  ObjPtr<mirror::Class> klass = GetRegTypeClass(klass_type);

  ArtMethod* res_method = dex_cache_->GetResolvedMethod(dex_method_idx);
  if (res_method == nullptr) {
    res_method = class_linker->FindResolvedMethod(
        klass, dex_cache_.Get(), class_loader_.Get(), dex_method_idx);
  }

  bool must_fail = false;
  // This is traditional and helps with screwy bytecode. It will tell you that, yes, a method
  // exists, but that it's called incorrectly. This significantly helps debugging, as locally it's
  // hard to see the differences.
  // If we don't have res_method here we must fail. Just use this bool to make sure of that with a
  // DCHECK.
  if (res_method == nullptr) {
    must_fail = true;
    // Try to find the method also with the other type for better error reporting below
    // but do not store such bogus lookup result in the DexCache or VerifierDeps.
    res_method = class_linker->FindIncompatibleMethod(
        klass, dex_cache_.Get(), class_loader_.Get(), dex_method_idx);
  }

  if (res_method == nullptr) {
    Fail(VERIFY_ERROR_NO_METHOD) << "couldn't find method "
                                 << klass->PrettyDescriptor() << "."
                                 << dex_file_->GetMethodName(method_id) << " "
                                 << dex_file_->GetMethodSignature(method_id);
    return nullptr;
  }

  // Make sure calls to constructors are "direct". There are additional restrictions but we don't
  // enforce them here.
  if (res_method->IsConstructor() && method_type != METHOD_DIRECT) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "rejecting non-direct call to constructor "
                                      << res_method->PrettyMethod();
    return nullptr;
  }
  // Disallow any calls to class initializers.
  if (res_method->IsClassInitializer()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "rejecting call to class initializer "
                                      << res_method->PrettyMethod();
    return nullptr;
  }

  // Check that interface methods are static or match interface classes.
  // We only allow statics if we don't have default methods enabled.
  //
  // Note: this check must be after the initializer check, as those are required to fail a class,
  //       while this check implies an IncompatibleClassChangeError.
  if (klass->IsInterface()) {
    // methods called on interfaces should be invoke-interface, invoke-super, invoke-direct (if
    // default methods are supported for the dex file), or invoke-static.
    if (method_type != METHOD_INTERFACE &&
        method_type != METHOD_STATIC &&
        (!dex_file_->SupportsDefaultMethods() ||
         method_type != METHOD_DIRECT) &&
        method_type != METHOD_SUPER) {
      Fail(VERIFY_ERROR_CLASS_CHANGE)
          << "non-interface method " << dex_file_->PrettyMethod(dex_method_idx)
          << " is in an interface class " << klass->PrettyClass();
      return nullptr;
    }
  } else {
    if (method_type == METHOD_INTERFACE) {
      Fail(VERIFY_ERROR_CLASS_CHANGE)
          << "interface method " << dex_file_->PrettyMethod(dex_method_idx)
          << " is in a non-interface class " << klass->PrettyClass();
      return nullptr;
    }
  }

  // Check specifically for non-public object methods being provided for interface dispatch. This
  // can occur if we failed to find a method with FindInterfaceMethod but later find one with
  // FindClassMethod for error message use.
  if (method_type == METHOD_INTERFACE &&
      res_method->GetDeclaringClass()->IsObjectClass() &&
      !res_method->IsPublic()) {
    Fail(VERIFY_ERROR_NO_METHOD) << "invoke-interface " << klass->PrettyDescriptor() << "."
                                 << dex_file_->GetMethodName(method_id) << " "
                                 << dex_file_->GetMethodSignature(method_id) << " resolved to "
                                 << "non-public object method " << res_method->PrettyMethod() << " "
                                 << "but non-public Object methods are excluded from interface "
                                 << "method resolution.";
    return nullptr;
  }
  // Check if access is allowed.
  if (!CanAccessMember(res_method->GetDeclaringClass(), res_method->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_METHOD) << "illegal method access (call "
                                     << res_method->PrettyMethod()
                                     << " from " << GetDeclaringClass() << ")";
    return res_method;
  }
  // Check that invoke-virtual and invoke-super are not used on private methods of the same class.
  if (res_method->IsPrivate() && (method_type == METHOD_VIRTUAL || method_type == METHOD_SUPER)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invoke-super/virtual can't be used on private method "
                                      << res_method->PrettyMethod();
    return nullptr;
  }
  // See if the method type implied by the invoke instruction matches the access flags for the
  // target method. The flags for METHOD_POLYMORPHIC are based on there being precisely two
  // signature polymorphic methods supported by the run-time which are native methods with variable
  // arguments.
  if ((method_type == METHOD_DIRECT && (!res_method->IsDirect() || res_method->IsStatic())) ||
      (method_type == METHOD_STATIC && !res_method->IsStatic()) ||
      ((method_type == METHOD_SUPER ||
        method_type == METHOD_VIRTUAL ||
        method_type == METHOD_INTERFACE) && res_method->IsDirect()) ||
      ((method_type == METHOD_POLYMORPHIC) &&
       (!res_method->IsNative() || !res_method->IsVarargs()))) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "invoke type (" << method_type << ") does not match method "
                                       "type of " << res_method->PrettyMethod();
    return nullptr;
  }
  // Make sure we weren't expecting to fail.
  DCHECK(!must_fail) << "invoke type (" << method_type << ")"
                     << klass->PrettyDescriptor() << "."
                     << dex_file_->GetMethodName(method_id) << " "
                     << dex_file_->GetMethodSignature(method_id) << " unexpectedly resolved to "
                     << res_method->PrettyMethod() << " without error. Initially this method was "
                     << "not found so we were expecting to fail for some reason.";
  return res_method;
}

template <bool kVerifierDebug>
template <class T>
ArtMethod* MethodVerifier<kVerifierDebug>::VerifyInvocationArgsFromIterator(
    T* it, const Instruction* inst, MethodType method_type, bool is_range, ArtMethod* res_method) {
  DCHECK_EQ(!is_range, inst->HasVarArgs());

  // We use vAA as our expected arg count, rather than res_method->insSize, because we need to
  // match the call to the signature. Also, we might be calling through an abstract method
  // definition (which doesn't have register count values).
  const size_t expected_args = inst->VRegA();
  /* caught by static verifier */
  DCHECK(is_range || expected_args <= 5);

  if (expected_args > code_item_accessor_.OutsSize()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid argument count (" << expected_args
                                      << ") exceeds outsSize ("
                                      << code_item_accessor_.OutsSize() << ")";
    return nullptr;
  }

  /*
   * Check the "this" argument, which must be an instance of the class that declared the method.
   * For an interface class, we don't do the full interface merge (see JoinClass), so we can't do a
   * rigorous check here (which is okay since we have to do it at runtime).
   */
  if (method_type != METHOD_STATIC) {
    const RegType& actual_arg_type = GetInvocationThis(inst);
    if (actual_arg_type.IsConflict()) {  // GetInvocationThis failed.
      CHECK(flags_.have_pending_hard_failure_);
      return nullptr;
    }
    bool is_init = false;
    if (actual_arg_type.IsUninitializedTypes()) {
      if (res_method != nullptr) {
        if (!res_method->IsConstructor()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'this' arg must be initialized";
          return nullptr;
        }
      } else {
        // Check whether the name of the called method is "<init>"
        const uint32_t method_idx = GetMethodIdxOfInvoke(inst);
        if (strcmp(dex_file_->GetMethodName(dex_file_->GetMethodId(method_idx)), "<init>") != 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'this' arg must be initialized";
          return nullptr;
        }
      }
      is_init = true;
    }
    const RegType& adjusted_type = is_init
                                       ? GetRegTypeCache()->FromUninitialized(actual_arg_type)
                                       : actual_arg_type;
    if (method_type != METHOD_INTERFACE && !adjusted_type.IsZeroOrNull()) {
      // Get the referenced class first. This is fast because it's already cached by the type
      // index due to method resolution. It is usually the resolved method's declaring class.
      const uint32_t method_idx = GetMethodIdxOfInvoke(inst);
      const dex::TypeIndex class_idx = dex_file_->GetMethodId(method_idx).class_idx_;
      const RegType* res_method_class = &reg_types_.FromTypeIndex(class_idx);
      DCHECK_IMPLIES(res_method != nullptr,
                     res_method_class->IsJavaLangObject() || res_method_class->IsReference());
      DCHECK_IMPLIES(res_method != nullptr && res_method_class->IsJavaLangObject(),
                     res_method->GetDeclaringClass()->IsObjectClass());
      // Miranda methods have the declaring interface as their declaring class, not the abstract
      // class. It would be wrong to use this for the type check (interface type checks are
      // postponed to runtime).
      if (res_method != nullptr && res_method_class->IsReference() && !res_method->IsMiranda()) {
        ObjPtr<mirror::Class> klass = res_method->GetDeclaringClass();
        if (res_method_class->GetClass() != klass) {
          // The resolved method is in a superclass, not directly in the referenced class.
          res_method_class = &reg_types_.FromClass(klass);
        }
      }
      if (!IsAssignableFrom(*res_method_class, adjusted_type)) {
        Fail(adjusted_type.IsUnresolvedTypes()
                 ? VERIFY_ERROR_UNRESOLVED_TYPE_CHECK
                 : VERIFY_ERROR_BAD_CLASS_HARD)
            << "'this' argument '" << actual_arg_type << "' not instance of '"
            << *res_method_class << "'";
        // Continue on soft failures. We need to find possible hard failures to avoid problems in
        // the compiler.
        if (flags_.have_pending_hard_failure_) {
          return nullptr;
        }
      }
    }
  }

  uint32_t arg[5];
  if (!is_range) {
    inst->GetVarArgs(arg);
  }
  uint32_t sig_registers = (method_type == METHOD_STATIC) ? 0 : 1;
  for ( ; it->HasNext(); it->Next()) {
    if (sig_registers >= expected_args) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation, expected " << inst->VRegA() <<
          " argument registers, method signature has " << sig_registers + 1 << " or more";
      return nullptr;
    }

    const RegType& reg_type = reg_types_.FromTypeIndex(it->GetTypeIdx());
    uint32_t get_reg = is_range ? inst->VRegC() + static_cast<uint32_t>(sig_registers) :
        arg[sig_registers];
    if (reg_type.IsIntegralTypes()) {
      const RegType& src_type = work_line_->GetRegisterType(this, get_reg);
      if (!src_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "register v" << get_reg << " has type " << src_type
            << " but expected " << reg_type;
        return nullptr;
      }
    } else {
      if (!VerifyRegisterType(get_reg, reg_type)) {
        // Continue on soft failures. We need to find possible hard failures to avoid problems in
        // the compiler.
        if (flags_.have_pending_hard_failure_) {
          return nullptr;
        }
      } else if (reg_type.IsLongOrDoubleTypes()) {
        // Check that registers are consecutive (for non-range invokes). Invokes are the only
        // instructions not specifying register pairs by the first component, but require them
        // nonetheless. Only check when there's an actual register in the parameters. If there's
        // none, this will fail below.
        if (!is_range && sig_registers + 1 < expected_args) {
          uint32_t second_reg = arg[sig_registers + 1];
          if (second_reg != get_reg + 1) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation, long or double parameter "
                "at index " << sig_registers << " is not a pair: " << get_reg << " + "
                << second_reg << ".";
            return nullptr;
          }
        }
      }
    }
    sig_registers += reg_type.IsLongOrDoubleTypes() ?  2 : 1;
  }
  if (expected_args != sig_registers) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation, expected " << expected_args <<
        " argument registers, method signature has " << sig_registers;
    return nullptr;
  }
  return res_method;
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::VerifyInvocationArgsUnresolvedMethod(const Instruction* inst,
                                                                          MethodType method_type,
                                                                          bool is_range) {
  // As the method may not have been resolved, make this static check against what we expect.
  // The main reason for this code block is to fail hard when we find an illegal use, e.g.,
  // wrong number of arguments or wrong primitive types, even if the method could not be resolved.
  const uint32_t method_idx = GetMethodIdxOfInvoke(inst);
  DexFileParameterIterator it(*dex_file_,
                              dex_file_->GetProtoId(dex_file_->GetMethodId(method_idx).proto_idx_));
  VerifyInvocationArgsFromIterator(&it, inst, method_type, is_range, nullptr);
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckCallSite(uint32_t call_site_idx) {
  if (call_site_idx >= dex_file_->NumCallSiteIds()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Bad call site id #" << call_site_idx
                                      << " >= " << dex_file_->NumCallSiteIds();
    return false;
  }

  CallSiteArrayValueIterator it(*dex_file_, dex_file_->GetCallSiteId(call_site_idx));
  // Check essential arguments are provided. The dex file verifier has verified indices of the
  // main values (method handle, name, method_type).
  static const size_t kRequiredArguments = 3;
  if (it.Size() < kRequiredArguments) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Call site #" << call_site_idx
                                      << " has too few arguments: "
                                      << it.Size() << " < " << kRequiredArguments;
    return false;
  }

  std::pair<const EncodedArrayValueIterator::ValueType, size_t> type_and_max[kRequiredArguments] =
      { { EncodedArrayValueIterator::ValueType::kMethodHandle, dex_file_->NumMethodHandles() },
        { EncodedArrayValueIterator::ValueType::kString, dex_file_->NumStringIds() },
        { EncodedArrayValueIterator::ValueType::kMethodType, dex_file_->NumProtoIds() }
      };
  uint32_t index[kRequiredArguments];

  // Check arguments have expected types and are within permitted ranges.
  for (size_t i = 0; i < kRequiredArguments; ++i) {
    if (it.GetValueType() != type_and_max[i].first) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Call site id #" << call_site_idx
                                        << " argument " << i << " has wrong type "
                                        << it.GetValueType() << "!=" << type_and_max[i].first;
      return false;
    }
    index[i] = static_cast<uint32_t>(it.GetJavaValue().i);
    if (index[i] >= type_and_max[i].second) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Call site id #" << call_site_idx
                                        << " argument " << i << " bad index "
                                        << index[i] << " >= " << type_and_max[i].second;
      return false;
    }
    it.Next();
  }

  // Check method handle kind is valid.
  const dex::MethodHandleItem& mh = dex_file_->GetMethodHandle(index[0]);
  if (mh.method_handle_type_ != static_cast<uint16_t>(DexFile::MethodHandleType::kInvokeStatic)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Call site #" << call_site_idx
                                      << " argument 0 method handle type is not InvokeStatic: "
                                      << mh.method_handle_type_;
    return false;
  }
  return true;
}

template <bool kVerifierDebug>
ArtMethod* MethodVerifier<kVerifierDebug>::VerifyInvocationArgs(
    const Instruction* inst, MethodType method_type, bool is_range) {
  // Resolve the method. This could be an abstract or concrete method depending on what sort of call
  // we're making.
  const uint32_t method_idx = GetMethodIdxOfInvoke(inst);
  ArtMethod* res_method = ResolveMethodAndCheckAccess(method_idx, method_type);
  if (res_method == nullptr) {  // error or class is unresolved
    // Check what we can statically.
    if (!flags_.have_pending_hard_failure_) {
      VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
    }
    return nullptr;
  }

  // If we're using invoke-super(method), make sure that the executing method's class' superclass
  // has a vtable entry for the target method. Or the target is on a interface.
  if (method_type == METHOD_SUPER) {
    dex::TypeIndex class_idx = dex_file_->GetMethodId(method_idx).class_idx_;
    const RegType& reference_type = reg_types_.FromTypeIndex(class_idx);
    if (reference_type.IsUnresolvedTypes()) {
      // We cannot differentiate on whether this is a class change error or just
      // a missing method. This will be handled at runtime.
      Fail(VERIFY_ERROR_NO_METHOD) << "Unable to find referenced class from invoke-super";
      VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
      return nullptr;
    }
    DCHECK(reference_type.IsJavaLangObject() || reference_type.IsReference());
    if (reference_type.IsReference() && reference_type.GetClass()->IsInterface()) {
      if (!GetDeclaringClass().HasClass()) {
        Fail(VERIFY_ERROR_NO_CLASS) << "Unable to resolve the full class of 'this' used in an"
                                    << "interface invoke-super";
        VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
        return nullptr;
      } else if (!IsStrictlyAssignableFrom(reference_type, GetDeclaringClass())) {
        Fail(VERIFY_ERROR_CLASS_CHANGE)
            << "invoke-super in " << mirror::Class::PrettyClass(GetDeclaringClass().GetClass())
            << " in method "
            << dex_file_->PrettyMethod(dex_method_idx_) << " to method "
            << dex_file_->PrettyMethod(method_idx) << " references "
            << "non-super-interface type " << mirror::Class::PrettyClass(reference_type.GetClass());
        VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
        return nullptr;
      }
    } else {
      if (UNLIKELY(!class_def_.superclass_idx_.IsValid())) {
        // Verification error in `j.l.Object` leads to a hang while trying to verify
        // the exception class. It is better to crash directly.
        LOG(FATAL) << "No superclass for invoke-super from "
                   << dex_file_->PrettyMethod(dex_method_idx_)
                   << " to super " << res_method->PrettyMethod() << ".";
        UNREACHABLE();
      }
      const RegType& super = reg_types_.FromTypeIndex(class_def_.superclass_idx_);
      if (super.IsUnresolvedTypes()) {
        Fail(VERIFY_ERROR_NO_METHOD) << "unknown super class in invoke-super from "
                                    << dex_file_->PrettyMethod(dex_method_idx_)
                                    << " to super " << res_method->PrettyMethod();
        VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
        return nullptr;
      }
      if (!IsStrictlyAssignableFrom(reference_type, GetDeclaringClass()) ||
          (res_method->GetMethodIndex() >= GetRegTypeClass(super)->GetVTableLength())) {
        Fail(VERIFY_ERROR_NO_METHOD) << "invalid invoke-super from "
                                    << dex_file_->PrettyMethod(dex_method_idx_)
                                    << " to super " << super
                                    << "." << res_method->GetName()
                                    << res_method->GetSignature();
        VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
        return nullptr;
      }
    }
  }

  dex::ProtoIndex proto_idx;
  if (UNLIKELY(method_type == METHOD_POLYMORPHIC)) {
    // Process the signature of the calling site that is invoking the method handle.
    proto_idx = dex::ProtoIndex(inst->VRegH());
  } else {
    // Process the target method's signature.
    proto_idx = dex_file_->GetMethodId(method_idx).proto_idx_;
  }
  DexFileParameterIterator it(*dex_file_, dex_file_->GetProtoId(proto_idx));
  ArtMethod* verified_method =
      VerifyInvocationArgsFromIterator(&it, inst, method_type, is_range, res_method);

  if (verified_method != nullptr && !verified_method->GetDeclaringClass()->IsInterface()) {
    CheckForFinalAbstractClass(res_method->GetDeclaringClass());
  }

  return verified_method;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckSignaturePolymorphicMethod(ArtMethod* method) {
  ObjPtr<mirror::Class> klass = method->GetDeclaringClass();
  const char* method_name = method->GetName();

  const char* expected_return_descriptor;
  ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots = GetClassLinker()->GetClassRoots();
  if (klass == GetClassRoot<mirror::MethodHandle>(class_roots)) {
    expected_return_descriptor = mirror::MethodHandle::GetReturnTypeDescriptor(method_name);
  } else if (klass == GetClassRoot<mirror::VarHandle>(class_roots)) {
    expected_return_descriptor = mirror::VarHandle::GetReturnTypeDescriptor(method_name);
  } else {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "Signature polymorphic method in unsuppported class: " << klass->PrettyDescriptor();
    return false;
  }

  if (expected_return_descriptor == nullptr) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "Signature polymorphic method name invalid: " << method_name;
    return false;
  }

  const dex::TypeList* types = method->GetParameterTypeList();
  if (types->Size() != 1) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "Signature polymorphic method has too many arguments " << types->Size() << " != 1";
    return false;
  }

  const dex::TypeIndex argument_type_index = types->GetTypeItem(0).type_idx_;
  const char* argument_descriptor = method->GetTypeDescriptorFromTypeIdx(argument_type_index);
  if (strcmp(argument_descriptor, "[Ljava/lang/Object;") != 0) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "Signature polymorphic method has unexpected argument type: " << argument_descriptor;
    return false;
  }

  const char* return_descriptor = method->GetReturnTypeDescriptor();
  if (strcmp(return_descriptor, expected_return_descriptor) != 0) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "Signature polymorphic method has unexpected return type: " << return_descriptor
        << " != " << expected_return_descriptor;
    return false;
  }

  return true;
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::CheckSignaturePolymorphicReceiver(const Instruction* inst) {
  const RegType& this_type = GetInvocationThis(inst);
  if (this_type.IsZeroOrNull()) {
    /* null pointer always passes (and always fails at run time) */
    return true;
  } else if (!this_type.IsNonZeroReferenceTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "invoke-polymorphic receiver is not a reference: "
        << this_type;
    return false;
  } else if (this_type.IsUninitializedReference()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "invoke-polymorphic receiver is uninitialized: "
        << this_type;
    return false;
  } else if (!this_type.HasClass()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "invoke-polymorphic receiver has no class: "
        << this_type;
    return false;
  } else {
    ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots = GetClassLinker()->GetClassRoots();
    if (!this_type.GetClass()->IsSubClass(GetClassRoot<mirror::MethodHandle>(class_roots)) &&
        !this_type.GetClass()->IsSubClass(GetClassRoot<mirror::VarHandle>(class_roots))) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD)
          << "invoke-polymorphic receiver is not a subclass of MethodHandle or VarHandle: "
          << this_type;
      return false;
    }
  }
  return true;
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::VerifyNewArray(const Instruction* inst,
                                                    bool is_filled,
                                                    bool is_range) {
  dex::TypeIndex type_idx;
  if (!is_filled) {
    DCHECK_EQ(inst->Opcode(), Instruction::NEW_ARRAY);
    type_idx = dex::TypeIndex(inst->VRegC_22c());
  } else if (!is_range) {
    DCHECK_EQ(inst->Opcode(), Instruction::FILLED_NEW_ARRAY);
    type_idx = dex::TypeIndex(inst->VRegB_35c());
  } else {
    DCHECK_EQ(inst->Opcode(), Instruction::FILLED_NEW_ARRAY_RANGE);
    type_idx = dex::TypeIndex(inst->VRegB_3rc());
  }
  const RegType& res_type = ResolveClass<CheckAccess::kYes>(type_idx);
  if (res_type.IsConflict()) {  // bad class
    DCHECK_NE(failures_.size(), 0U);
  } else {
    // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
    if (!res_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "new-array on non-array class " << res_type;
    } else if (!is_filled) {
      /* make sure "size" register is valid type */
      VerifyRegisterType(inst->VRegB_22c(), RegType::Kind::kInteger);
      /* set register type to array class */
      work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_22c(), res_type);
    } else {
      DCHECK(!res_type.IsUnresolvedMergedReference());
      // Verify each register. If "arg_count" is bad, VerifyRegisterType() will run off the end of
      // the list and fail. It's legal, if silly, for arg_count to be zero.
      const RegType& expected_type = reg_types_.GetComponentType(res_type);
      uint32_t arg_count = (is_range) ? inst->VRegA_3rc() : inst->VRegA_35c();
      uint32_t arg[5];
      if (!is_range) {
        inst->GetVarArgs(arg);
      }
      for (size_t ui = 0; ui < arg_count; ui++) {
        uint32_t get_reg = is_range ? inst->VRegC_3rc() + ui : arg[ui];
        VerifyRegisterType(get_reg, expected_type);
        if (flags_.have_pending_hard_failure_) {
          // Don't continue on hard failures.
          return;
        }
      }
      // filled-array result goes into "result" register
      work_line_->SetResultRegisterType(this, res_type);
    }
  }
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::VerifyAGet(const Instruction* inst,
                                                const RegType& insn_type,
                                                bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(this, inst->VRegC_23x());
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    const RegType& array_type = work_line_->GetRegisterType(this, inst->VRegB_23x());
    if (array_type.IsZeroOrNull()) {
      // Null array class; this code path will fail at runtime. Infer a merge-able type from the
      // instruction type.
      if (!is_primitive) {
        work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_23x(), reg_types_.Null());
      } else if (insn_type.IsInteger()) {
        // Pick a non-zero constant (to distinguish with null) that can fit in any primitive.
        // We cannot use 'insn_type' as it could be a float array or an int array.
        work_line_->SetRegisterType(inst->VRegA_23x(), DetermineCat1Constant(1));
      } else if (insn_type.IsCategory1Types()) {
        // Category 1
        // The 'insn_type' is exactly the type we need.
        work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_23x(), insn_type);
      } else {
        // Category 2
        work_line_->SetRegisterTypeWide(inst->VRegA_23x(),
                                        reg_types_.ConstantLo(),
                                        reg_types_.ConstantHi());
      }
    } else if (!array_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "not array type " << array_type << " with aget";
    } else if (array_type.IsUnresolvedMergedReference()) {
      // Unresolved array types must be reference array types.
      if (is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "reference array type " << array_type
                    << " source for category 1 aget";
      } else {
        Fail(VERIFY_ERROR_NO_CLASS) << "cannot verify aget for " << array_type
            << " because of missing class";
        // Approximate with java.lang.Object[].
        work_line_->SetRegisterType(inst->VRegA_23x(), RegType::Kind::kJavaLangObject);
      }
    } else {
      /* verify the class */
      const RegType& component_type = reg_types_.GetComponentType(array_type);
      if (!component_type.IsReferenceTypes() && !is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "primitive array type " << array_type
            << " source for aget-object";
      } else if (component_type.IsNonZeroReferenceTypes() && is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "reference array type " << array_type
            << " source for category 1 aget";
      } else if (is_primitive && !insn_type.Equals(component_type) &&
                 !((insn_type.IsInteger() && component_type.IsFloat()) ||
                 (insn_type.IsLongLo() && component_type.IsDoubleLo()))) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array type " << array_type
            << " incompatible with aget of type " << insn_type;
      } else {
        // Use knowledge of the field type which is stronger than the type inferred from the
        // instruction, which can't differentiate object types and ints from floats, longs from
        // doubles.
        if (!component_type.IsLowHalf()) {
          work_line_->SetRegisterType<LockOp::kClear>(inst->VRegA_23x(), component_type);
        } else {
          work_line_->SetRegisterTypeWide(inst->VRegA_23x(), component_type,
                                          component_type.HighHalf(&reg_types_));
        }
      }
    }
  }
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::VerifyPrimitivePut(const RegType& target_type,
                                                        uint32_t vregA) {
  // Primitive assignability rules are weaker than regular assignability rules.
  bool value_compatible;
  const RegType& value_type = work_line_->GetRegisterType(this, vregA);
  if (target_type.IsIntegralTypes()) {
    value_compatible = value_type.IsIntegralTypes();
  } else if (target_type.IsFloat()) {
    value_compatible = value_type.IsFloatTypes();
  } else if (target_type.IsLongLo()) {
    DCHECK_LT(vregA + 1, work_line_->NumRegs());
    const RegType& value_type_hi = work_line_->GetRegisterType(this, vregA + 1);
    value_compatible = value_type.IsLongTypes() && value_type.CheckWidePair(value_type_hi);
  } else if (target_type.IsDoubleLo()) {
    DCHECK_LT(vregA + 1, work_line_->NumRegs());
    const RegType& value_type_hi = work_line_->GetRegisterType(this, vregA + 1);
    value_compatible = value_type.IsDoubleTypes() && value_type.CheckWidePair(value_type_hi);
  } else {
    value_compatible = false;  // unused
  }
  if (!value_compatible) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected value in v" << vregA
        << " of type " << value_type << " but expected " << target_type << " for put";
    return;
  }
}

template <bool kVerifierDebug>
void MethodVerifier<kVerifierDebug>::VerifyAPut(const Instruction* inst,
                                                const RegType& insn_type,
                                                bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(this, inst->VRegC_23x());
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    const RegType& array_type = work_line_->GetRegisterType(this, inst->VRegB_23x());
    if (array_type.IsZeroOrNull()) {
      // Null array type; this code path will fail at runtime.
      // Still check that the given value matches the instruction's type.
      // Note: this is, as usual, complicated by the fact the the instruction isn't fully typed
      //       and fits multiple register types.
      const RegType* modified_reg_type = &insn_type;
      if ((modified_reg_type == &reg_types_.Integer()) ||
          (modified_reg_type == &reg_types_.LongLo())) {
        // May be integer or float | long or double. Overwrite insn_type accordingly.
        const RegType& value_type = work_line_->GetRegisterType(this, inst->VRegA_23x());
        if (modified_reg_type == &reg_types_.Integer()) {
          if (&value_type == &reg_types_.Float()) {
            modified_reg_type = &value_type;
          }
        } else {
          if (&value_type == &reg_types_.DoubleLo()) {
            modified_reg_type = &value_type;
          }
        }
      }
      VerifyRegisterType(inst->VRegA_23x(), *modified_reg_type);
    } else if (!array_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "not array type " << array_type << " with aput";
    } else if (array_type.IsUnresolvedMergedReference()) {
      // Unresolved array types must be reference array types.
      if (is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "aput insn has type '" << insn_type
                                          << "' but unresolved type '" << array_type << "'";
      } else {
        Fail(VERIFY_ERROR_NO_CLASS) << "cannot verify aput for " << array_type
                                    << " because of missing class";
      }
    } else {
      const RegType& component_type = reg_types_.GetComponentType(array_type);
      const uint32_t vregA = inst->VRegA_23x();
      if (is_primitive) {
        bool instruction_compatible;
        if (component_type.IsIntegralTypes()) {
          instruction_compatible = component_type.Equals(insn_type);
        } else if (component_type.IsFloat()) {
          instruction_compatible = insn_type.IsInteger();  // no put-float, so expect put-int
        } else if (component_type.IsLongLo()) {
          instruction_compatible = insn_type.IsLongLo();
        } else if (component_type.IsDoubleLo()) {
          instruction_compatible = insn_type.IsLongLo();  // no put-double, so expect put-long
        } else {
          instruction_compatible = false;  // reference with primitive store
        }
        if (!instruction_compatible) {
          // This is a global failure rather than a class change failure as the instructions and
          // the descriptors for the type should have been consistent within the same file at
          // compile time.
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "aput insn has type '" << insn_type
              << "' but expected type '" << component_type << "'";
          return;
        }
        VerifyPrimitivePut(component_type, vregA);
      } else {
        if (!component_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "primitive array type " << array_type
              << " source for aput-object";
        } else {
          // The instruction agrees with the type of array, confirm the value to be stored does too
          // Note: we use the instruction type (rather than the component type) for aput-object as
          // incompatible classes will be caught at runtime as an array store exception
          VerifyRegisterType(vregA, insn_type);
        }
      }
    }
  }
}

template <bool kVerifierDebug>
ArtField* MethodVerifier<kVerifierDebug>::GetStaticField(uint32_t field_idx, bool is_put) {
  const dex::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  // Check access to class
  const RegType& klass_type = ResolveClass<CheckAccess::kYes>(field_id.class_idx_);
  // Dex file verifier ensures that field ids reference valid descriptors starting with `L`.
  DCHECK(klass_type.IsJavaLangObject() ||
         klass_type.IsReference() ||
         klass_type.IsUnresolvedReference());
  if (klass_type.IsUnresolvedReference()) {
    // Accessibility checks depend on resolved fields.
    DCHECK(klass_type.Equals(GetDeclaringClass()) ||
           !failures_.empty() ||
           IsSdkVersionSetAndLessThan(api_level_, SdkVersion::kP));
    return nullptr;  // Can't resolve Class so no more to do here, will do checking at runtime.
  }
  ClassLinker* class_linker = GetClassLinker();
  ArtField* field = class_linker->ResolveFieldJLS(field_idx, dex_cache_, class_loader_);
  if (field == nullptr) {
    VLOG(verifier) << "Unable to resolve static field " << field_idx << " ("
              << dex_file_->GetFieldName(field_id) << ") in "
              << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(self_->IsExceptionPending());
    self_->ClearException();
    Fail(VERIFY_ERROR_NO_FIELD)
        << "field " << dex_file_->PrettyField(field_idx)
        << " not found in the resolved type " << klass_type;
    return nullptr;
  } else if (!field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << field->PrettyField() << " to be static";
    return nullptr;
  }

  return GetISFieldCommon(field, is_put);
}

template <bool kVerifierDebug>
ArtField* MethodVerifier<kVerifierDebug>::GetInstanceField(uint32_t vregB,
                                                           uint32_t field_idx,
                                                           bool is_put) {
  const RegType& obj_type = work_line_->GetRegisterType(this, vregB);
  if (!obj_type.IsReferenceTypes()) {
    // Trying to read a field from something that isn't a reference.
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "instance field access on object that has non-reference type " << obj_type;
    return nullptr;
  }
  const dex::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  // Check access to class.
  const RegType& klass_type = ResolveClass<CheckAccess::kYes>(field_id.class_idx_);
  // Dex file verifier ensures that field ids reference valid descriptors starting with `L`.
  DCHECK(klass_type.IsJavaLangObject() ||
         klass_type.IsReference() ||
         klass_type.IsUnresolvedReference());
  ArtField* field = nullptr;
  if (!klass_type.IsUnresolvedReference()) {
    ClassLinker* class_linker = GetClassLinker();
    field = class_linker->ResolveFieldJLS(field_idx, dex_cache_, class_loader_);
    if (field == nullptr) {
      VLOG(verifier) << "Unable to resolve instance field " << field_idx << " ("
                     << dex_file_->GetFieldName(field_id) << ") in "
                     << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
      DCHECK(self_->IsExceptionPending());
      self_->ClearException();
    }
  }

  if (obj_type.IsUninitializedTypes()) {
    // One is not allowed to access fields on uninitialized references, except to write to
    // fields in the constructor (before calling another constructor). We strictly check
    // that the field id references the class directly instead of some subclass.
    if (is_put && field_id.class_idx_ == GetClassDef().class_idx_) {
      if (obj_type.IsUnresolvedUninitializedThisReference()) {
        DCHECK(GetDeclaringClass().IsUnresolvedReference());
        DCHECK(GetDeclaringClass().Equals(reg_types_.FromUninitialized(obj_type)));
        ClassAccessor accessor(*dex_file_, GetClassDef());
        auto it = std::find_if(
            accessor.GetInstanceFields().begin(),
            accessor.GetInstanceFields().end(),
            [field_idx] (const ClassAccessor::Field& f) { return f.GetIndex() == field_idx; });
        if (it != accessor.GetInstanceFields().end()) {
          // There are no soft failures to report anymore, other than the class being unresolved.
          return nullptr;
        }
      } else if (obj_type.IsUninitializedThisReference()) {
        DCHECK(GetDeclaringClass().IsJavaLangObject() || GetDeclaringClass().IsReference());
        DCHECK(GetDeclaringClass().Equals(reg_types_.FromUninitialized(obj_type)));
        if (field != nullptr &&
            field->GetDeclaringClass() == GetDeclaringClass().GetClass() &&
            !field->IsStatic()) {
          // The field is now fully verified against the `obj_type`.
          return field;
        }
      }
    }
    // Allow `iget` on resolved uninitialized `this` for app compatibility.
    // This is rejected by the RI but there are Android apps that actually have such `iget`s.
    // TODO: Should we start rejecting such bytecode based on the SDK level?
    if (!is_put &&
        obj_type.IsUninitializedThisReference() &&
        field != nullptr &&
        field->GetDeclaringClass() == GetDeclaringClass().GetClass()) {
      return field;
    }
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "cannot access instance field " << dex_file_->PrettyField(field_idx)
        << " of a not fully initialized object within the context of "
        << dex_file_->PrettyMethod(dex_method_idx_);
    return nullptr;
  }

  if (klass_type.IsUnresolvedReference()) {
    // Accessibility checks depend on resolved fields.
    DCHECK(klass_type.Equals(GetDeclaringClass()) ||
           !failures_.empty() ||
           IsSdkVersionSetAndLessThan(api_level_, SdkVersion::kP));
    return nullptr;  // Can't resolve Class so no more to do here, will do checking at runtime.
  } else if (field == nullptr) {
    Fail(VERIFY_ERROR_NO_FIELD)
        << "field " << dex_file_->PrettyField(field_idx)
        << " not found in the resolved type " << klass_type;
    return nullptr;
  } else if (obj_type.IsZeroOrNull()) {
    // Cannot infer and check type, however, access will cause null pointer exception.
    // Fall through into a few last soft failure checks below.
  } else {
    ObjPtr<mirror::Class> klass = field->GetDeclaringClass();
    DCHECK_IMPLIES(klass_type.IsJavaLangObject(), klass->IsObjectClass());
    const RegType& field_klass =
        LIKELY(klass_type.IsJavaLangObject() || klass_type.GetClass() == klass)
            ? klass_type
            : reg_types_.FromClass(klass);
    DCHECK(!obj_type.IsUninitializedTypes());
    if (!IsAssignableFrom(field_klass, obj_type)) {
      // Trying to access C1.field1 using reference of type C2, which is neither C1 or a sub-class
      // of C1. For resolution to occur the declared class of the field must be compatible with
      // obj_type, we've discovered this wasn't so, so report the field didn't exist.
      DCHECK(!field_klass.IsUnresolvedTypes());
      Fail(obj_type.IsUnresolvedTypes()
                 ? VERIFY_ERROR_UNRESOLVED_TYPE_CHECK
                 : VERIFY_ERROR_BAD_CLASS_HARD)
          << "cannot access instance field " << field->PrettyField()
          << " from object of type " << obj_type;
      return nullptr;
    }
  }

  // Few last soft failure checks.
  if (field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << field->PrettyField()
                                    << " to not be static";
    return nullptr;
  }

  return GetISFieldCommon(field, is_put);
}

template <bool kVerifierDebug>
ArtField* MethodVerifier<kVerifierDebug>::GetISFieldCommon(ArtField* field, bool is_put) {
  DCHECK(field != nullptr);
  if (!CanAccessMember(field->GetDeclaringClass(), field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD)
        << "cannot access " << (field->IsStatic() ? "static" : "instance") << " field "
        << field->PrettyField() << " from " << GetDeclaringClass();
    return nullptr;
  }
  if (is_put && field->IsFinal() && field->GetDeclaringClass() != GetDeclaringClass().GetClass()) {
    Fail(VERIFY_ERROR_ACCESS_FIELD)
        << "cannot modify final field " << field->PrettyField()
        << " from other class " << GetDeclaringClass();
    return nullptr;
  }
  CheckForFinalAbstractClass(field->GetDeclaringClass());
  return field;
}

template <bool kVerifierDebug>
template <FieldAccessType kAccType>
void MethodVerifier<kVerifierDebug>::VerifyISFieldAccess(const Instruction* inst,
                                                         bool is_primitive,
                                                         bool is_static) {
  uint32_t field_idx = GetFieldIdxOfFieldAccess(inst, is_static);
  DCHECK(!flags_.have_pending_hard_failure_);
  ArtField* field;
  if (is_static) {
    field = GetStaticField(field_idx, kAccType == FieldAccessType::kAccPut);
  } else {
    field = GetInstanceField(inst->VRegB_22c(), field_idx, kAccType == FieldAccessType::kAccPut);
    if (UNLIKELY(flags_.have_pending_hard_failure_)) {
      return;
    }
  }
  DCHECK(!flags_.have_pending_hard_failure_);
  const dex::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  DCHECK_IMPLIES(field == nullptr && IsSdkVersionSetAndAtLeast(api_level_, SdkVersion::kP),
                 field_id.class_idx_ == class_def_.class_idx_ || !failures_.empty());
  const RegType& field_type = reg_types_.FromTypeIndex(field_id.type_idx_);
  const uint32_t vregA = (is_static) ? inst->VRegA_21c() : inst->VRegA_22c();
  static_assert(kAccType == FieldAccessType::kAccPut || kAccType == FieldAccessType::kAccGet,
                "Unexpected third access type");
  if (kAccType == FieldAccessType::kAccPut) {
    // sput or iput.
    if (is_primitive) {
      VerifyPrimitivePut(field_type, vregA);
    } else {
      VerifyRegisterType(vregA, field_type);
    }
  } else if (kAccType == FieldAccessType::kAccGet) {
    // sget or iget.
    if (!field_type.IsLowHalf()) {
      work_line_->SetRegisterType<LockOp::kClear>(vregA, field_type);
    } else {
      work_line_->SetRegisterTypeWide(vregA, field_type, field_type.HighHalf(&reg_types_));
    }
  } else {
    LOG(FATAL) << "Unexpected case.";
  }
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::UpdateRegisters(uint32_t next_insn,
                                                     RegisterLine* merge_line,
                                                     bool update_merge_line) {
  bool changed = true;
  RegisterLine* target_line = reg_table_.GetLine(next_insn);
  if (!GetInstructionFlags(next_insn).IsVisitedOrChanged()) {
    /*
     * We haven't processed this instruction before, and we haven't touched the registers here, so
     * there's nothing to "merge". Copy the registers over and mark it as changed. (This is the
     * only way a register can transition out of "unknown", so this is not just an optimization.)
     */
    target_line->CopyFromLine(merge_line);
    if (GetInstructionFlags(next_insn).IsReturn()) {
      // Verify that the monitor stack is empty on return.
      merge_line->VerifyMonitorStackEmpty(this);

      // For returns we only care about the operand to the return, all other registers are dead.
      // Initialize them as conflicts so they don't add to GC and deoptimization information.
      const Instruction* ret_inst = &code_item_accessor_.InstructionAt(next_insn);
      AdjustReturnLine(this, ret_inst, target_line);
      // Directly bail if a hard failure was found.
      if (flags_.have_pending_hard_failure_) {
        return false;
      }
    }
  } else {
    RegisterLineArenaUniquePtr copy;
    if (kVerifierDebug) {
      copy.reset(RegisterLine::Create(target_line->NumRegs(), allocator_, GetRegTypeCache()));
      copy->CopyFromLine(target_line);
    }
    changed = target_line->MergeRegisters(this, merge_line);
    if (flags_.have_pending_hard_failure_) {
      return false;
    }
    if (kVerifierDebug && changed) {
      LogVerifyInfo() << "Merging at [" << reinterpret_cast<void*>(work_insn_idx_) << "]"
                      << " to [" << reinterpret_cast<void*>(next_insn) << "]: " << "\n"
                      << copy->Dump(this) << "  MERGE\n"
                      << merge_line->Dump(this) << "  ==\n"
                      << target_line->Dump(this);
    }
    if (update_merge_line && changed) {
      merge_line->CopyFromLine(target_line);
    }
  }
  if (changed) {
    GetModifiableInstructionFlags(next_insn).SetChanged();
  }
  return true;
}

template <bool kVerifierDebug>
const RegType& MethodVerifier<kVerifierDebug>::GetMethodReturnType() {
  if (return_type_ == nullptr) {
    const dex::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx_);
    const dex::ProtoId& proto_id = dex_file_->GetMethodPrototype(method_id);
    return_type_ = &reg_types_.FromTypeIndex(proto_id.return_type_idx_);
  }
  return *return_type_;
}

template <bool kVerifierDebug>
RegType::Kind MethodVerifier<kVerifierDebug>::DetermineCat1Constant(int32_t value) {
  // Imprecise constant type.
  if (value < -32768) {
    return RegType::Kind::kIntegerConstant;
  } else if (value < -128) {
    return RegType::Kind::kShortConstant;
  } else if (value < 0) {
    return RegType::Kind::kByteConstant;
  } else if (value == 0) {
    return RegType::Kind::kZero;
  } else if (value == 1) {
    return RegType::Kind::kBooleanConstant;
  } else if (value < 128) {
    return RegType::Kind::kPositiveByteConstant;
  } else if (value < 32768) {
    return RegType::Kind::kPositiveShortConstant;
  } else if (value < 65536) {
    return RegType::Kind::kCharConstant;
  } else {
    return RegType::Kind::kIntegerConstant;
  }
}

template <bool kVerifierDebug>
bool MethodVerifier<kVerifierDebug>::PotentiallyMarkRuntimeThrow() {
  if (IsAotMode() || IsSdkVersionSetAndAtLeast(api_level_, SdkVersion::kS_V2)) {
    return false;
  }
  // Compatibility mode: we treat the following code unreachable and the verifier
  // will not analyze it.
  // The verifier may fail before we touch any instruction, for the signature of a method. So
  // add a check.
  if (work_insn_idx_ < dex::kDexNoIndex) {
    const Instruction& inst = code_item_accessor_.InstructionAt(work_insn_idx_);
    Instruction::Code opcode = inst.Opcode();
    if (opcode == Instruction::MOVE_EXCEPTION) {
      // This is an unreachable handler. The instruction doesn't throw, but we
      // mark the method as having a pending runtime throw failure so that
      // the compiler does not try to compile it.
      Fail(VERIFY_ERROR_RUNTIME_THROW, /* pending_exc= */ false);
      return true;
    }
    // How to handle runtime failures for instructions that are not flagged kThrow.
    if ((Instruction::FlagsOf(opcode) & Instruction::kThrow) == 0 &&
        !impl::IsCompatThrow(opcode) &&
        GetInstructionFlags(work_insn_idx_).IsInTry()) {
      if (Runtime::Current()->IsVerifierMissingKThrowFatal()) {
        LOG(FATAL) << "Unexpected throw: " << std::hex << work_insn_idx_ << " " << opcode;
        UNREACHABLE();
      }
      // We need to save the work_line if the instruction wasn't throwing before. Otherwise
      // we'll try to merge garbage.
      // Note: this assumes that Fail is called before we do any work_line modifications.
      saved_line_->CopyFromLine(work_line_.get());
    }
  }
  flags_.have_pending_runtime_throw_failure_ = true;
  return true;
}

}  // namespace
}  // namespace impl

inline ClassLinker* MethodVerifier::GetClassLinker() const {
  return reg_types_.GetClassLinker();
}

MethodVerifier::MethodVerifier(Thread* self,
                               ArenaPool* arena_pool,
                               RegTypeCache* reg_types,
                               VerifierDeps* verifier_deps,
                               const dex::ClassDef& class_def,
                               const dex::CodeItem* code_item,
                               uint32_t dex_method_idx,
                               bool aot_mode)
    : self_(self),
      allocator_(arena_pool),
      reg_types_(*reg_types),
      reg_table_(allocator_),
      work_insn_idx_(dex::kDexNoIndex),
      dex_method_idx_(dex_method_idx),
      dex_file_(reg_types->GetDexFile()),
      class_def_(class_def),
      code_item_accessor_(*dex_file_, code_item),
      flags_{ .have_pending_hard_failure_ = false, .have_pending_runtime_throw_failure_ = false },
      const_flags_{ .aot_mode_ = aot_mode, .can_load_classes_ = reg_types->CanLoadClasses() },
      encountered_failure_types_(0),
      info_messages_(std::nullopt),
      verifier_deps_(verifier_deps),
      link_(nullptr) {
}

MethodVerifier::~MethodVerifier() {
  STLDeleteElements(&failure_messages_);
}

MethodVerifier::FailureData MethodVerifier::VerifyMethod(Thread* self,
                                                         ArenaPool* arena_pool,
                                                         RegTypeCache* reg_types,
                                                         VerifierDeps* verifier_deps,
                                                         uint32_t method_idx,
                                                         Handle<mirror::DexCache> dex_cache,
                                                         const dex::ClassDef& class_def,
                                                         const dex::CodeItem* code_item,
                                                         uint32_t method_access_flags,
                                                         HardFailLogMode log_level,
                                                         uint32_t api_level,
                                                         bool aot_mode,
                                                         std::string* hard_failure_msg) {
  if (VLOG_IS_ON(verifier_debug)) {
    return VerifyMethod<true>(self,
                              arena_pool,
                              reg_types,
                              verifier_deps,
                              method_idx,
                              dex_cache,
                              class_def,
                              code_item,
                              method_access_flags,
                              log_level,
                              api_level,
                              aot_mode,
                              hard_failure_msg);
  } else {
    return VerifyMethod<false>(self,
                               arena_pool,
                               reg_types,
                               verifier_deps,
                               method_idx,
                               dex_cache,
                               class_def,
                               code_item,
                               method_access_flags,
                               log_level,
                               api_level,
                               aot_mode,
                               hard_failure_msg);
  }
}

// Return whether the runtime knows how to execute a method without needing to
// re-verify it at runtime (and therefore save on first use of the class).
// The AOT/JIT compiled code is not affected.
static inline bool CanRuntimeHandleVerificationFailure(uint32_t encountered_failure_types) {
  constexpr uint32_t unresolved_mask =
      verifier::VerifyError::VERIFY_ERROR_UNRESOLVED_TYPE_CHECK |
      verifier::VerifyError::VERIFY_ERROR_NO_CLASS |
      verifier::VerifyError::VERIFY_ERROR_CLASS_CHANGE |
      verifier::VerifyError::VERIFY_ERROR_INSTANTIATION |
      verifier::VerifyError::VERIFY_ERROR_ACCESS_CLASS |
      verifier::VerifyError::VERIFY_ERROR_ACCESS_FIELD |
      verifier::VerifyError::VERIFY_ERROR_NO_METHOD |
      verifier::VerifyError::VERIFY_ERROR_NO_FIELD |
      verifier::VerifyError::VERIFY_ERROR_ACCESS_METHOD |
      verifier::VerifyError::VERIFY_ERROR_RUNTIME_THROW;
  return (encountered_failure_types & (~unresolved_mask)) == 0;
}

template <bool kVerifierDebug>
MethodVerifier::FailureData MethodVerifier::VerifyMethod(Thread* self,
                                                         ArenaPool* arena_pool,
                                                         RegTypeCache* reg_types,
                                                         VerifierDeps* verifier_deps,
                                                         uint32_t method_idx,
                                                         Handle<mirror::DexCache> dex_cache,
                                                         const dex::ClassDef& class_def,
                                                         const dex::CodeItem* code_item,
                                                         uint32_t method_access_flags,
                                                         HardFailLogMode log_level,
                                                         uint32_t api_level,
                                                         bool aot_mode,
                                                         std::string* hard_failure_msg) {
  MethodVerifier::FailureData result;
  uint64_t start_ns = kTimeVerifyMethod ? NanoTime() : 0;

  impl::MethodVerifier<kVerifierDebug> verifier(self,
                                                arena_pool,
                                                reg_types,
                                                verifier_deps,
                                                code_item,
                                                method_idx,
                                                aot_mode,
                                                dex_cache,
                                                class_def,
                                                method_access_flags,
                                                /* verify_to_dump= */ false,
                                                api_level);
  if (verifier.Verify()) {
    // Verification completed, however failures may be pending that didn't cause the verification
    // to hard fail.
    CHECK(!verifier.flags_.have_pending_hard_failure_);

    if (verifier.failures_.size() != 0) {
      if (VLOG_IS_ON(verifier)) {
        verifier.DumpFailures(VLOG_STREAM(verifier)
            << "Soft verification failures in "
            << reg_types->GetDexFile()->PrettyMethod(method_idx) << "\n");
      }
      if (kVerifierDebug) {
        LOG(INFO) << verifier.InfoMessages().str();
        verifier.Dump(LOG_STREAM(INFO));
      }
      if (CanRuntimeHandleVerificationFailure(verifier.encountered_failure_types_)) {
        if (verifier.encountered_failure_types_ & VERIFY_ERROR_UNRESOLVED_TYPE_CHECK) {
          result.kind = FailureKind::kTypeChecksFailure;
        } else {
          result.kind = FailureKind::kAccessChecksFailure;
        }
      } else {
        result.kind = FailureKind::kSoftFailure;
      }
    }
  } else {
    // Bad method data.
    CHECK_NE(verifier.failures_.size(), 0U);
    CHECK(verifier.flags_.have_pending_hard_failure_);
    if (VLOG_IS_ON(verifier)) {
      log_level = std::max(HardFailLogMode::kLogVerbose, log_level);
    }
    if (log_level >= HardFailLogMode::kLogVerbose) {
      LogSeverity severity;
      switch (log_level) {
        case HardFailLogMode::kLogVerbose:
          severity = LogSeverity::VERBOSE;
          break;
        case HardFailLogMode::kLogWarning:
          severity = LogSeverity::WARNING;
          break;
        case HardFailLogMode::kLogInternalFatal:
          severity = LogSeverity::FATAL_WITHOUT_ABORT;
          break;
        default:
          LOG(FATAL) << "Unsupported log-level " << static_cast<uint32_t>(log_level);
          UNREACHABLE();
      }
      verifier.DumpFailures(LOG_STREAM(severity)
          << "Verification error in "
          << reg_types->GetDexFile()->PrettyMethod(method_idx) << "\n");
    }
    if (hard_failure_msg != nullptr) {
      CHECK(!verifier.failure_messages_.empty());
      *hard_failure_msg =
          verifier.failure_messages_[verifier.failure_messages_.size() - 1]->str();
    }
    result.kind = FailureKind::kHardFailure;

    if (kVerifierDebug || VLOG_IS_ON(verifier)) {
      LOG(ERROR) << verifier.InfoMessages().str();
      verifier.Dump(LOG_STREAM(ERROR));
    }
    // Under verifier-debug, dump the complete log into the error message.
    if (kVerifierDebug && hard_failure_msg != nullptr) {
      hard_failure_msg->append("\n");
      hard_failure_msg->append(verifier.InfoMessages().str());
      hard_failure_msg->append("\n");
      std::ostringstream oss;
      verifier.Dump(oss);
      hard_failure_msg->append(oss.str());
    }
  }
  if (kTimeVerifyMethod) {
    uint64_t duration_ns = NanoTime() - start_ns;
    if (duration_ns > MsToNs(Runtime::Current()->GetVerifierLoggingThresholdMs())) {
      double bytecodes_per_second =
          verifier.code_item_accessor_.InsnsSizeInCodeUnits() / (duration_ns * 1e-9);
      LOG(WARNING) << "Verification of " << reg_types->GetDexFile()->PrettyMethod(method_idx)
                   << " took " << PrettyDuration(duration_ns)
                   << (impl::IsLargeMethod(verifier.CodeItem()) ? " (large method)" : "")
                   << " (" << StringPrintf("%.2f", bytecodes_per_second) << " bytecodes/s)"
                   << " (" << verifier.allocator_.BytesAllocated() << "B arena alloc)";
    }
  }
  result.types = verifier.encountered_failure_types_;
  return result;
}

MethodVerifier* MethodVerifier::CalculateVerificationInfo(
      Thread* self,
      RegTypeCache* reg_types,
      ArtMethod* method,
      Handle<mirror::DexCache> dex_cache,
      uint32_t dex_pc) {
  Runtime* runtime = Runtime::Current();
  std::unique_ptr<impl::MethodVerifier<false>> verifier(
      new impl::MethodVerifier<false>(self,
                                      runtime->GetArenaPool(),
                                      reg_types,
                                      /* verifier_deps= */ nullptr,
                                      method->GetCodeItem(),
                                      method->GetDexMethodIndex(),
                                      runtime->IsAotCompiler(),
                                      dex_cache,
                                      *method->GetDeclaringClass()->GetClassDef(),
                                      method->GetAccessFlags(),
                                      /* verify_to_dump= */ false,
                                      // Just use the verifier at the current skd-version.
                                      // This might affect what soft-verifier errors are reported.
                                      // Callers can then filter out relevant errors if needed.
                                      runtime->GetTargetSdkVersion()));
  verifier->interesting_dex_pc_ = dex_pc;
  verifier->Verify();
  if (VLOG_IS_ON(verifier)) {
    verifier->DumpFailures(VLOG_STREAM(verifier));
    VLOG(verifier) << verifier->InfoMessages().str();
    verifier->Dump(VLOG_STREAM(verifier));
  }
  if (verifier->flags_.have_pending_hard_failure_) {
    return nullptr;
  } else {
    return verifier.release();
  }
}

void MethodVerifier::VerifyMethodAndDump(Thread* self,
                                         VariableIndentationOutputStream* vios,
                                         uint32_t dex_method_idx,
                                         const DexFile* dex_file,
                                         Handle<mirror::DexCache> dex_cache,
                                         Handle<mirror::ClassLoader> class_loader,
                                         const dex::ClassDef& class_def,
                                         const dex::CodeItem* code_item,
                                         uint32_t method_access_flags,
                                         uint32_t api_level) {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  ArenaPool* arena_pool = runtime->GetArenaPool();
  RegTypeCache reg_types(self, class_linker, arena_pool, class_loader, dex_file);
  impl::MethodVerifier<false> verifier(
      self,
      arena_pool,
      &reg_types,
      /* verifier_deps= */ nullptr,
      code_item,
      dex_method_idx,
      runtime->IsAotCompiler(),
      dex_cache,
      class_def,
      method_access_flags,
      /* verify_to_dump= */ true,
      api_level);
  verifier.Verify();
  verifier.DumpFailures(vios->Stream());
  vios->Stream() << verifier.InfoMessages().str();
  // Only dump if no hard failures. Otherwise the verifier may be not fully initialized
  // and querying any info is dangerous/can abort.
  if (!verifier.flags_.have_pending_hard_failure_) {
    verifier.Dump(vios);
  }
}

void MethodVerifier::FindLocksAtDexPc(
    ArtMethod* m,
    uint32_t dex_pc,
    std::vector<MethodVerifier::DexLockInfo>* monitor_enter_dex_pcs,
    uint32_t api_level) {
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(m->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(m->GetClassLoader()));
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  ArenaPool* arena_pool = runtime->GetArenaPool();
  RegTypeCache reg_types(self,
                         class_linker,
                         arena_pool,
                         class_loader,
                         dex_cache->GetDexFile(),
                         /* can_load_classes= */ false,
                         /* can_suspend= */ false);
  impl::MethodVerifier<false> verifier(self,
                                       arena_pool,
                                       &reg_types,
                                       /* verifier_deps= */ nullptr,
                                       m->GetCodeItem(),
                                       m->GetDexMethodIndex(),
                                       runtime->IsAotCompiler(),
                                       dex_cache,
                                       m->GetClassDef(),
                                       m->GetAccessFlags(),
                                       /* verify_to_dump= */ false,
                                       api_level);
  verifier.interesting_dex_pc_ = dex_pc;
  verifier.monitor_enter_dex_pcs_ = monitor_enter_dex_pcs;
  verifier.FindLocksAtDexPc();
}

MethodVerifier* MethodVerifier::CreateVerifier(Thread* self,
                                               RegTypeCache* reg_types,
                                               VerifierDeps* verifier_deps,
                                               Handle<mirror::DexCache> dex_cache,
                                               const dex::ClassDef& class_def,
                                               const dex::CodeItem* code_item,
                                               uint32_t method_idx,
                                               uint32_t access_flags,
                                               bool verify_to_dump,
                                               uint32_t api_level) {
  return new impl::MethodVerifier<false>(self,
                                         Runtime::Current()->GetArenaPool(),
                                         reg_types,
                                         verifier_deps,
                                         code_item,
                                         method_idx,
                                         Runtime::Current()->IsAotCompiler(),
                                         dex_cache,
                                         class_def,
                                         access_flags,
                                         verify_to_dump,
                                         api_level);
}

std::ostream& MethodVerifier::Fail(VerifyError error, bool pending_exc) {
  // Mark the error type as encountered.
  encountered_failure_types_ |= static_cast<uint32_t>(error);

  if (pending_exc) {
    switch (error) {
      case VERIFY_ERROR_NO_CLASS:
      case VERIFY_ERROR_UNRESOLVED_TYPE_CHECK:
      case VERIFY_ERROR_NO_METHOD:
      case VERIFY_ERROR_NO_FIELD:
      case VERIFY_ERROR_ACCESS_CLASS:
      case VERIFY_ERROR_ACCESS_FIELD:
      case VERIFY_ERROR_ACCESS_METHOD:
      case VERIFY_ERROR_INSTANTIATION:
      case VERIFY_ERROR_CLASS_CHANGE: {
        PotentiallyMarkRuntimeThrow();
        break;
      }

      case VERIFY_ERROR_LOCKING:
        PotentiallyMarkRuntimeThrow();
        // This will be reported to the runtime as a soft failure.
        break;

      // Hard verification failures at compile time will still fail at runtime, so the class is
      // marked as rejected to prevent it from being compiled.
      case VERIFY_ERROR_BAD_CLASS_HARD: {
        flags_.have_pending_hard_failure_ = true;
        break;
      }

      case VERIFY_ERROR_RUNTIME_THROW: {
        LOG(FATAL) << "UNREACHABLE";
      }
    }
  } else if (kIsDebugBuild) {
    CHECK_NE(error, VERIFY_ERROR_BAD_CLASS_HARD);
  }

  failures_.push_back(error);
  std::string location(StringPrintf("%s: [0x%X] ", dex_file_->PrettyMethod(dex_method_idx_).c_str(),
                                    work_insn_idx_));
  std::ostringstream* failure_message = new std::ostringstream(location, std::ostringstream::ate);
  failure_messages_.push_back(failure_message);
  return *failure_message;
}

ScopedNewLine MethodVerifier::LogVerifyInfo() {
  ScopedNewLine ret{InfoMessages()};
  ret << "VFY: " << dex_file_->PrettyMethod(dex_method_idx_)
      << '[' << reinterpret_cast<void*>(work_insn_idx_) << "] : ";
  return ret;
}

static FailureKind FailureKindMax(FailureKind fk1, FailureKind fk2) {
  static_assert(FailureKind::kNoFailure < FailureKind::kSoftFailure
                    && FailureKind::kSoftFailure < FailureKind::kHardFailure,
                "Unexpected FailureKind order");
  return std::max(fk1, fk2);
}

void MethodVerifier::FailureData::Merge(const MethodVerifier::FailureData& fd) {
  kind = FailureKindMax(kind, fd.kind);
  types |= fd.types;
}

const RegType& MethodVerifier::GetInvocationThis(const Instruction* inst) {
  DCHECK(inst->IsInvoke());
  const size_t args_count = inst->VRegA();
  if (args_count < 1) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invoke lacks 'this'";
    return reg_types_.Conflict();
  }
  const uint32_t this_reg = inst->VRegC();
  const RegType& this_type = work_line_->GetRegisterType(this, this_reg);
  if (!this_type.IsReferenceTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "tried to get class from non-reference register v" << this_reg
        << " (type=" << this_type << ")";
    return reg_types_.Conflict();
  }
  return this_type;
}

bool MethodVerifier::AssignableFrom(const RegType& lhs, const RegType& rhs, bool strict) const {
  if (lhs.Equals(rhs)) {
    return true;
  }

  RegType::Assignability assignable = RegType::AssignabilityFrom(lhs.GetKind(), rhs.GetKind());
  DCHECK(assignable != RegType::Assignability::kInvalid)
      << "Unexpected register type in IsAssignableFrom: '" << lhs << "' := '" << rhs << "'";
  if (assignable == RegType::Assignability::kAssignable) {
    return true;
  } else if (assignable == RegType::Assignability::kNotAssignable) {
    return false;
  } else if (assignable == RegType::Assignability::kNarrowingConversion) {
    // FIXME: The `MethodVerifier` is mostly doing a category check and avoiding
    // assignability checks that would expose narrowing conversions. However, for
    // the `return` instruction, it explicitly allows certain narrowing conversions
    // and prohibits others by doing a modified assignability check. Without strict
    // enforcement in all cases, this can compromise compiler optimizations that
    // rely on knowing the range of the values. Bug: 270660613
    return false;
  } else {
    DCHECK(assignable == RegType::Assignability::kReference);
    DCHECK(lhs.IsNonZeroReferenceTypes());
    DCHECK(rhs.IsNonZeroReferenceTypes());
    DCHECK(!lhs.IsUninitializedTypes());
    DCHECK(!rhs.IsUninitializedTypes());
    DCHECK(!lhs.IsJavaLangObject());
    if (!strict && !lhs.IsUnresolvedTypes() && lhs.GetClass()->IsInterface()) {
      // If we're not strict allow assignment to any interface, see comment in ClassJoin.
      return true;
    } else if (lhs.IsJavaLangObjectArray()) {
      return rhs.IsObjectArrayTypes();  // All reference arrays may be assigned to Object[]
    } else if (lhs.HasClass() && rhs.IsJavaLangObject()) {
      return false;  // Note: Non-strict check for interface `lhs` is handled above.
    } else if (lhs.HasClass() && rhs.HasClass()) {
      // Test assignability from the Class point-of-view.
      bool result = lhs.GetClass()->IsAssignableFrom(rhs.GetClass());
      // Record assignability dependency. The `verifier` is null during unit tests and
      // VerifiedMethod::GenerateSafeCastSet.
      if (result) {
        VerifierDeps::MaybeRecordAssignability(GetVerifierDeps(),
                                               GetDexFile(),
                                               GetClassDef(),
                                               lhs.GetClass(),
                                               rhs.GetClass());
      }
      return result;
    } else {
      // For unresolved types, we don't know if they are assignable, and the
      // verifier will continue assuming they are. We need to record that.
      //
      // Note that if `rhs` is an interface type, `lhs` may be j.l.Object
      // and if the assignability check is not strict, then this should be
      // OK. However we don't encode strictness in the verifier deps, and
      // such a situation will force a full verification.
      VerifierDeps::MaybeRecordAssignability(GetVerifierDeps(),
                                             GetDexFile(),
                                             GetClassDef(),
                                             lhs,
                                             rhs);
      // Unresolved types are only assignable for null and equality.
      // Null cannot be the left-hand side.
      return false;
    }
  }
}

inline bool MethodVerifier::IsAssignableFrom(const RegType& lhs, const RegType& rhs) const {
  return AssignableFrom(lhs, rhs, false);
}

inline bool MethodVerifier::IsStrictlyAssignableFrom(const RegType& lhs, const RegType& rhs) const {
  return AssignableFrom(lhs, rhs, true);
}

}  // namespace verifier
}  // namespace art
