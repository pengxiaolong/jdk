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

template<bool REENTRANT>
void ShenandoahLockImpl<REENTRANT>::contended_lock(bool allow_block_for_safepoint) {
  Thread* thread = Thread::current();
  if (allow_block_for_safepoint && thread->is_Java_thread()) {
    contended_lock_internal<true>(JavaThread::cast(thread));
  } else {
    contended_lock_internal<false>(nullptr);
  }
}

template void ShenandoahLockImpl<true>::contended_lock(bool allow_block_for_safepoint);
template void ShenandoahLockImpl<false>::contended_lock(bool allow_block_for_safepoint);

template<bool REENTRANT>
template<bool ALLOW_BLOCK>
void ShenandoahLockImpl<REENTRANT>::contended_lock_internal(JavaThread* java_thread) {
  assert(!ALLOW_BLOCK || java_thread != nullptr, "Must have a Java thread when allowing block.");
  // Spin this much, but only on multi-processor systems.
  int ctr = os::is_MP() ? 0xFF : 0;
  // Apply TTAS to avoid more expensive CAS calls if the lock is still held by other thread.
  while (Atomic::load(&_state) == locked ||
         Atomic::cmpxchg(&_state, unlocked, locked) != unlocked) {
    if (ctr > 0 && !SafepointSynchronize::is_synchronizing()) {
      // Lightly contended, spin a little if no safepoint is pending.
      SpinPause();
      ctr--;
    } else if (ALLOW_BLOCK) {
      ThreadBlockInVM block(java_thread);
      if (SafepointSynchronize::is_synchronizing()) {
        // If safepoint is pending, we want to block and allow safepoint to proceed.
        // Normally, TBIVM above would block us in its destructor.
        //
        // But that blocking only happens when TBIVM knows the thread poll is armed.
        // There is a window between announcing a safepoint and arming the thread poll
        // during which trying to continuously enter TBIVM is counter-productive.
        // Under high contention, we may end up going in circles thousands of times.
        // To avoid it, we wait here until local poll is armed and then proceed
        // to TBVIM exit for blocking. We do not SpinPause, but yield to let
        // VM thread to arm the poll sooner.
        while (SafepointSynchronize::is_synchronizing() &&
               !SafepointMechanism::local_poll_armed(java_thread)) {
          os::naked_yield();
        }
      } else {
        os::naked_yield();
      }
    } else {
      os::naked_yield();
    }
  }
}