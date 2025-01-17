/*
 * Copyright (c) 2017, 2019, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_HPP

#include "gc/shenandoah/shenandoahPadding.hpp"
#include "memory/allocation.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/safepoint.hpp"

template<bool REENTRANT>
class ShenandoahLockImpl {
private:
  enum LockState { unlocked = 0, locked = 1 };

  shenandoah_padding(0);
  volatile LockState _state;
  shenandoah_padding(1);
  Thread* volatile _owner;
  shenandoah_padding(2);
  uint64_t volatile _count;
  shenandoah_padding(3);
  template<bool ALLOW_BLOCK>
  void contended_lock_internal(JavaThread* java_thread);
public:
  ShenandoahLockImpl() : _state(unlocked), _owner(nullptr), _count(0) {};

  void lock(bool allow_block_for_safepoint = false) {
    if (!REENTRANT) {
      assert(Atomic::load(&_owner) != Thread::current(), "reentrant locking attempt, would deadlock");
    }

    if (REENTRANT && owned_by_self()) {
      Atomic::add(&_count, (uint64_t) 1);
      return;
    }

    if ((allow_block_for_safepoint && SafepointSynchronize::is_synchronizing()) ||
        (Atomic::cmpxchg(&_state, unlocked, locked) != unlocked)) {
      // 1. Java thread, and there is a pending safepoint. Dive into contended locking
      //    immediately without trying anything else, and block.
      // 2. Fast lock fails, dive into contended lock handling.
      contended_lock(allow_block_for_safepoint);
    }

    assert(Atomic::load(&_state) == locked, "must be locked");

    if (REENTRANT) {
      Atomic::store(&_owner, Thread::current());
      Atomic::add(&_count, (uint64_t) 1);
    } else {
      assert(Atomic::load(&_owner) == nullptr, "must not be owned");
      DEBUG_ONLY(Atomic::store(&_owner, Thread::current());)
    }
  }

  void unlock() {
    assert(Atomic::load(&_owner) == Thread::current(), "sanity");
    if (REENTRANT) {
      assert(Atomic::load(&_count) > 0, "sanity");
      auto newValue = Atomic::add(&_count, (uint64_t)-1);
      if (newValue == 0) {
        Atomic::store(&_owner, (Thread*)nullptr);
        OrderAccess::fence();
        Atomic::store(&_state, unlocked);
      }
    } else {
      DEBUG_ONLY(Atomic::store(&_owner, (Thread*)nullptr);)
      OrderAccess::fence();
      Atomic::store(&_state, unlocked);
    }
  }

  void contended_lock(bool allow_block_for_safepoint);

  inline bool owned_by_self() {
    if (REENTRANT) {
      return Atomic::load(&_state) == locked && Atomic::load(&_owner) == Thread::current();
    } else {
#ifdef ASSERT
      return Atomic::load(&_state) == locked && Atomic::load(&_owner) == Thread::current();
#else
      ShouldNotReachHere();
      return false;
#endif
    }
  }
};

template<bool REENTRANT>
class ShenandoahLockerImpl : public StackObj {
private:
  ShenandoahLockImpl<REENTRANT>* const _lock;
public:
  ShenandoahLockerImpl(ShenandoahLockImpl<REENTRANT>* lock, bool allow_block_for_safepoint = false) : _lock(lock) {
    if (_lock != nullptr) {
      _lock->lock(allow_block_for_safepoint);
    }
  }

  ~ShenandoahLockerImpl() {
    if (_lock != nullptr) {
      _lock->unlock();
    }
  }
};

typedef ShenandoahLockImpl<false>   ShenandoahLock;
typedef ShenandoahLockerImpl<false> ShenandoahLocker;

typedef ShenandoahLockImpl<true>    ShenandoahReentrantLock;
typedef ShenandoahLockerImpl<true>  ShenandoahReentrantLocker;

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_HPP
