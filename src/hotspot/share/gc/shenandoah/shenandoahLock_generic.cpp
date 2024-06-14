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

#include "gc/shenandoah/shenandoahLock_generic.hpp"
#include "runtime/atomic.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.inline.hpp"

void GenericShenandoahLock::contended_lock(bool allow_block_for_safepoint) {
  Thread* thread = Thread::current();
  if (thread->is_Java_thread()) {
    //Java threads spin a little before yielding and potentially blocking.
    constexpr uint32_t SPINS = 0x1F;
    if (allow_block_for_safepoint) {
      contended_lock_internal<true, SPINS>(thread);
    } else {
      contended_lock_internal<false, SPINS>(thread);
    }
  } else {
    // Non-Java threads are not allowed to block, and they spin hard
    // to progress quickly. The normal number of GC threads is low enough
    // for this not to have detrimental effect. This favors GC threads
    // a little over Java threads, which is good for GC progress under
    // extreme contention.
    contended_lock_internal<false, 0xFFF>(thread);
  }
}


template<bool ALLOW_BLOCK, uint32_t MAX_SPINS>
void GenericShenandoahLock::contended_lock_internal(Thread* thread) {
  assert(!ALLOW_BLOCK || thread->is_Java_thread(), "Must be a Java thread when allow block.");
  uint32_t ctr = os::is_MP() ? MAX_SPINS : 0; //Do not spin on single processor.
  uint32_t yield_ctr = 0;
  while (Atomic::load(&_state) == locked ||
      Atomic::cmpxchg(&_state, unlocked, locked) != unlocked) {
    if (ctr > 0 && !SafepointSynchronize::is_synchronizing()) {
      // Lightly contended, spin a little if SP it NOT synchronizing.
      SpinPause();
      ctr--;
    } else if (ALLOW_BLOCK && SafepointSynchronize::is_synchronizing()) {
      //We know SP is synchronizing and block is allosed,
      //yield to safepoint call to so VM will reach safepoint faster.
      ThreadBlockInVM block(JavaThread::cast(thread), true);
    } else {
      if ((++yield_ctr & 0x7F) == 0) {
#ifdef _WINDOWS
        os::naked_short_sleep(1);
#else
        os::naked_short_nanosleep(10000);
#endif 
      } else {
        os::naked_yield();
      }
    }
  }
}