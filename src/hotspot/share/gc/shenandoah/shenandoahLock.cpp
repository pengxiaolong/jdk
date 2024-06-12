/*
 * Copyright (c) 2019, Red Hat, Inc. All rights reserved.
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

#include "precompiled.hpp"

#include "runtime/os.hpp"

#include "gc/shenandoah/shenandoahLock.hpp"
#include "runtime/atomic.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.inline.hpp"

  void ShenandoahLock::lock(bool allow_block_for_safepoint) {
    assert(Atomic::load(&_owner) != Thread::current(), "reentrant locking attempt, would deadlock");

    int attempts = 1;
    // Try to lock fast, or dive into contended lock handling.
    if (Atomic::cmpxchg(&_state, unlocked, locked) != unlocked) {
      contended_lock(allow_block_for_safepoint, attempts);
    }
    log_info(gc)("ShenandoahLock::lock, JavaThread: %i,allow block:%i, attempts:%i", Thread::current()->is_Java_thread(), allow_block_for_safepoint, attempts);
    assert(Atomic::load(&_state) == locked, "must be locked");
    assert(Atomic::load(&_owner) == nullptr, "must not be owned");
    DEBUG_ONLY(Atomic::store(&_owner, Thread::current());)
  }


void ShenandoahLock::unlock() {
    assert(Atomic::load(&_owner) == Thread::current(), "sanity");
    DEBUG_ONLY(Atomic::store(&_owner, (Thread*)nullptr);)
    log_info(gc)("ShenandoahLock::unlock, javaThread: %i", Thread::current()->is_Java_thread());
    OrderAccess::fence();
    Atomic::store(&_state, unlocked);
}

void ShenandoahLock::contended_lock(bool allow_block_for_safepoint, int &attempts) {
  Thread* thread = Thread::current();
  if ( thread->is_Java_thread()) {
    if (allow_block_for_safepoint) {
      contended_lock_internal<true>(JavaThread::cast(thread), attempts);
    } else {
      contended_lock_internal<false>(JavaThread::cast(thread), attempts);
    }
  } else {
    contended_lock_internal_non_java_thread(attempts);
  }
}

void ShenandoahLock::contended_lock_internal_non_java_thread(int &attempts) {
  assert(!Thread::current()->is_Java_thread(), "Can't be Java Thread.");
  int ctr = 0;
  int yields = 0;
  while (_state == locked || Atomic::cmpxchg(&_state, unlocked, locked) != unlocked) {
    attempts++;
    if (SafepointSynchronize::is_at_safepoint() || SafepointSynchronize::is_synchronizing() 
        || (os::is_MP() && (ctr++ & 0x3FF) != 0)) {
      SpinPause();
    } else {
      if(yields < 5) {
        os::naked_yield();
        yields ++;
      } else {
        os::naked_short_sleep(1);
      }
    }
  }
}

template<bool ALLOW_BLOCK>
void ShenandoahLock::contended_lock_internal(JavaThread* java_thread, int &attempts) {
  int ctr = os::is_MP() ? 0xF : 0;
  int afterTBIVMYields = 0;
  while (_state == locked ||
      Atomic::cmpxchg(&_state, unlocked, locked) != unlocked) {
    attempts ++;
    if (ctr >0 && !SafepointMechanism::local_poll_armed(java_thread)) {
    //if (ctr > 0 && !SafepointSynchronize::is_synchronizing()) {
      // Lightly contended, Spin a little if there are multiple processors,
      // there is no point in spinning and not giving up on CPU if there is single CPU processor.
      ctr--;
      SpinPause();
    } else if (ALLOW_BLOCK) {
      //if (SafepointSynchronize::is_synchronizing()) {
      if (SafepointMechanism::local_poll_armed(java_thread)) { 
       // Notify VM we are blocking, and suspend if safepoint was announced
        // while we were backing off.
        ThreadBlockInVM block(java_thread, true);
	os::naked_yield();
	afterTBIVMYields ++;
      } else {
        os::naked_short_sleep(1);
      }
    } else {
      os::naked_short_sleep(1);
    }
  }
  if(afterTBIVMYields != 0) {
    log_info(gc)("ShenandoahLock::contended_lock_internal, afterTBIVMYields: %i", afterTBIVMYields);
  }
}

ShenandoahSimpleLock::ShenandoahSimpleLock() {
  assert(os::mutex_init_done(), "Too early!");
}

void ShenandoahSimpleLock::lock() {
  _lock.lock();
}

void ShenandoahSimpleLock::unlock() {
  _lock.unlock();
}

ShenandoahReentrantLock::ShenandoahReentrantLock() :
  ShenandoahSimpleLock(), _owner(nullptr), _count(0) {
  assert(os::mutex_init_done(), "Too early!");
}

ShenandoahReentrantLock::~ShenandoahReentrantLock() {
  assert(_count == 0, "Unbalance");
}

void ShenandoahReentrantLock::lock() {
  Thread* const thread = Thread::current();
  Thread* const owner = Atomic::load(&_owner);

  if (owner != thread) {
    ShenandoahSimpleLock::lock();
    Atomic::store(&_owner, thread);
  }

  _count++;
}

void ShenandoahReentrantLock::unlock() {
  assert(owned_by_self(), "Invalid owner");
  assert(_count > 0, "Invalid count");

  _count--;

  if (_count == 0) {
    Atomic::store(&_owner, (Thread*)nullptr);
    ShenandoahSimpleLock::unlock();
  }
}

bool ShenandoahReentrantLock::owned_by_self() const {
  Thread* const thread = Thread::current();
  Thread* const owner = Atomic::load(&_owner);
  return owner == thread;
}
