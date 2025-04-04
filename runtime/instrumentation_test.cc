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

#include "instrumentation.h"

#include "android-base/macros.h"
#include "art_method-inl.h"
#include "base/pointer_size.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "common_throws.h"
#include "dex/dex_file.h"
#include "gc/scoped_gc_critical_section.h"
#include "handle_scope-inl.h"
#include "jni/jni_internal.h"
#include "jvalue.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "interpreter/shadow_frame.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art HIDDEN {
namespace instrumentation {

class TestInstrumentationListener final : public instrumentation::InstrumentationListener {
 public:
  TestInstrumentationListener()
    : received_method_enter_event(false),
      received_method_exit_event(false),
      received_method_exit_object_event(false),
      received_method_unwind_event(false),
      received_dex_pc_moved_event(false),
      received_field_read_event(false),
      received_field_written_event(false),
      received_field_written_object_event(false),
      received_exception_thrown_event(false),
      received_exception_handled_event(false),
      received_branch_event(false),
      received_watched_frame_pop(false) {}

  virtual ~TestInstrumentationListener() {}

  void MethodEntered([[maybe_unused]] Thread* thread, [[maybe_unused]] ArtMethod* method) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_method_enter_event = true;
  }

  void MethodExited([[maybe_unused]] Thread* thread,
                    [[maybe_unused]] ArtMethod* method,
                    [[maybe_unused]] instrumentation::OptionalFrame frame,
                    [[maybe_unused]] MutableHandle<mirror::Object>& return_value) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_method_exit_object_event = true;
  }

  void MethodExited([[maybe_unused]] Thread* thread,
                    [[maybe_unused]] ArtMethod* method,
                    [[maybe_unused]] instrumentation::OptionalFrame frame,
                    [[maybe_unused]] JValue& return_value) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_method_exit_event = true;
  }

  void MethodUnwind([[maybe_unused]] Thread* thread,
                    [[maybe_unused]] ArtMethod* method,
                    [[maybe_unused]] uint32_t dex_pc) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_method_unwind_event = true;
  }

  void DexPcMoved([[maybe_unused]] Thread* thread,
                  [[maybe_unused]] Handle<mirror::Object> this_object,
                  [[maybe_unused]] ArtMethod* method,
                  [[maybe_unused]] uint32_t new_dex_pc) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_dex_pc_moved_event = true;
  }

  void FieldRead([[maybe_unused]] Thread* thread,
                 [[maybe_unused]] Handle<mirror::Object> this_object,
                 [[maybe_unused]] ArtMethod* method,
                 [[maybe_unused]] uint32_t dex_pc,
                 [[maybe_unused]] ArtField* field) override REQUIRES_SHARED(Locks::mutator_lock_) {
    received_field_read_event = true;
  }

  void FieldWritten([[maybe_unused]] Thread* thread,
                    [[maybe_unused]] Handle<mirror::Object> this_object,
                    [[maybe_unused]] ArtMethod* method,
                    [[maybe_unused]] uint32_t dex_pc,
                    [[maybe_unused]] ArtField* field,
                    [[maybe_unused]] Handle<mirror::Object> field_value) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_field_written_object_event = true;
  }

  void FieldWritten([[maybe_unused]] Thread* thread,
                    [[maybe_unused]] Handle<mirror::Object> this_object,
                    [[maybe_unused]] ArtMethod* method,
                    [[maybe_unused]] uint32_t dex_pc,
                    [[maybe_unused]] ArtField* field,
                    [[maybe_unused]] const JValue& field_value) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_field_written_event = true;
  }

  void ExceptionThrown([[maybe_unused]] Thread* thread,
                       [[maybe_unused]] Handle<mirror::Throwable> exception_object) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_exception_thrown_event = true;
  }

  void ExceptionHandled([[maybe_unused]] Thread* self,
                        [[maybe_unused]] Handle<mirror::Throwable> throwable) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_exception_handled_event = true;
  }

  void Branch([[maybe_unused]] Thread* thread,
              [[maybe_unused]] ArtMethod* method,
              [[maybe_unused]] uint32_t dex_pc,
              [[maybe_unused]] int32_t dex_pc_offset) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_branch_event = true;
  }

  void WatchedFramePop([[maybe_unused]] Thread* thread,
                       [[maybe_unused]] const ShadowFrame& frame) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    received_watched_frame_pop  = true;
  }

  void Reset() {
    received_method_enter_event = false;
    received_method_exit_event = false;
    received_method_exit_object_event = false;
    received_method_unwind_event = false;
    received_dex_pc_moved_event = false;
    received_field_read_event = false;
    received_field_written_event = false;
    received_field_written_object_event = false;
    received_exception_thrown_event = false;
    received_exception_handled_event = false;
    received_branch_event = false;
    received_watched_frame_pop = false;
  }

  bool received_method_enter_event;
  bool received_method_exit_event;
  bool received_method_exit_object_event;
  bool received_method_unwind_event;
  bool received_dex_pc_moved_event;
  bool received_field_read_event;
  bool received_field_written_event;
  bool received_field_written_object_event;
  bool received_exception_thrown_event;
  bool received_exception_handled_event;
  bool received_branch_event;
  bool received_watched_frame_pop;

 private:
  DISALLOW_COPY_AND_ASSIGN(TestInstrumentationListener);
};

class InstrumentationTest : public CommonRuntimeTest {
 public:
  // Unique keys used to test Instrumentation::ConfigureStubs.
  static constexpr const char* kClientOneKey = "TestClient1";
  static constexpr const char* kClientTwoKey = "TestClient2";

  InstrumentationTest() {
    use_boot_image_ = true;  // Make the Runtime creation cheaper.
  }

  void CheckConfigureStubs(const char* key, Instrumentation::InstrumentationLevel level) {
    ScopedObjectAccess soa(Thread::Current());
    instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
    ScopedThreadSuspension sts(soa.Self(), ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(soa.Self(),
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Instrumentation::ConfigureStubs");
    instr->ConfigureStubs(key, level, /*try_switch_to_non_debuggable=*/false);
  }

  Instrumentation::InstrumentationLevel GetCurrentInstrumentationLevel() {
    return Runtime::Current()->GetInstrumentation()->GetCurrentInstrumentationLevel();
  }

  size_t GetInstrumentationUserCount() {
    ScopedObjectAccess soa(Thread::Current());
    return Runtime::Current()->GetInstrumentation()->requested_instrumentation_levels_.size();
  }

  void TestEvent(uint32_t instrumentation_event) {
    TestEvent(instrumentation_event, nullptr, nullptr, false);
  }

  void TestEvent(uint32_t instrumentation_event,
                 ArtMethod* event_method,
                 ArtField* event_field,
                 bool with_object) {
    ScopedObjectAccess soa(Thread::Current());
    instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
    TestInstrumentationListener listener;
    {
      ScopedThreadSuspension sts(soa.Self(), ThreadState::kSuspended);
      ScopedSuspendAll ssa("Add instrumentation listener");
      instr->AddListener(&listener, instrumentation_event);
    }

    mirror::Object* const event_obj = nullptr;
    const uint32_t event_dex_pc = 0;
    ShadowFrameAllocaUniquePtr test_frame = CREATE_SHADOW_FRAME(0, event_method, 0);

    // Check the listener is registered and is notified of the event.
    EXPECT_TRUE(HasEventListener(instr, instrumentation_event));
    EXPECT_FALSE(DidListenerReceiveEvent(listener, instrumentation_event, with_object));
    ReportEvent(instr,
                instrumentation_event,
                soa.Self(),
                event_method,
                event_obj,
                event_field,
                event_dex_pc,
                *test_frame);
    EXPECT_TRUE(DidListenerReceiveEvent(listener, instrumentation_event, with_object));

    listener.Reset();
    {
      ScopedThreadSuspension sts(soa.Self(), ThreadState::kSuspended);
      ScopedSuspendAll ssa("Remove instrumentation listener");
      instr->RemoveListener(&listener, instrumentation_event);
    }

    // Check the listener is not registered and is not notified of the event.
    EXPECT_FALSE(HasEventListener(instr, instrumentation_event));
    EXPECT_FALSE(DidListenerReceiveEvent(listener, instrumentation_event, with_object));
    ReportEvent(instr,
                instrumentation_event,
                soa.Self(),
                event_method,
                event_obj,
                event_field,
                event_dex_pc,
                *test_frame);
    EXPECT_FALSE(DidListenerReceiveEvent(listener, instrumentation_event, with_object));
  }

  void DeoptimizeMethod(Thread* self, ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Single method deoptimization");
    instrumentation->Deoptimize(method);
  }

  void UndeoptimizeMethod(Thread* self, ArtMethod* method,
                          const char* key, bool disable_deoptimization)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Single method undeoptimization");
    instrumentation->Undeoptimize(method);
    if (disable_deoptimization) {
      instrumentation->DisableDeoptimization(key, /*try_switch_to_non_debuggable=*/false);
    }
  }

  void DeoptimizeEverything(Thread* self, const char* key)
        REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Full deoptimization");
    instrumentation->DeoptimizeEverything(key);
  }

  void UndeoptimizeEverything(Thread* self, const char* key, bool disable_deoptimization)
        REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Full undeoptimization");
    instrumentation->UndeoptimizeEverything(key);
    if (disable_deoptimization) {
      instrumentation->DisableDeoptimization(key, /*try_switch_to_non_debuggable=*/false);
    }
  }

  void EnableMethodTracing(Thread* self, const char* key, bool needs_interpreter)
        REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    TestInstrumentationListener listener;
    ScopedThreadSuspension sts(self, ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("EnableMethodTracing");
    instrumentation->EnableMethodTracing(key, &listener, needs_interpreter);
  }

  void DisableMethodTracing(Thread* self, const char* key)
        REQUIRES_SHARED(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, ThreadState::kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("EnableMethodTracing");
    instrumentation->DisableMethodTracing(key);
  }

 private:
  static bool HasEventListener(const instrumentation::Instrumentation* instr, uint32_t event_type)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    switch (event_type) {
      case instrumentation::Instrumentation::kMethodEntered:
        return instr->HasMethodEntryListeners();
      case instrumentation::Instrumentation::kMethodExited:
        return instr->HasMethodExitListeners();
      case instrumentation::Instrumentation::kMethodUnwind:
        return instr->HasMethodUnwindListeners();
      case instrumentation::Instrumentation::kDexPcMoved:
        return instr->HasDexPcListeners();
      case instrumentation::Instrumentation::kFieldRead:
        return instr->HasFieldReadListeners();
      case instrumentation::Instrumentation::kFieldWritten:
        return instr->HasFieldWriteListeners();
      case instrumentation::Instrumentation::kExceptionThrown:
        return instr->HasExceptionThrownListeners();
      case instrumentation::Instrumentation::kExceptionHandled:
        return instr->HasExceptionHandledListeners();
      case instrumentation::Instrumentation::kBranch:
        return instr->HasBranchListeners();
      case instrumentation::Instrumentation::kWatchedFramePop:
        return instr->HasWatchedFramePopListeners();
      default:
        LOG(FATAL) << "Unknown instrumentation event " << event_type;
        UNREACHABLE();
    }
  }

  static void ReportEvent(const instrumentation::Instrumentation* instr,
                          uint32_t event_type,
                          Thread* self,
                          ArtMethod* method,
                          mirror::Object* obj,
                          ArtField* field,
                          uint32_t dex_pc,
                          const ShadowFrame& frame)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    switch (event_type) {
      case instrumentation::Instrumentation::kMethodEntered:
        instr->MethodEnterEvent(self, method);
        break;
      case instrumentation::Instrumentation::kMethodExited: {
        JValue value;
        instr->MethodExitEvent(self, method, {}, value);
        break;
      }
      case instrumentation::Instrumentation::kMethodUnwind:
        instr->MethodUnwindEvent(self, method, dex_pc);
        break;
      case instrumentation::Instrumentation::kDexPcMoved:
        instr->DexPcMovedEvent(self, obj, method, dex_pc);
        break;
      case instrumentation::Instrumentation::kFieldRead:
        instr->FieldReadEvent(self, obj, method, dex_pc, field);
        break;
      case instrumentation::Instrumentation::kFieldWritten: {
        JValue value;
        instr->FieldWriteEvent(self, obj, method, dex_pc, field, value);
        break;
      }
      case instrumentation::Instrumentation::kExceptionThrown: {
        ThrowArithmeticExceptionDivideByZero();
        mirror::Throwable* event_exception = self->GetException();
        instr->ExceptionThrownEvent(self, event_exception);
        self->ClearException();
        break;
      }
      case instrumentation::Instrumentation::kBranch:
        instr->Branch(self, method, dex_pc, -1);
        break;
      case instrumentation::Instrumentation::kWatchedFramePop:
        instr->WatchedFramePopped(self, frame);
        break;
      case instrumentation::Instrumentation::kExceptionHandled: {
        ThrowArithmeticExceptionDivideByZero();
        mirror::Throwable* event_exception = self->GetException();
        self->ClearException();
        instr->ExceptionHandledEvent(self, event_exception);
        break;
      }
      default:
        LOG(FATAL) << "Unknown instrumentation event " << event_type;
        UNREACHABLE();
    }
  }

  static bool DidListenerReceiveEvent(const TestInstrumentationListener& listener,
                                      uint32_t event_type,
                                      bool with_object) {
    switch (event_type) {
      case instrumentation::Instrumentation::kMethodEntered:
        return listener.received_method_enter_event;
      case instrumentation::Instrumentation::kMethodExited:
        return (!with_object && listener.received_method_exit_event) ||
            (with_object && listener.received_method_exit_object_event);
      case instrumentation::Instrumentation::kMethodUnwind:
        return listener.received_method_unwind_event;
      case instrumentation::Instrumentation::kDexPcMoved:
        return listener.received_dex_pc_moved_event;
      case instrumentation::Instrumentation::kFieldRead:
        return listener.received_field_read_event;
      case instrumentation::Instrumentation::kFieldWritten:
        return (!with_object && listener.received_field_written_event) ||
            (with_object && listener.received_field_written_object_event);
      case instrumentation::Instrumentation::kExceptionThrown:
        return listener.received_exception_thrown_event;
      case instrumentation::Instrumentation::kExceptionHandled:
        return listener.received_exception_handled_event;
      case instrumentation::Instrumentation::kBranch:
        return listener.received_branch_event;
      case instrumentation::Instrumentation::kWatchedFramePop:
        return listener.received_watched_frame_pop;
      default:
        LOG(FATAL) << "Unknown instrumentation event " << event_type;
        UNREACHABLE();
    }
  }
};

TEST_F(InstrumentationTest, NoInstrumentation) {
  ScopedObjectAccess soa(Thread::Current());
  instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
  ASSERT_NE(instr, nullptr);

  EXPECT_FALSE(instr->RunExitHooks());
  EXPECT_FALSE(instr->EntryExitStubsInstalled());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->NeedsSlowInterpreterForListeners());

  // Check there is no registered listener.
  EXPECT_FALSE(instr->HasDexPcListeners());
  EXPECT_FALSE(instr->HasExceptionThrownListeners());
  EXPECT_FALSE(instr->HasExceptionHandledListeners());
  EXPECT_FALSE(instr->HasFieldReadListeners());
  EXPECT_FALSE(instr->HasFieldWriteListeners());
  EXPECT_FALSE(instr->HasMethodEntryListeners());
  EXPECT_FALSE(instr->HasMethodExitListeners());
}

// Test instrumentation listeners for each event.
TEST_F(InstrumentationTest, MethodEntryEvent) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method =
      klass->FindClassMethod("returnReference", "()Ljava/lang/Object;", kRuntimePointerSize);
  ASSERT_TRUE(method != nullptr);
  ASSERT_TRUE(method->IsDirect());
  ASSERT_TRUE(method->GetDeclaringClass() == klass);
  TestEvent(instrumentation::Instrumentation::kMethodEntered,
            /*event_method=*/ method,
            /*event_field=*/ nullptr,
            /*with_object=*/ true);
}

TEST_F(InstrumentationTest, MethodExitObjectEvent) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  MutableHandle<mirror::ClassLoader> loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method =
      klass->FindClassMethod("returnReference", "()Ljava/lang/Object;", kRuntimePointerSize);
  ASSERT_TRUE(method != nullptr);
  ASSERT_TRUE(method->IsDirect());
  ASSERT_TRUE(method->GetDeclaringClass() == klass);
  TestEvent(instrumentation::Instrumentation::kMethodExited,
            /*event_method=*/ method,
            /*event_field=*/ nullptr,
            /*with_object=*/ true);
}

TEST_F(InstrumentationTest, MethodExitPrimEvent) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method = klass->FindClassMethod("returnPrimitive", "()I", kRuntimePointerSize);
  ASSERT_TRUE(method != nullptr);
  ASSERT_TRUE(method->IsDirect());
  ASSERT_TRUE(method->GetDeclaringClass() == klass);
  TestEvent(instrumentation::Instrumentation::kMethodExited,
            /*event_method=*/ method,
            /*event_field=*/ nullptr,
            /*with_object=*/ false);
}

TEST_F(InstrumentationTest, MethodUnwindEvent) {
  TestEvent(instrumentation::Instrumentation::kMethodUnwind);
}

TEST_F(InstrumentationTest, DexPcMovedEvent) {
  TestEvent(instrumentation::Instrumentation::kDexPcMoved);
}

TEST_F(InstrumentationTest, FieldReadEvent) {
  TestEvent(instrumentation::Instrumentation::kFieldRead);
}

TEST_F(InstrumentationTest, WatchedFramePop) {
  TestEvent(instrumentation::Instrumentation::kWatchedFramePop);
}

TEST_F(InstrumentationTest, FieldWriteObjectEvent) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtField* field = klass->FindDeclaredStaticField("referenceField", "Ljava/lang/Object;");
  ASSERT_TRUE(field != nullptr);

  TestEvent(instrumentation::Instrumentation::kFieldWritten,
            /*event_method=*/ nullptr,
            /*event_field=*/ field,
            /*with_object=*/ true);
}

TEST_F(InstrumentationTest, FieldWritePrimEvent) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtField* field = klass->FindDeclaredStaticField("primitiveField", "I");
  ASSERT_TRUE(field != nullptr);

  TestEvent(instrumentation::Instrumentation::kFieldWritten,
            /*event_method=*/ nullptr,
            /*event_field=*/ field,
            /*with_object=*/ false);
}

TEST_F(InstrumentationTest, ExceptionHandledEvent) {
  TestEvent(instrumentation::Instrumentation::kExceptionHandled);
}

TEST_F(InstrumentationTest, ExceptionThrownEvent) {
  TestEvent(instrumentation::Instrumentation::kExceptionThrown);
}

TEST_F(InstrumentationTest, BranchEvent) {
  TestEvent(instrumentation::Instrumentation::kBranch);
}

TEST_F(InstrumentationTest, DeoptimizeDirectMethod) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method_to_deoptimize =
      klass->FindClassMethod("instanceMethod", "()V", kRuntimePointerSize);
  ASSERT_TRUE(method_to_deoptimize != nullptr);
  ASSERT_TRUE(method_to_deoptimize->IsDirect());
  ASSERT_TRUE(method_to_deoptimize->GetDeclaringClass() == klass);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));

  DeoptimizeMethod(soa.Self(), method_to_deoptimize);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  constexpr const char* instrumentation_key = "DeoptimizeDirectMethod";
  UndeoptimizeMethod(soa.Self(), method_to_deoptimize, instrumentation_key, true);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));
}

TEST_F(InstrumentationTest, FullDeoptimization) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());

  constexpr const char* instrumentation_key = "FullDeoptimization";
  DeoptimizeEverything(soa.Self(), instrumentation_key);

  EXPECT_TRUE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());
  EXPECT_TRUE(instr->InterpreterStubsInstalled());

  UndeoptimizeEverything(soa.Self(), instrumentation_key, true);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
}

TEST_F(InstrumentationTest, MixedDeoptimization) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader)));
  ObjPtr<mirror::Class> klass = FindClass("LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method_to_deoptimize =
      klass->FindClassMethod("instanceMethod", "()V", kRuntimePointerSize);
  ASSERT_TRUE(method_to_deoptimize != nullptr);
  ASSERT_TRUE(method_to_deoptimize->IsDirect());
  ASSERT_TRUE(method_to_deoptimize->GetDeclaringClass() == klass);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));

  DeoptimizeMethod(soa.Self(), method_to_deoptimize);
  // Deoptimizing a method does not change instrumentation level.
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  constexpr const char* instrumentation_key = "MixedDeoptimization";
  DeoptimizeEverything(soa.Self(), instrumentation_key);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter,
            GetCurrentInstrumentationLevel());
  EXPECT_TRUE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  UndeoptimizeEverything(soa.Self(), instrumentation_key, false);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  UndeoptimizeMethod(soa.Self(), method_to_deoptimize, instrumentation_key, true);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));
}

TEST_F(InstrumentationTest, MethodTracing_Interpreter) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());

  constexpr const char* instrumentation_key = "MethodTracing";
  EnableMethodTracing(soa.Self(), instrumentation_key, true);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter,
            GetCurrentInstrumentationLevel());
  EXPECT_TRUE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());

  DisableMethodTracing(soa.Self(), instrumentation_key);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
}

TEST_F(InstrumentationTest, MethodTracing_InstrumentationEntryExitStubs) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());

  constexpr const char* instrumentation_key = "MethodTracing";
  EnableMethodTracing(soa.Self(), instrumentation_key, false);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->RunExitHooks());

  DisableMethodTracing(soa.Self(), instrumentation_key);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
}

// We use a macro to print the line number where the test is failing.
#define CHECK_INSTRUMENTATION(_level, _user_count)                                      \
  do {                                                                                  \
    Instrumentation* const instr = Runtime::Current()->GetInstrumentation();            \
    bool interpreter =                                                                  \
      ((_level) == Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);  \
    EXPECT_EQ(_level, GetCurrentInstrumentationLevel());                                \
    EXPECT_EQ(_user_count, GetInstrumentationUserCount());                              \
    if (instr->IsForcedInterpretOnly()) {                                               \
      EXPECT_TRUE(instr->InterpretOnly());                                              \
    } else if (interpreter) {                                                           \
      EXPECT_TRUE(instr->InterpretOnly());                                              \
    } else {                                                                            \
      EXPECT_FALSE(instr->InterpretOnly());                                             \
    }                                                                                   \
    if (interpreter) {                                                                  \
      EXPECT_TRUE(instr->AreAllMethodsDeoptimized());                                   \
    } else {                                                                            \
      EXPECT_FALSE(instr->AreAllMethodsDeoptimized());                                  \
    }                                                                                   \
  } while (false)

TEST_F(InstrumentationTest, ConfigureStubs_Nothing) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check no-op.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_InstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check we can switch to instrumentation stubs
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_Interpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check we can switch to interpreter
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_InstrumentationStubsToInterpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Configure stubs with interpreter.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_InterpreterToInstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with interpreter.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with instrumentation stubs.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest,
       ConfigureStubs_InstrumentationStubsToInterpreterToInstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Configure stubs with interpreter.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with instrumentation stubs again.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_Nothing) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check kInstrumentNothing with two clients.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_InstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Configure stubs with instrumentation stubs for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs
  // instrumentation stubs.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_Interpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with interpreter for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with interpreter for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs interpreter.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_InstrumentationStubsThenInterpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // Configure stubs with interpreter for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs interpreter.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_InterpreterThenInstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with interpreter for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with instrumentation stubs for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs
  // instrumentation stubs.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithEntryExitHooks, 1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

}  // namespace instrumentation
}  // namespace art
