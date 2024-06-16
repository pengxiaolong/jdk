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

#include "memory/allocation.hpp"
#include "runtime/thread.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/os.hpp"


#if defined(LINUX)
#include "gc/shenandoah/shenandoahLock_linux.hpp"
typedef LinuxShenandoahLock ShenandoahLockDefault;
#else
#include "gc/shenandoah/shenandoahLock_generic.hpp"
typedef GenericShenandoahLock ShenandoahLockDefault;
#endif

template <typename ShenandoahLockImpl>
class ShenandoahLockType  {
private:
  ShenandoahLockImpl _impl;
  jlong acquire;
  jlong acquired;
  jlong released;
public:
  ShenandoahLockType() : _impl(), acquire(0), acquired(0), released(0) {};

  void lock(bool allow_block_for_safepoint) {
    acquire = os::javaTimeNanos();
    _impl.lock(allow_block_for_safepoint);
    acquired = os::javaTimeNanos();
  }

  void unlock() {
    released = os::javaTimeNanos();
    _impl.unlock();
    Thread* thread = Thread::current();
    const char* name = thread->is_Java_thread() ? "Java thread" : thread->name();
    log_info(gc)("ShenandoahLock has been unlocked (" PTR_FORMAT ") by %s (" PTR_FORMAT "), took " JLONG_FORMAT " ns to acquire, holded for " JLONG_FORMAT " ns.",
                   p2i(this), name, p2i(thread), (acquired - acquire), (released - acquired));
  }

  bool owned_by_self() {
#ifdef ASSERT
    return _impl.owned_by_self();
#else
    ShouldNotReachHere();
    return false;
#endif
  }
};

typedef ShenandoahLockType<ShenandoahLockDefault> ShenandoahLock;

class ShenandoahLocker : public StackObj {
private:
  ShenandoahLock* _lock;
public:
  ShenandoahLocker(ShenandoahLock* lock, bool allow_block_for_safepoint = false) : _lock(lock) {
    if (_lock != nullptr) {
      _lock->lock(allow_block_for_safepoint);
    }
  }

  ~ShenandoahLocker() {
    if (_lock != nullptr) {
      _lock->unlock();
    }
  }
};

class ShenandoahSimpleLock {
private:
  PlatformMonitor   _lock; // native lock
public:
  ShenandoahSimpleLock();

  virtual void lock();
  virtual void unlock();
};

class ShenandoahReentrantLock : public ShenandoahSimpleLock {
private:
  Thread* volatile      _owner;
  uint64_t              _count;

public:
  ShenandoahReentrantLock();
  ~ShenandoahReentrantLock();

  virtual void lock();
  virtual void unlock();

  // If the lock already owned by this thread
  bool owned_by_self() const ;
};

class ShenandoahReentrantLocker : public StackObj {
private:
  ShenandoahReentrantLock* const _lock;

public:
  ShenandoahReentrantLocker(ShenandoahReentrantLock* lock) :
    _lock(lock) {
    if (_lock != nullptr) {
      _lock->lock();
    }
  }

  ~ShenandoahReentrantLocker() {
    if (_lock != nullptr) {
      assert(_lock->owned_by_self(), "Must be owner");
      _lock->unlock();
    }
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_HPP
