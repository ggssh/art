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

#ifndef ART_COMPILER_LINKER_LINKER_PATCH_H_
#define ART_COMPILER_LINKER_LINKER_PATCH_H_

#include <iosfwd>
#include <stdint.h>

#include <android-base/logging.h>

#include "base/bit_utils.h"
#include "base/macros.h"
#include "dex/method_reference.h"
#include "dex/proto_reference.h"
#include "dex/string_reference.h"
#include "dex/type_reference.h"

namespace art HIDDEN {

class DexFile;

namespace linker {

class LinkerPatch {
 public:
  // Note: We explicitly specify the underlying type of the enum because GCC
  // would otherwise select a bigger underlying type and then complain that
  //     'art::LinkerPatch::patch_type_' is too small to hold all
  //     values of 'enum class art::LinkerPatch::Type'
  // which is ridiculous given we have only a handful of values here. If we
  // choose to squeeze the Type into fewer than 8 bits, we'll have to declare
  // patch_type_ as an uintN_t and do explicit static_cast<>s.
  //
  // Note: Actual patching is instruction_set-dependent.
  enum class Type : uint8_t {
    kIntrinsicReference,      // Boot image reference for an intrinsic, see IntrinsicObjects.
    kBootImageRelRo,
    kMethodRelative,
    kMethodAppImageRelRo,
    kMethodBssEntry,
    kJniEntrypointRelative,
    kCallRelative,
    kTypeRelative,
    kTypeAppImageRelRo,
    kTypeBssEntry,
    kPublicTypeBssEntry,
    kPackageTypeBssEntry,
    kStringRelative,
    kStringBssEntry,
    kMethodTypeBssEntry,
    kCallEntrypoint,
    kBakerReadBarrierBranch,
  };

  static LinkerPatch IntrinsicReferencePatch(size_t literal_offset,
                                             uint32_t pc_insn_offset,
                                             uint32_t intrinsic_data) {
    LinkerPatch patch(literal_offset, Type::kIntrinsicReference, /* target_dex_file= */ nullptr);
    patch.intrinsic_data_ = intrinsic_data;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch BootImageRelRoPatch(size_t literal_offset,
                                         uint32_t pc_insn_offset,
                                         uint32_t boot_image_offset) {
    LinkerPatch patch(literal_offset, Type::kBootImageRelRo, /* target_dex_file= */ nullptr);
    patch.boot_image_offset_ = boot_image_offset;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch RelativeMethodPatch(size_t literal_offset,
                                         const DexFile* target_dex_file,
                                         uint32_t pc_insn_offset,
                                         uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kMethodRelative, target_dex_file);
    patch.method_idx_ = target_method_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch MethodAppImageRelRoPatch(size_t literal_offset,
                                              const DexFile* target_dex_file,
                                              uint32_t pc_insn_offset,
                                              uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kMethodAppImageRelRo, target_dex_file);
    patch.method_idx_ = target_method_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch MethodBssEntryPatch(size_t literal_offset,
                                         const DexFile* target_dex_file,
                                         uint32_t pc_insn_offset,
                                         uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kMethodBssEntry, target_dex_file);
    patch.method_idx_ = target_method_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch RelativeJniEntrypointPatch(size_t literal_offset,
                                                const DexFile* target_dex_file,
                                                uint32_t pc_insn_offset,
                                                uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kJniEntrypointRelative, target_dex_file);
    patch.method_idx_ = target_method_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch RelativeCodePatch(size_t literal_offset,
                                       const DexFile* target_dex_file,
                                       uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kCallRelative, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch RelativeTypePatch(size_t literal_offset,
                                       const DexFile* target_dex_file,
                                       uint32_t pc_insn_offset,
                                       uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, Type::kTypeRelative, target_dex_file);
    patch.type_idx_ = target_type_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch TypeAppImageRelRoPatch(size_t literal_offset,
                                            const DexFile* target_dex_file,
                                            uint32_t pc_insn_offset,
                                            uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, Type::kTypeAppImageRelRo, target_dex_file);
    patch.type_idx_ = target_type_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch TypeBssEntryPatch(size_t literal_offset,
                                       const DexFile* target_dex_file,
                                       uint32_t pc_insn_offset,
                                       uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, Type::kTypeBssEntry, target_dex_file);
    patch.type_idx_ = target_type_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch PublicTypeBssEntryPatch(size_t literal_offset,
                                             const DexFile* target_dex_file,
                                             uint32_t pc_insn_offset,
                                             uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, Type::kPublicTypeBssEntry, target_dex_file);
    patch.type_idx_ = target_type_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch PackageTypeBssEntryPatch(size_t literal_offset,
                                              const DexFile* target_dex_file,
                                              uint32_t pc_insn_offset,
                                              uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, Type::kPackageTypeBssEntry, target_dex_file);
    patch.type_idx_ = target_type_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch RelativeStringPatch(size_t literal_offset,
                                         const DexFile* target_dex_file,
                                         uint32_t pc_insn_offset,
                                         uint32_t target_string_idx) {
    LinkerPatch patch(literal_offset, Type::kStringRelative, target_dex_file);
    patch.string_idx_ = target_string_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch StringBssEntryPatch(size_t literal_offset,
                                         const DexFile* target_dex_file,
                                         uint32_t pc_insn_offset,
                                         uint32_t target_string_idx) {
    LinkerPatch patch(literal_offset, Type::kStringBssEntry, target_dex_file);
    patch.string_idx_ = target_string_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch MethodTypeBssEntryPatch(size_t literal_offset,
                                             const DexFile* target_dex_file,
                                             uint32_t pc_insn_offset,
                                             uint32_t target_proto_idx) {
    LinkerPatch patch(literal_offset, Type::kMethodTypeBssEntry, target_dex_file);
    patch.proto_idx_ = target_proto_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch CallEntrypointPatch(size_t literal_offset,
                                         uint32_t entrypoint_offset) {
    LinkerPatch patch(literal_offset,
                      Type::kCallEntrypoint,
                      /* target_dex_file= */ nullptr);
    patch.entrypoint_offset_ = entrypoint_offset;
    return patch;
  }

  static LinkerPatch BakerReadBarrierBranchPatch(size_t literal_offset,
                                                 uint32_t custom_value1 = 0u,
                                                 uint32_t custom_value2 = 0u) {
    LinkerPatch patch(literal_offset,
                      Type::kBakerReadBarrierBranch,
                      /* target_dex_file= */ nullptr);
    patch.baker_custom_value1_ = custom_value1;
    patch.baker_custom_value2_ = custom_value2;
    return patch;
  }

  LinkerPatch(const LinkerPatch& other) = default;
  LinkerPatch& operator=(const LinkerPatch& other) = default;

  size_t LiteralOffset() const {
    return literal_offset_;
  }

  Type GetType() const {
    return patch_type_;
  }

  uint32_t IntrinsicData() const {
    DCHECK(patch_type_ == Type::kIntrinsicReference);
    return intrinsic_data_;
  }

  uint32_t BootImageOffset() const {
    DCHECK(patch_type_ == Type::kBootImageRelRo);
    return boot_image_offset_;
  }

  MethodReference TargetMethod() const {
    DCHECK(patch_type_ == Type::kMethodRelative ||
           patch_type_ == Type::kMethodAppImageRelRo ||
           patch_type_ == Type::kMethodBssEntry ||
           patch_type_ == Type::kJniEntrypointRelative ||
           patch_type_ == Type::kCallRelative);
    return MethodReference(target_dex_file_, method_idx_);
  }

  TypeReference TargetType() const {
    DCHECK(patch_type_ == Type::kTypeRelative ||
           patch_type_ == Type::kTypeAppImageRelRo ||
           patch_type_ == Type::kTypeBssEntry ||
           patch_type_ == Type::kPublicTypeBssEntry ||
           patch_type_ == Type::kPackageTypeBssEntry);
    return TypeReference(target_dex_file_, dex::TypeIndex(type_idx_));
  }

  StringReference TargetString() const {
    DCHECK(patch_type_ == Type::kStringRelative ||
           patch_type_ == Type::kStringBssEntry);
    return StringReference(target_dex_file_, dex::StringIndex(string_idx_));
  }

  ProtoReference TargetProto() const {
    DCHECK(patch_type_ == Type::kMethodTypeBssEntry);
    return ProtoReference(target_dex_file_, dex::ProtoIndex(proto_idx_));
  }

  uint32_t PcInsnOffset() const {
    DCHECK(patch_type_ == Type::kIntrinsicReference ||
           patch_type_ == Type::kBootImageRelRo ||
           patch_type_ == Type::kMethodRelative ||
           patch_type_ == Type::kMethodAppImageRelRo ||
           patch_type_ == Type::kMethodBssEntry ||
           patch_type_ == Type::kJniEntrypointRelative ||
           patch_type_ == Type::kTypeRelative ||
           patch_type_ == Type::kTypeAppImageRelRo ||
           patch_type_ == Type::kTypeBssEntry ||
           patch_type_ == Type::kPublicTypeBssEntry ||
           patch_type_ == Type::kPackageTypeBssEntry ||
           patch_type_ == Type::kStringRelative ||
           patch_type_ == Type::kStringBssEntry ||
           patch_type_ == Type::kMethodTypeBssEntry);
    return pc_insn_offset_;
  }

  uint32_t EntrypointOffset() const {
    DCHECK(patch_type_ == Type::kCallEntrypoint);
    return entrypoint_offset_;
  }

  uint32_t GetBakerCustomValue1() const {
    DCHECK(patch_type_ == Type::kBakerReadBarrierBranch);
    return baker_custom_value1_;
  }

  uint32_t GetBakerCustomValue2() const {
    DCHECK(patch_type_ == Type::kBakerReadBarrierBranch);
    return baker_custom_value2_;
  }

 private:
  LinkerPatch(size_t literal_offset, Type patch_type, const DexFile* target_dex_file)
      : target_dex_file_(target_dex_file),
        literal_offset_(literal_offset),
        patch_type_(patch_type) {
    cmp1_ = 0u;
    cmp2_ = 0u;
    // The compiler rejects methods that are too big, so the compiled code
    // of a single method really shouln't be anywhere close to 16MiB.
    DCHECK(IsUint<24>(literal_offset));
  }

  const DexFile* target_dex_file_;
  // TODO: Clean up naming. Some patched locations are literals but others are not.
  uint32_t literal_offset_ : 24;  // Method code size up to 16MiB.
  Type patch_type_ : 8;
  union {
    uint32_t cmp1_;               // Used for relational operators.
    uint32_t boot_image_offset_;  // Data to write to the boot image .data.img.rel.ro entry.
    uint32_t method_idx_;         // Method index for Call/Method patches.
    uint32_t type_idx_;           // Type index for Type patches.
    uint32_t string_idx_;         // String index for String patches.
    uint32_t proto_idx_;          // Proto index for MethodType patches.
    uint32_t intrinsic_data_;     // Data for IntrinsicObjects.
    uint32_t entrypoint_offset_;  // Entrypoint offset in the Thread object.
    uint32_t baker_custom_value1_;
    static_assert(sizeof(method_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(type_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(string_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(proto_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(intrinsic_data_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(baker_custom_value1_) == sizeof(cmp1_), "needed by relational operators");
  };
  union {
    // Note: To avoid uninitialized padding on 64-bit systems, we use `size_t` for `cmp2_`.
    // This allows a hashing function to treat an array of linker patches as raw memory.
    size_t cmp2_;             // Used for relational operators.
    // Literal offset of the insn loading PC (same as literal_offset if it's the same insn,
    // may be different if the PC-relative addressing needs multiple insns).
    uint32_t pc_insn_offset_;
    uint32_t baker_custom_value2_;
    static_assert(sizeof(pc_insn_offset_) <= sizeof(cmp2_), "needed by relational operators");
    static_assert(sizeof(baker_custom_value2_) <= sizeof(cmp2_), "needed by relational operators");
  };

  friend bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs);
  friend bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs);
};
EXPORT std::ostream& operator<<(std::ostream& os, LinkerPatch::Type type);

inline bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs) {
  return lhs.literal_offset_ == rhs.literal_offset_ &&
      lhs.patch_type_ == rhs.patch_type_ &&
      lhs.target_dex_file_ == rhs.target_dex_file_ &&
      lhs.cmp1_ == rhs.cmp1_ &&
      lhs.cmp2_ == rhs.cmp2_;
}

inline bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs) {
  return (lhs.literal_offset_ != rhs.literal_offset_) ? lhs.literal_offset_ < rhs.literal_offset_
      : (lhs.patch_type_ != rhs.patch_type_) ? lhs.patch_type_ < rhs.patch_type_
      : (lhs.target_dex_file_ != rhs.target_dex_file_) ? lhs.target_dex_file_ < rhs.target_dex_file_
      : (lhs.cmp1_ != rhs.cmp1_) ? lhs.cmp1_ < rhs.cmp1_
      : lhs.cmp2_ < rhs.cmp2_;
}

}  // namespace linker
}  // namespace art

#endif  // ART_COMPILER_LINKER_LINKER_PATCH_H_
