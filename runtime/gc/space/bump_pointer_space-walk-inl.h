/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_WALK_INL_H_
#define ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_WALK_INL_H_

#include "bump_pointer_space-inl.h"

#include "base/bit_utils.h"
#include "mirror/object-inl.h"
#include "thread-current-inl.h"

#include <memory>

namespace art HIDDEN {
namespace gc {
namespace space {

template <typename Visitor>
inline void BumpPointerSpace::Walk(Visitor&& visitor) {
  uint8_t* pos = Begin();
  uint8_t* end = End();
  uint8_t* main_end = pos;
  size_t black_dense_size;
  std::unique_ptr<std::vector<size_t>> block_sizes_copy;
  // Internal indirection w/ NO_THREAD_SAFETY_ANALYSIS. Optimally, we'd like to have an annotation
  // like
  //   REQUIRES_AS(visitor.operator(mirror::Object*))
  // on Walk to expose the interprocedural nature of locks here without having to duplicate the
  // function.
  //
  // NO_THREAD_SAFETY_ANALYSIS is a workaround. The problem with the workaround of course is that
  // it doesn't complain at the callsite. However, that is strictly not worse than the
  // ObjectCallback version it replaces.
  auto no_thread_safety_analysis_visit = [&](mirror::Object* obj) NO_THREAD_SAFETY_ANALYSIS {
    visitor(obj);
  };

  {
    MutexLock mu(Thread::Current(), lock_);
    // If we have 0 blocks then we need to update the main header since we have bump pointer style
    // allocation into an unbounded region (actually bounded by Capacity()).
    if (block_sizes_.empty()) {
      UpdateMainBlock();
    }
    main_end = Begin() + main_block_size_;
    if (block_sizes_.empty()) {
      // We don't have any other blocks, this means someone else may be allocating into the main
      // block. In this case, we don't want to try and visit the other blocks after the main block
      // since these could actually be part of the main block.
      end = main_end;
    } else {
      block_sizes_copy.reset(new std::vector<size_t>(block_sizes_.begin(), block_sizes_.end()));
    }

    black_dense_size = black_dense_region_size_;
  }

  // black_dense_region_size_ will be non-zero only in case of moving-space of CMC GC.
  if (black_dense_size > 0) {
    // Objects are not packed in this case, and therefore the bitmap is needed
    // to walk this part of the space.
    // Remember the last object visited using bitmap to be able to fetch its size.
    mirror::Object* last_obj = nullptr;
    auto return_obj_visit = [&](mirror::Object* obj) NO_THREAD_SAFETY_ANALYSIS {
      visitor(obj);
      last_obj = obj;
    };
    GetMarkBitmap()->VisitMarkedRange(reinterpret_cast<uintptr_t>(pos),
                                      reinterpret_cast<uintptr_t>(pos + black_dense_size),
                                      return_obj_visit);
    pos += black_dense_size;
    if (last_obj != nullptr) {
      // If the last object visited using bitmap was large enough to go past the
      // black-dense region, then we need to adjust for that to be able to visit
      // objects one after the other below.
      pos = std::max(pos, reinterpret_cast<uint8_t*>(GetNextObject(last_obj)));
    }
  }
  // Walk all of the objects in the main block first.
  while (pos < main_end) {
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(pos);
    // No read barrier because obj may not be a valid object.
    if (obj->GetClass<kDefaultVerifyFlags, kWithoutReadBarrier>() == nullptr) {
      // There is a race condition where a thread has just allocated an object but not set the
      // class. We can't know the size of this object, so we don't visit it and break the loop
      pos = main_end;
      break;
    } else {
      no_thread_safety_analysis_visit(obj);
      pos = reinterpret_cast<uint8_t*>(GetNextObject(obj));
    }
  }
  // Walk the other blocks (currently only TLABs).
  if (block_sizes_copy != nullptr) {
    size_t iter = 0;
    size_t num_blks = block_sizes_copy->size();
    // Skip blocks which are already visited above as part of black-dense region.
    for (uint8_t* ptr = main_end; iter < num_blks; iter++) {
      size_t block_size = (*block_sizes_copy)[iter];
      ptr += block_size;
      if (ptr > pos) {
        // Adjust block-size in case 'pos' is in the middle of the block.
        if (static_cast<ssize_t>(block_size) > ptr - pos) {
          (*block_sizes_copy)[iter] = ptr - pos;
        }
        break;
      }
    }

    for (; iter < num_blks; iter++) {
      size_t block_size = (*block_sizes_copy)[iter];
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(pos);
      const mirror::Object* end_obj = reinterpret_cast<const mirror::Object*>(pos + block_size);
      CHECK_LE(reinterpret_cast<const uint8_t*>(end_obj), End());
      // We don't know how many objects are allocated in the current block. When we hit a null class
      // assume it's the end. TODO: Have a thread update the header when it flushes the block?
      // No read barrier because obj may not be a valid object.
      while (obj < end_obj && obj->GetClass<kDefaultVerifyFlags, kWithoutReadBarrier>() != nullptr) {
        no_thread_safety_analysis_visit(obj);
        obj = GetNextObject(obj);
      }
      pos += block_size;
    }
  } else {
    CHECK_EQ(end, main_end);
  }
  CHECK_EQ(pos, end);
}

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_WALK_INL_H_
