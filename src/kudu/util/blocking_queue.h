// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#pragma once

#include <list>
#include <memory>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include "kudu/gutil/basictypes.h"
#include "kudu/util/condition_variable.h"
#include "kudu/util/monotime.h"
#include "kudu/util/mutex.h"
#include "kudu/util/status.h"

namespace kudu {

// Return values for BlockingQueue::Put()
enum QueueStatus {
  QUEUE_SUCCESS = 0,
  QUEUE_SHUTDOWN = 1,
  QUEUE_FULL = 2
};

// Default logical length implementation: always returns 1.
struct DefaultLogicalSize {
  template<typename T>
  static size_t logical_size(const T& /* unused */) {
    return 1;
  }
};

template <typename T, class LOGICAL_SIZE = DefaultLogicalSize>
class BlockingQueue {
 public:
  // If T is a pointer, this will be the base type.  If T is not a pointer, you
  // can ignore this and the functions which make use of it.
  // Template substitution failure is not an error.
  typedef typename std::remove_pointer<T>::type T_VAL;

  explicit BlockingQueue(size_t max_size)
    : shutdown_(false),
      size_(0),
      max_size_(max_size),
      not_empty_(&lock_),
      not_full_(&lock_) {
  }

  // If the queue holds a bare pointer, it must be empty on destruction, since
  // it may have ownership of the pointer.
  ~BlockingQueue() {
    DCHECK(list_.empty() || !std::is_pointer<T>::value)
        << "BlockingQueue holds bare pointers at destruction time";
  }

  // Gets an element from the queue; if the queue is empty, blocks until the
  // queue becomes non-empty, or until the deadline passes.
  //
  // If the queue has been shut down but there are still elements in the queue,
  // it returns those elements as if the queue were not yet shut down.
  //
  // Returns:
  // - OK if successful
  // - TimedOut if the deadline passed
  // - Aborted if the queue shut down
  Status BlockingGet(T* out, MonoTime deadline = MonoTime()) {
    MutexLock l(lock_);
    while (true) {
      if (!list_.empty()) {
        *out = list_.front();
        list_.pop_front();
        decrement_size_unlocked(*out);
        not_full_.Signal();
        return Status::OK();
      }
      if (shutdown_) {
        return Status::Aborted("");
      }
      if (!deadline.Initialized()) {
        not_empty_.Wait();
      } else if (PREDICT_FALSE(!not_empty_.WaitUntil(deadline))) {
        return Status::TimedOut("");
      }
    }
  }

  // Get an element from the queue.  Returns false if the queue is empty and
  // we were shut down prior to getting the element.
  Status BlockingGet(std::unique_ptr<T_VAL>* out, MonoTime deadline = MonoTime()) {
    T t = nullptr;
    RETURN_NOT_OK(BlockingGet(&t, deadline));
    out->reset(t);
    return Status::OK();
  }

  // Get all elements from the queue and append them to a vector.
  //
  // If 'deadline' passes and no elements have been returned from the
  // queue, returns Status::TimedOut(). If 'deadline' is uninitialized,
  // no deadline is used.
  //
  // If the queue has been shut down, but there are still elements waiting,
  // then it returns those elements as if the queue were not yet shut down.
  //
  // Returns:
  // - OK if successful
  // - TimedOut if the deadline passed
  // - Aborted if the queue shut down
  Status BlockingDrainTo(std::vector<T>* out, MonoTime deadline = MonoTime()) {
    MutexLock l(lock_);
    while (true) {
      if (!list_.empty()) {
        out->reserve(list_.size());
        for (const T& elt : list_) {
          out->push_back(elt);
          decrement_size_unlocked(elt);
        }
        list_.clear();
        not_full_.Signal();
        return Status::OK();
      }
      if (PREDICT_FALSE(shutdown_)) {
        return Status::Aborted("");
      }
      if (!deadline.Initialized()) {
        not_empty_.Wait();
      } else if (PREDICT_FALSE(!not_empty_.WaitUntil(deadline))) {
        return Status::TimedOut("");
      }
    }
  }

  // Attempts to put the given value in the queue.
  // Returns:
  //   QUEUE_SUCCESS: if successfully enqueued
  //   QUEUE_FULL: if the queue has reached max_size
  //   QUEUE_SHUTDOWN: if someone has already called Shutdown()
  QueueStatus Put(const T& val) {
    MutexLock l(lock_);
    if (size_ >= max_size_) {
      return QUEUE_FULL;
    }
    if (shutdown_) {
      return QUEUE_SHUTDOWN;
    }
    list_.push_back(val);
    increment_size_unlocked(val);
    l.Unlock();
    not_empty_.Signal();
    return QUEUE_SUCCESS;
  }

  // Same as other Put() overload above.
  //
  // If the element was enqueued, the contents of 'val' are released.
  QueueStatus Put(std::unique_ptr<T_VAL>* val) {
    QueueStatus s = Put(val->get());
    if (s == QUEUE_SUCCESS) {
      ignore_result<>(val->release());
    }
    return s;
  }

  // Puts an element onto the queue; if the queue is full, blocks until space
  // becomes available, or until the deadline passes.
  //
  // NOTE: unlike BlockingGet() and BlockingDrainTo(), which succeed as long as
  // there are elements in the queue (regardless of deadline), if the deadline
  // has passed, an error will be returned even if there is space in the queue.
  //
  // Returns:
  // - OK if successful
  // - TimedOut if the deadline passed
  // - Aborted if the queue shut down
  Status BlockingPut(const T& val, MonoTime deadline = MonoTime()) {
    if (deadline.Initialized() && MonoTime::Now() > deadline) {
      return Status::TimedOut("");
    }
    MutexLock l(lock_);
    while (true) {
      if (shutdown_) {
        return Status::Aborted("");
      }
      if (size_ < max_size_) {
        list_.push_back(val);
        increment_size_unlocked(val);
        l.Unlock();
        not_empty_.Signal();
        return Status::OK();
      }
      if (!deadline.Initialized()) {
        not_full_.Wait();
      } else if (PREDICT_FALSE(!not_full_.WaitUntil(deadline))) {
        return Status::TimedOut("");
      }
    }
  }

  // Same as other BlockingPut() overload above.
  //
  // If the element was enqueued, the contents of 'val' are released.
  Status BlockingPut(std::unique_ptr<T_VAL>* val, MonoTime deadline = MonoTime()) {
    RETURN_NOT_OK(BlockingPut(val->get(), deadline));
    ignore_result(val->release());
    return Status::OK();
  }

  // Shuts down the queue.
  //
  // When a blocking queue is shut down, no more elements can be added to it,
  // and Put() will return QUEUE_SHUTDOWN.
  //
  // Existing elements will drain out of it, and then BlockingGet will start
  // returning false.
  void Shutdown() {
    MutexLock l(lock_);
    shutdown_ = true;
    not_full_.Broadcast();
    not_empty_.Broadcast();
  }

  bool empty() const {
    MutexLock l(lock_);
    return list_.empty();
  }

  size_t max_size() const {
    return max_size_;
  }

  size_t size() const {
    MutexLock l(lock_);
    return size_;
  }

  std::string ToString() const {
    std::string ret;

    MutexLock l(lock_);
    for (const T& t : list_) {
      ret.append(t->ToString());
      ret.append("\n");
    }
    return ret;
  }

 private:

  // Increments queue size. Must be called when 'lock_' is held.
  void increment_size_unlocked(const T& t) {
    size_ += LOGICAL_SIZE::logical_size(t);
  }

  // Decrements queue size. Must be called when 'lock_' is held.
  void decrement_size_unlocked(const T& t) {
    size_ -= LOGICAL_SIZE::logical_size(t);
  }

  bool shutdown_;
  size_t size_;
  const size_t max_size_;
  mutable Mutex lock_;
  ConditionVariable not_empty_;
  ConditionVariable not_full_;
  std::list<T> list_;
};

} // namespace kudu
