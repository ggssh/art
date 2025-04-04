/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "unstarted_runtime_test.h"

#include <limits>
#include <locale>

#include "base/casts.h"
#include "base/memory_tool.h"
#include "base/pointer_size.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "common_runtime_test.h"
#include "common_throws.h"
#include "dex/descriptors_names.h"
#include "dex/dex_instruction.h"
#include "handle.h"
#include "handle_scope-inl.h"
#include "interpreter_common.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "runtime.h"
#include "runtime_intrinsics.h"
#include "scoped_thread_state_change-inl.h"
#include "shadow_frame-inl.h"
#include "thread.h"
#include "unstarted_runtime_list.h"

namespace art HIDDEN {
namespace interpreter {

class UnstartedRuntimeTest : public UnstartedRuntimeTestBase {
 protected:
  // Helpers for ArrayCopy.
  //
  // Note: as we have to use handles, we use StackHandleScope to transfer data. Hardcode a size
  //       of three everywhere. That is enough to test all cases.

  static ObjPtr<mirror::ObjectArray<mirror::Object>> CreateObjectArray(
      Thread* self,
      ObjPtr<mirror::Class> component_type,
      const StackHandleScope<3>& data)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    ObjPtr<mirror::Class> array_type =
        runtime->GetClassLinker()->FindArrayClass(self, component_type);
    CHECK(array_type != nullptr);
    ObjPtr<mirror::ObjectArray<mirror::Object>> result =
        mirror::ObjectArray<mirror::Object>::Alloc(self, array_type, 3);
    CHECK(result != nullptr);
    for (size_t i = 0; i < 3; ++i) {
      result->Set(static_cast<int32_t>(i), data.GetReference(i));
      CHECK(!self->IsExceptionPending());
    }
    return result;
  }

  static void CheckObjectArray(ObjPtr<mirror::ObjectArray<mirror::Object>> array,
                               const StackHandleScope<3>& data)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    CHECK_EQ(array->GetLength(), 3);
    CHECK_EQ(data.Size(), 3U);
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_OBJ_PTR_EQ(data.GetReference(i), array->Get(static_cast<int32_t>(i))) << i;
    }
  }

  void RunArrayCopy(Thread* self,
                    ShadowFrame* tmp,
                    bool expect_exception,
                    ObjPtr<mirror::ObjectArray<mirror::Object>> src,
                    int32_t src_pos,
                    ObjPtr<mirror::ObjectArray<mirror::Object>> dst,
                    int32_t dst_pos,
                    int32_t length)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    JValue result;
    tmp->SetVRegReference(0, src);
    tmp->SetVReg(1, src_pos);
    tmp->SetVRegReference(2, dst);
    tmp->SetVReg(3, dst_pos);
    tmp->SetVReg(4, length);
    UnstartedSystemArraycopy(self, tmp, &result, 0);
    bool exception_pending = self->IsExceptionPending();
    EXPECT_EQ(exception_pending, expect_exception);
    if (exception_pending) {
      self->ClearException();
    }
  }

  void RunArrayCopy(Thread* self,
                    ShadowFrame* tmp,
                    bool expect_exception,
                    ObjPtr<mirror::Class> src_component_class,
                    ObjPtr<mirror::Class> dst_component_class,
                    const StackHandleScope<3>& src_data,
                    int32_t src_pos,
                    const StackHandleScope<3>& dst_data,
                    int32_t dst_pos,
                    int32_t length,
                    const StackHandleScope<3>& expected_result)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    StackHandleScope<3> hs_misc(self);
    Handle<mirror::Class> dst_component_handle(hs_misc.NewHandle(dst_component_class));

    Handle<mirror::ObjectArray<mirror::Object>> src_handle(
        hs_misc.NewHandle(CreateObjectArray(self, src_component_class, src_data)));

    Handle<mirror::ObjectArray<mirror::Object>> dst_handle(
        hs_misc.NewHandle(CreateObjectArray(self, dst_component_handle.Get(), dst_data)));

    RunArrayCopy(self,
                 tmp,
                 expect_exception,
                 src_handle.Get(),
                 src_pos,
                 dst_handle.Get(),
                 dst_pos,
                 length);
    CheckObjectArray(dst_handle.Get(), expected_result);
  }

  void TestCeilFloor(bool ceil,
                     Thread* self,
                     ShadowFrame* tmp,
                     double const test_pairs[][2],
                     size_t num_pairs)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    for (size_t i = 0; i < num_pairs; ++i) {
      tmp->SetVRegDouble(0, test_pairs[i][0]);

      JValue result;
      if (ceil) {
        UnstartedMathCeil(self, tmp, &result, 0);
      } else {
        UnstartedMathFloor(self, tmp, &result, 0);
      }

      ASSERT_FALSE(self->IsExceptionPending());

      // We want precise results.
      int64_t result_int64t = bit_cast<int64_t, double>(result.GetD());
      int64_t expect_int64t = bit_cast<int64_t, double>(test_pairs[i][1]);
      EXPECT_EQ(expect_int64t, result_int64t) << result.GetD() << " vs " << test_pairs[i][1];
    }
  }
};

TEST_F(UnstartedRuntimeTest, MemoryPeekByte) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  for (int32_t i = 0; i < kBaseLen; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekByte(self, tmp.get(), &result, 0);

    EXPECT_EQ(result.GetB(), static_cast<int8_t>(base_array[i]));
  }
}

TEST_F(UnstartedRuntimeTest, MemoryPeekShort) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int16_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekShort(self, tmp.get(), &result, 0);

    using unaligned_short __attribute__((__aligned__(1))) = int16_t;
    const unaligned_short* short_ptr = reinterpret_cast<const unaligned_short*>(base_ptr + i);
    EXPECT_EQ(result.GetS(), *short_ptr);
  }
}

TEST_F(UnstartedRuntimeTest, MemoryPeekInt) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int32_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekInt(self, tmp.get(), &result, 0);

    using unaligned_int __attribute__((__aligned__(1))) = int32_t;
    const unaligned_int* int_ptr = reinterpret_cast<const unaligned_int*>(base_ptr + i);
    EXPECT_EQ(result.GetI(), *int_ptr);
  }
}

TEST_F(UnstartedRuntimeTest, MemoryPeekLong) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int64_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekLong(self, tmp.get(), &result, 0);

    using unaligned_long __attribute__((__aligned__(1))) = int64_t;
    const unaligned_long* long_ptr = reinterpret_cast<const unaligned_long*>(base_ptr + i);
    EXPECT_EQ(result.GetJ(), *long_ptr);
  }
}

TEST_F(UnstartedRuntimeTest, StringGetCharsNoCheck) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  StackHandleScope<2> hs(self);
  // TODO: Actual UTF.
  constexpr const char base_string[] = "abcdefghijklmnop";
  Handle<mirror::String> h_test_string(hs.NewHandle(
      mirror::String::AllocFromModifiedUtf8(self, base_string)));
  constexpr int32_t kBaseLen = sizeof(base_string) / sizeof(char) - 1;
  Handle<mirror::CharArray> h_char_array(hs.NewHandle(
      mirror::CharArray::Alloc(self, kBaseLen)));
  // A buffer so we can make sure we only modify the elements targetted.
  uint16_t buf[kBaseLen];

  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  for (int32_t start_index = 0; start_index < kBaseLen; ++start_index) {
    for (int32_t count = 0; count <= kBaseLen; ++count) {
      for (int32_t trg_offset = 0; trg_offset < kBaseLen; ++trg_offset) {
        // Only do it when in bounds.
        if (start_index + count <= kBaseLen && trg_offset + count <= kBaseLen) {
          tmp->SetVRegReference(0, h_test_string.Get());
          tmp->SetVReg(1, start_index);
          tmp->SetVReg(2, count);
          tmp->SetVRegReference(3, h_char_array.Get());
          tmp->SetVReg(3, trg_offset);

          // Copy the char_array into buf.
          memcpy(buf, h_char_array->GetData(), kBaseLen * sizeof(uint16_t));

          UnstartedStringCharAt(self, tmp.get(), &result, 0);

          uint16_t* data = h_char_array->GetData();

          bool success = true;

          // First segment should be unchanged.
          for (int32_t i = 0; i < trg_offset; ++i) {
            success = success && (data[i] == buf[i]);
          }
          // Second segment should be a copy.
          for (int32_t i = trg_offset; i < trg_offset + count; ++i) {
            success = success && (data[i] == buf[i - trg_offset + start_index]);
          }
          // Third segment should be unchanged.
          for (int32_t i = trg_offset + count; i < kBaseLen; ++i) {
            success = success && (data[i] == buf[i]);
          }

          EXPECT_TRUE(success);
        }
      }
    }
  }
}

TEST_F(UnstartedRuntimeTest, StringCharAt) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  // TODO: Actual UTF.
  constexpr const char* base_string = "abcdefghijklmnop";
  int32_t base_len = static_cast<int32_t>(strlen(base_string));
  ObjPtr<mirror::String> test_string = mirror::String::AllocFromModifiedUtf8(self, base_string);

  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  for (int32_t i = 0; i < base_len; ++i) {
    tmp->SetVRegReference(0, test_string);
    tmp->SetVReg(1, i);

    UnstartedStringCharAt(self, tmp.get(), &result, 0);

    EXPECT_EQ(result.GetI(), base_string[i]);
  }
}

TEST_F(UnstartedRuntimeTest, StringInit) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  ObjPtr<mirror::Class> klass = GetClassRoot<mirror::String>();
  ArtMethod* method =
      klass->FindConstructor("(Ljava/lang/String;)V", class_linker_->GetImagePointerSize());
  ASSERT_TRUE(method != nullptr);

  // create instruction data for invoke-direct {v0, v1} of method with fake index
  uint16_t inst_data[3] = { 0x2070, 0x0000, 0x0010 };

  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, method, 0);
  const char* base_string = "hello_world";
  StackHandleScope<2> hs(self);
  Handle<mirror::String> string_arg =
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, base_string));
  Handle<mirror::String> reference_empty_string =
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, ""));
  shadow_frame->SetVRegReference(0, reference_empty_string.Get());
  shadow_frame->SetVRegReference(1, string_arg.Get());

  ArtMethod* factory = WellKnownClasses::StringInitToStringFactory(method);
  interpreter::DoCall<false>(factory,
                             self,
                             *shadow_frame,
                             Instruction::At(inst_data),
                             inst_data[0],
                             /* string_init= */ true,
                             &result);
  ObjPtr<mirror::String> string_result = down_cast<mirror::String*>(result.GetL());
  EXPECT_EQ(string_arg->GetLength(), string_result->GetLength());

  if (string_arg->IsCompressed() && string_result->IsCompressed()) {
    EXPECT_EQ(memcmp(string_arg->GetValueCompressed(), string_result->GetValueCompressed(),
                     string_arg->GetLength() * sizeof(uint8_t)), 0);
  } else if (!string_arg->IsCompressed() && !string_result->IsCompressed()) {
    EXPECT_EQ(memcmp(string_arg->GetValue(), string_result->GetValue(),
                     string_arg->GetLength() * sizeof(uint16_t)), 0);
  } else {
    bool equal = true;
    for (int i = 0; i < string_arg->GetLength(); ++i) {
      if (string_arg->CharAt(i) != string_result->CharAt(i)) {
        equal = false;
        break;
      }
    }
    EXPECT_EQ(equal, true);
  }
}

// Tests the exceptions that should be checked before modifying the destination.
// (Doesn't check the object vs primitive case ATM.)
TEST_F(UnstartedRuntimeTest, SystemArrayCopyObjectArrayTestExceptions) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Note: all tests are not GC safe. Assume there's no GC running here with the few objects we
  //       allocate.
  StackHandleScope<3> hs_misc(self);
  Handle<mirror::Class> object_class(hs_misc.NewHandle(GetClassRoot<mirror::Object>()));

  StackHandleScope<3> hs_data(self);
  hs_data.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
  hs_data.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "2"));
  hs_data.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

  Handle<mirror::ObjectArray<mirror::Object>> array(
      hs_misc.NewHandle(CreateObjectArray(self, object_class.Get(), hs_data)));

  RunArrayCopy(self, tmp.get(), true, array.Get(), -1, array.Get(), 0, 0);
  RunArrayCopy(self, tmp.get(), true, array.Get(), 0, array.Get(), -1, 0);
  RunArrayCopy(self, tmp.get(), true, array.Get(), 0, array.Get(), 0, -1);
  RunArrayCopy(self, tmp.get(), true, array.Get(), 0, array.Get(), 0, 4);
  RunArrayCopy(self, tmp.get(), true, array.Get(), 0, array.Get(), 1, 3);
  RunArrayCopy(self, tmp.get(), true, array.Get(), 1, array.Get(), 0, 3);

  Handle<mirror::ObjectArray<mirror::Object>> class_as_array =
      hs_misc.NewHandle(reinterpret_cast<mirror::ObjectArray<mirror::Object>*>(object_class.Get()));
  RunArrayCopy(self, tmp.get(), true, class_as_array.Get(), 0, array.Get(), 0, 0);
  RunArrayCopy(self, tmp.get(), true, array.Get(), 0, class_as_array.Get(), 0, 0);
}

TEST_F(UnstartedRuntimeTest, SystemArrayCopyObjectArrayTest) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  JValue result;
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Simple test:
  // [1,2,3]{1 @ 2} into [4,5,6] = [4,2,6]
  {
    StackHandleScope<3> hs_src(self);
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "2"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

    StackHandleScope<3> hs_dst(self);
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "4"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "5"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "6"));

    StackHandleScope<3> hs_expected(self);
    hs_expected.NewHandle(hs_dst.GetReference(0));
    hs_expected.NewHandle(hs_dst.GetReference(1));
    hs_expected.NewHandle(hs_src.GetReference(1));

    RunArrayCopy(self,
                 tmp.get(),
                 false,
                 GetClassRoot<mirror::Object>(),
                 GetClassRoot<mirror::Object>(),
                 hs_src,
                 1,
                 hs_dst,
                 2,
                 1,
                 hs_expected);
  }

  // Simple test:
  // [1,2,3]{1 @ 1} into [4,5,6] = [4,2,6]  (with dst String[])
  {
    StackHandleScope<3> hs_src(self);
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "2"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

    StackHandleScope<3> hs_dst(self);
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "4"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "5"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "6"));

    StackHandleScope<3> hs_expected(self);
    hs_expected.NewHandle(hs_dst.GetReference(0));
    hs_expected.NewHandle(hs_src.GetReference(1));
    hs_expected.NewHandle(hs_dst.GetReference(2));

    RunArrayCopy(self,
                 tmp.get(),
                 false,
                 GetClassRoot<mirror::Object>(),
                 GetClassRoot<mirror::String>(),
                 hs_src,
                 1,
                 hs_dst,
                 1,
                 1,
                 hs_expected);
  }

  // Simple test:
  // [1,*,3] into [4,5,6] = [1,5,6] + exc
  {
    StackHandleScope<3> hs_src(self);
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
    hs_src.NewHandle(GetClassRoot<mirror::String>());
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

    StackHandleScope<3> hs_dst(self);
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "4"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "5"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "6"));

    StackHandleScope<3> hs_expected(self);
    hs_expected.NewHandle(hs_src.GetReference(0));
    hs_expected.NewHandle(hs_dst.GetReference(1));
    hs_expected.NewHandle(hs_dst.GetReference(2));

    RunArrayCopy(self,
                 tmp.get(),
                 true,
                 GetClassRoot<mirror::Object>(),
                 GetClassRoot<mirror::String>(),
                 hs_src,
                 0,
                 hs_dst,
                 0,
                 3,
                 hs_expected);
  }
}

TEST_F(UnstartedRuntimeTest, IntegerParseIntTest) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Test string. Should be valid, and between minimal values of LONG_MIN and LONG_MAX (for all
  // suffixes).
  constexpr const char* test_string = "-2147483646";
  constexpr int32_t test_values[] = {
                6,
               46,
              646,
             3646,
            83646,
           483646,
          7483646,
         47483646,
        147483646,
       2147483646,
      -2147483646
  };

  static_assert(arraysize(test_values) == 11U, "test_values");
  CHECK_EQ(strlen(test_string), 11U);

  for (size_t i = 0; i <= 10; ++i) {
    const char* test_value = &test_string[10 - i];

    StackHandleScope<1> hs_str(self);
    Handle<mirror::String> h_str(
        hs_str.NewHandle(mirror::String::AllocFromModifiedUtf8(self, test_value)));
    ASSERT_NE(h_str.Get(), nullptr);
    ASSERT_FALSE(self->IsExceptionPending());

    tmp->SetVRegReference(0, h_str.Get());

    JValue result;
    UnstartedIntegerParseInt(self, tmp.get(), &result, 0);

    ASSERT_FALSE(self->IsExceptionPending());
    EXPECT_EQ(result.GetI(), test_values[i]);
  }
}

// Right now the same as Integer.Parse
TEST_F(UnstartedRuntimeTest, LongParseLongTest) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Test string. Should be valid, and between minimal values of LONG_MIN and LONG_MAX (for all
  // suffixes).
  constexpr const char* test_string = "-2147483646";
  constexpr int64_t test_values[] = {
                6,
               46,
              646,
             3646,
            83646,
           483646,
          7483646,
         47483646,
        147483646,
       2147483646,
      -2147483646
  };

  static_assert(arraysize(test_values) == 11U, "test_values");
  CHECK_EQ(strlen(test_string), 11U);

  for (size_t i = 0; i <= 10; ++i) {
    const char* test_value = &test_string[10 - i];

    StackHandleScope<1> hs_str(self);
    Handle<mirror::String> h_str(
        hs_str.NewHandle(mirror::String::AllocFromModifiedUtf8(self, test_value)));
    ASSERT_NE(h_str.Get(), nullptr);
    ASSERT_FALSE(self->IsExceptionPending());

    tmp->SetVRegReference(0, h_str.Get());

    JValue result;
    UnstartedLongParseLong(self, tmp.get(), &result, 0);

    ASSERT_FALSE(self->IsExceptionPending());
    EXPECT_EQ(result.GetJ(), test_values[i]);
  }
}

TEST_F(UnstartedRuntimeTest, Ceil) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double ld1 = static_cast<double>((UINT64_C(1) << 53) - 1);
  constexpr double ld2 = static_cast<double>(UINT64_C(1) << 55);
  constexpr double test_pairs[][2] = {
      { -0.0, -0.0 },
      {  0.0,  0.0 },
      { -0.5, -0.0 },
      { -1.0, -1.0 },
      {  0.5,  1.0 },
      {  1.0,  1.0 },
      {  nan,  nan },
      {  inf,  inf },
      { -inf, -inf },
      {  ld1,  ld1 },
      {  ld2,  ld2 }
  };

  TestCeilFloor(/* ceil= */ true, self, tmp.get(), test_pairs, arraysize(test_pairs));
}

TEST_F(UnstartedRuntimeTest, Floor) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double ld1 = static_cast<double>((UINT64_C(1) << 53) - 1);
  constexpr double ld2 = static_cast<double>(UINT64_C(1) << 55);
  constexpr double test_pairs[][2] = {
      { -0.0, -0.0 },
      {  0.0,  0.0 },
      { -0.5, -1.0 },
      { -1.0, -1.0 },
      {  0.5,  0.0 },
      {  1.0,  1.0 },
      {  nan,  nan },
      {  inf,  inf },
      { -inf, -inf },
      {  ld1,  ld1 },
      {  ld2,  ld2 }
  };

  TestCeilFloor(/* ceil= */ false, self, tmp.get(), test_pairs, arraysize(test_pairs));
}

TEST_F(UnstartedRuntimeTest, ToLowerUpper) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  std::locale c_locale("C");

  // Check ASCII.
  for (uint32_t i = 0; i < 128; ++i) {
    bool c_upper = std::isupper(static_cast<char>(i), c_locale);
    bool c_lower = std::islower(static_cast<char>(i), c_locale);
    EXPECT_FALSE(c_upper && c_lower) << i;

    // Check toLowerCase.
    {
      JValue result;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      UnstartedCharacterToLowerCase(self, tmp.get(), &result, 0);
      ASSERT_FALSE(self->IsExceptionPending());
      uint32_t lower_result = static_cast<uint32_t>(result.GetI());
      if (c_lower) {
        EXPECT_EQ(i, lower_result);
      } else if (c_upper) {
        EXPECT_EQ(static_cast<uint32_t>(std::tolower(static_cast<char>(i), c_locale)),
                  lower_result);
      } else {
        EXPECT_EQ(i, lower_result);
      }
    }

    // Check toUpperCase.
    {
      JValue result2;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      UnstartedCharacterToUpperCase(self, tmp.get(), &result2, 0);
      ASSERT_FALSE(self->IsExceptionPending());
      uint32_t upper_result = static_cast<uint32_t>(result2.GetI());
      if (c_upper) {
        EXPECT_EQ(i, upper_result);
      } else if (c_lower) {
        EXPECT_EQ(static_cast<uint32_t>(std::toupper(static_cast<char>(i), c_locale)),
                  upper_result);
      } else {
        EXPECT_EQ(i, upper_result);
      }
    }
  }
}

TEST_F(UnstartedRuntimeTest, Sin) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Test an important value, PI/6. That's the one we see in practice.
  constexpr uint64_t lvalue = UINT64_C(0x3fe0c152382d7365);
  tmp->SetVRegLong(0, static_cast<int64_t>(lvalue));

  JValue result;
  UnstartedMathSin(self, tmp.get(), &result, 0);

  const uint64_t lresult = static_cast<uint64_t>(result.GetJ());
  EXPECT_EQ(UINT64_C(0x3fdfffffffffffff), lresult);
}

TEST_F(UnstartedRuntimeTest, Cos) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Test an important value, PI/6. That's the one we see in practice.
  constexpr uint64_t lvalue = UINT64_C(0x3fe0c152382d7365);
  tmp->SetVRegLong(0, static_cast<int64_t>(lvalue));

  JValue result;
  UnstartedMathCos(self, tmp.get(), &result, 0);

  const uint64_t lresult = static_cast<uint64_t>(result.GetJ());
  EXPECT_EQ(UINT64_C(0x3febb67ae8584cab), lresult);
}

TEST_F(UnstartedRuntimeTest, Pow) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  // Test an important pair.
  constexpr uint64_t lvalue1 = UINT64_C(0x4079000000000000);
  constexpr uint64_t lvalue2 = UINT64_C(0xbfe6db6dc0000000);

  tmp->SetVRegLong(0, static_cast<int64_t>(lvalue1));
  tmp->SetVRegLong(2, static_cast<int64_t>(lvalue2));

  JValue result;
  UnstartedMathPow(self, tmp.get(), &result, 0);

  const uint64_t lresult = static_cast<uint64_t>(result.GetJ());
  EXPECT_EQ(UINT64_C(0x3f8c5c51326aa7ee), lresult);
}

TEST_F(UnstartedRuntimeTest, IsAnonymousClass) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, nullptr, 0);

  ObjPtr<mirror::Class> class_klass = GetClassRoot<mirror::Class>();
  shadow_frame->SetVRegReference(0, class_klass);
  UnstartedClassIsAnonymousClass(self, shadow_frame.get(), &result, 0);
  EXPECT_EQ(result.GetZ(), 0);

  jobject class_loader = LoadDex("Nested");
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> c = FindClass("LNested$1;", loader);
  ASSERT_TRUE(c != nullptr);
  shadow_frame->SetVRegReference(0, c);
  UnstartedClassIsAnonymousClass(self, shadow_frame.get(), &result, 0);
  EXPECT_EQ(result.GetZ(), 1);
}

TEST_F(UnstartedRuntimeTest, GetDeclaringClass) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, nullptr, 0);

  jobject class_loader = LoadDex("Nested");
  StackHandleScope<4> hs(self);
  Handle<mirror::ClassLoader> loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));

  Handle<mirror::Class> nested_klass = hs.NewHandle(FindClass("LNested;", loader));
  Handle<mirror::Class> inner_klass = hs.NewHandle(FindClass("LNested$Inner;", loader));
  Handle<mirror::Class> anon_klass = hs.NewHandle(FindClass("LNested$1;", loader));

  shadow_frame->SetVRegReference(0, nested_klass.Get());
  UnstartedClassGetDeclaringClass(self, shadow_frame.get(), &result, 0);
  EXPECT_EQ(result.GetL(), nullptr);

  shadow_frame->SetVRegReference(0, inner_klass.Get());
  UnstartedClassGetDeclaringClass(self, shadow_frame.get(), &result, 0);
  EXPECT_EQ(result.GetL(), nested_klass.Get());

  shadow_frame->SetVRegReference(0, anon_klass.Get());
  UnstartedClassGetDeclaringClass(self, shadow_frame.get(), &result, 0);
  EXPECT_EQ(result.GetL(), nullptr);
}

TEST_F(UnstartedRuntimeTest, ThreadLocalGet) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, nullptr, 0);

  StackHandleScope<1> hs(self);

  // Positive test. See that We get something for float conversion.
  {
    Handle<mirror::Class> floating_decimal = hs.NewHandle(
        FindClass("Ljdk/internal/math/FloatingDecimal;", ScopedNullHandle<mirror::ClassLoader>()));
    ASSERT_TRUE(floating_decimal != nullptr);
    ASSERT_TRUE(class_linker_->EnsureInitialized(self, floating_decimal, true, true));

    ArtMethod* caller_method = floating_decimal->FindClassMethod(
        "getBinaryToASCIIBuffer",
        "()Ljdk/internal/math/FloatingDecimal$BinaryToASCIIBuffer;",
        class_linker_->GetImagePointerSize());
    // floating_decimal->DumpClass(LOG_STREAM(ERROR), mirror::Class::kDumpClassFullDetail);
    ASSERT_TRUE(caller_method != nullptr);
    ASSERT_TRUE(caller_method->IsDirect());
    ASSERT_TRUE(caller_method->GetDeclaringClass() == floating_decimal.Get());
    UniqueDeoptShadowFramePtr caller_frame = CreateShadowFrame(10, caller_method, 0);
    shadow_frame->SetLink(caller_frame.get());

    UnstartedThreadLocalGet(self, shadow_frame.get(), &result, 0);
    EXPECT_TRUE(result.GetL() != nullptr);
    EXPECT_FALSE(self->IsExceptionPending());

    shadow_frame->ClearLink();
  }
}

TEST_F(UnstartedRuntimeTest, FloatConversion) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  StackHandleScope<1> hs(self);
  Handle<mirror::Class> double_class =
      hs.NewHandle(FindClass("Ljava/lang/Double;", ScopedNullHandle<mirror::ClassLoader>()));
  ASSERT_TRUE(double_class != nullptr);
  ASSERT_TRUE(class_linker_->EnsureInitialized(self, double_class, true, true));

  ArtMethod* method = double_class->FindClassMethod("toString",
                                                    "(D)Ljava/lang/String;",
                                                    class_linker_->GetImagePointerSize());
  ASSERT_TRUE(method != nullptr);
  ASSERT_TRUE(method->IsDirect());
  ASSERT_TRUE(method->GetDeclaringClass() == double_class.Get());

  // create instruction data for invoke-direct {v0, v1} of method with fake index
  uint16_t inst_data[3] = { 0x2070, 0x0000, 0x0010 };

  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, method, 0);

  shadow_frame->SetVRegDouble(0, 1.23);
  interpreter::DoCall<false>(method,
                             self,
                             *shadow_frame,
                             Instruction::At(inst_data),
                             inst_data[0],
                             /* string_init= */ false,
                             &result);
  ObjPtr<mirror::String> string_result = down_cast<mirror::String*>(result.GetL());
  ASSERT_TRUE(string_result != nullptr);

  std::string mod_utf = string_result->ToModifiedUtf8();
  EXPECT_EQ("1.23", mod_utf);
}

TEST_F(UnstartedRuntimeTest, LogManager) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  StackHandleScope<1> hs(self);
  Handle<mirror::Class> log_manager_class = hs.NewHandle(
      FindClass("Ljava/util/logging/LogManager;", ScopedNullHandle<mirror::ClassLoader>()));
  ASSERT_TRUE(log_manager_class.Get() != nullptr);
  ASSERT_TRUE(class_linker_->EnsureInitialized(self, log_manager_class, true, true));
}

class UnstartedClassForNameTest : public UnstartedRuntimeTest {
 public:
  template <typename T>
  void RunTest(T&& runner) {
    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);

    // Ensure that Class is initialized.
    CHECK(GetClassRoot<mirror::Class>()->IsInitialized());

    // A selection of classes from different core classpath components.
    constexpr const char* kTestCases[] = {
        "java.net.CookieManager",  // From libcore.
        "dalvik.system.ClassExt",  // From libart.
    };

    JValue result;
    UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, nullptr, 0);

    for (const char* name : kTestCases) {
      ObjPtr<mirror::String> name_string = mirror::String::AllocFromModifiedUtf8(self, name);
      CHECK(name_string != nullptr);
      CHECK(!self->IsExceptionPending());

      runner(self, shadow_frame.get(), name_string, &result);

      CHECK(!self->IsExceptionPending()) << name << " " << self->GetException()->Dump();
      CHECK(result.GetL() != nullptr) << name;
    }
  }
};

TEST_F(UnstartedClassForNameTest, ClassForName) {
  auto runner = [](Thread* self,
                   ShadowFrame* shadow_frame,
                   ObjPtr<mirror::String> name,
                   JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
    shadow_frame->SetVRegReference(0, name);
    UnstartedClassForName(self, shadow_frame, result, 0);
  };
  RunTest(runner);
}

TEST_F(UnstartedClassForNameTest, ClassForNameLong) {
  auto runner = [](Thread* self,
                   ShadowFrame* shadow_frame,
                   ObjPtr<mirror::String> name,
                   JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
    shadow_frame->SetVRegReference(0, name);
    shadow_frame->SetVReg(1, 0);
    shadow_frame->SetVRegReference(2, nullptr);
    UnstartedClassForNameLong(self, shadow_frame, result, 0);
  };
  RunTest(runner);
}

TEST_F(UnstartedClassForNameTest, ClassForNameLongWithClassLoader) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> boot_cp = hs.NewHandle(GetBootClassLoader());

  auto runner = [&](Thread* th,
                    ShadowFrame* shadow_frame,
                    ObjPtr<mirror::String> name,
                    JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
    shadow_frame->SetVRegReference(0, name);
    shadow_frame->SetVReg(1, 0);
    shadow_frame->SetVRegReference(2, boot_cp.Get());
    UnstartedClassForNameLong(th, shadow_frame, result, 0);
  };
  RunTest(runner);
}

TEST_F(UnstartedRuntimeTest, ClassGetSignatureAnnotation) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  StackHandleScope<1> hs(self);
  Handle<mirror::Class> list_class =
      hs.NewHandle(FindClass("Ljava/util/List;", ScopedNullHandle<mirror::ClassLoader>()));
  ASSERT_TRUE(list_class.Get() != nullptr);
  ASSERT_TRUE(class_linker_->EnsureInitialized(self, list_class, true, true));

  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, nullptr, 0);

  shadow_frame->SetVRegReference(0, list_class.Get());
  UnstartedClassGetSignatureAnnotation(self, shadow_frame.get(), &result, 0);
  ASSERT_TRUE(result.GetL() != nullptr);
  ASSERT_FALSE(self->IsExceptionPending());

  ASSERT_TRUE(result.GetL()->IsObjectArray());
  ObjPtr<mirror::ObjectArray<mirror::Object>> array =
      result.GetL()->AsObjectArray<mirror::Object>();
  std::ostringstream oss;
  for (int32_t i = 0; i != array->GetLength(); ++i) {
    ObjPtr<mirror::Object> elem = array->Get(i);
    ASSERT_TRUE(elem != nullptr);
    ASSERT_TRUE(elem->IsString());
    oss << elem->AsString()->ToModifiedUtf8();
  }
  std::string output_string = oss.str();
  ASSERT_EQ(output_string,
            "<E:Ljava/lang/Object;>Ljava/lang/Object;Ljava/util/SequencedCollection<TE;>;"
            "Ljava/util/Collection<TE;>;");
}

TEST_F(UnstartedRuntimeTest, ConstructorNewInstance0) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  StackHandleScope<4> hs(self);

  // Get Throwable.
  Handle<mirror::Class> throw_class = hs.NewHandle(GetClassRoot<mirror::Throwable>());
  ASSERT_TRUE(class_linker_->EnsureInitialized(self, throw_class, true, true));

  // Get an input object.
  Handle<mirror::String> input = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "abd"));

  // Find the constructor.
  PointerSize pointer_size = class_linker_->GetImagePointerSize();
  ArtMethod* throw_cons = throw_class->FindConstructor("(Ljava/lang/String;)V", pointer_size);
  ASSERT_TRUE(throw_cons != nullptr);
  Handle<mirror::Constructor> cons = hs.NewHandle((pointer_size == PointerSize::k64)
      ? mirror::Constructor::CreateFromArtMethod<PointerSize::k64>(self, throw_cons)
      : mirror::Constructor::CreateFromArtMethod<PointerSize::k32>(self, throw_cons));
  ASSERT_TRUE(cons != nullptr);

  Handle<mirror::ObjectArray<mirror::Object>> args = hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(
          self, GetClassRoot<mirror::ObjectArray<mirror::Object>>(class_linker_), 1));
  ASSERT_TRUE(args != nullptr);
  args->Set(0, input.Get());

  // OK, we're ready now.
  JValue result;
  UniqueDeoptShadowFramePtr shadow_frame = CreateShadowFrame(10, nullptr, 0);
  shadow_frame->SetVRegReference(0, cons.Get());
  shadow_frame->SetVRegReference(1, args.Get());
  UnstartedConstructorNewInstance0(self, shadow_frame.get(), &result, 0);

  ASSERT_TRUE(result.GetL() != nullptr);
  ASSERT_FALSE(self->IsExceptionPending());

  // Should be a new object.
  ASSERT_NE(result.GetL(), input.Get());
  // Should be of type Throwable.
  ASSERT_OBJ_PTR_EQ(GetClassRoot<mirror::Throwable>(), result.GetL()->GetClass());
  // Should have the right string.
  ObjPtr<mirror::String> result_msg =
      reinterpret_cast<mirror::Throwable*>(result.GetL())->GetDetailMessage();
  EXPECT_OBJ_PTR_EQ(input.Get(), result_msg);
}

TEST_F(UnstartedRuntimeTest, IdentityHashCode) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  UniqueDeoptShadowFramePtr tmp = CreateShadowFrame(10, nullptr, 0);

  JValue result;
  UnstartedSystemIdentityHashCode(self, tmp.get(), &result, 0);

  EXPECT_EQ(0, result.GetI());
  ASSERT_FALSE(self->IsExceptionPending());

  ObjPtr<mirror::String> str = mirror::String::AllocFromModifiedUtf8(self, "abd");
  tmp->SetVRegReference(0, str);
  UnstartedSystemIdentityHashCode(self, tmp.get(), &result, 0);
  EXPECT_NE(0, result.GetI());
  EXPECT_EQ(str->IdentityHashCode(), result.GetI());
  ASSERT_FALSE(self->IsExceptionPending());
}

}  // namespace interpreter
}  // namespace art
