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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_GENERIC_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_GENERIC_HPP

#include "runtime/os.hpp"
#include "runtime/atomic.hpp"

class GenericShenandoahLock {
private:
  enum LockState { unlocked = 0, locked = 1 };

  volatile LockState _state;
  volatile Thread* _owner;

  void contended_lock(bool allow_block_for_safepoint);
  template<bool ALLOW_BLOC, uint32_t MAX_SPINS>
  void contended_lock_internal(Thread* thread);
public:
  GenericShenandoahLock() : _state(unlocked), _owner(nullptr) {};

  void lock(bool allow_block_for_safepoint) {
    assert(Atomic::load(&_owner) != Thread::current(), "reentrant locking attempt, would deadlock");

    // Try to lock fast, or dive into contended lock handling.
    if (Atomic::cmpxchg(&_state, unlocked, locked) != unlocked) {
      contended_lock(allow_block_for_safepoint);
    }

    assert(Atomic::load(&_state) == locked, "must be locked");
    assert(Atomic::load(&_owner) == nullptr, "must not be owned");
    DEBUG_ONLY(Atomic::store(&_owner, Thread::current());)
  }

  void unlock() {
    assert(Atomic::load(&_owner) == Thread::current(), "sanity");
    DEBUG_ONLY(Atomic::store(&_owner, (Thread*)nullptr);)
    OrderAccess::fence();
    Atomic::store(&_state, unlocked);
  }

  bool owned_by_self() {
    return _state == locked && _owner == Thread::current();
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHLOCK_GENERIC_HPP
