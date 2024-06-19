/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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
 */

#include "precompiled.hpp"

#include "shenandoahLock_linux.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/os.hpp"
#include "runtime/atomic.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include <sys/syscall.h>
#include <linux/futex.h>

// 32-bit RISC-V has no SYS_futex syscall.
#ifdef RISCV32
  #if !defined(SYS_futex) && defined(SYS_futex_time64)
    #define SYS_futex SYS_futex_time64
  #endif
#endif

//long syscall(SYS_futex, uint32_t *uaddr, int futex_op, uint32_t val,
//    const struct timespec *timeout,  <or: uint32_t val2>
//    uint32_t *uaddr2, uint32_t val3);
static long futex_wake(volatile uint32_t *addr, uint32_t val) {
  return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, val, nullptr, nullptr, 0);
}

static long futex_wait(volatile uint32_t *addr, uint32_t val) {
  timeout.tv_sec = 0;
  timeout.tv_nsec = 10000;
  return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, &timeout, nullptr, 0);
}

void LinuxShenandoahLock::lock(bool allow_block_for_safepoint) {
    Thread* const thread = Thread::current();
    assert(Atomic::load(&_owner) != thread, "reentrant locking attempt, would deadlock");
    // Try fast lock
    uint32_t current = Atomic::cmpxchg(&_state, unlocked, locked);
    if (current != unlocked) { // When fast lock fails dive into contented lock.
        if (thread->is_Java_thread()) {
            if (allow_block_for_safepoint) contended_lock<true, true>(current);
            else contended_lock<true, false>(current);
        }
        else contended_lock<false, false>(current);
    }
    assert(Atomic::load(&_state) != unlocked, "must not be unlocked");
    assert(Atomic::load(&_owner) == nullptr, "must not be owned");
    OrderAccess::fence();
    Atomic::store(&_owner, Thread::current());
    if(SafepointSynchronize::is_synchronizing()) {
      const char* owner_name = thread -> is_Java_thread() ? "JavaThread" : thread->name();
      log_info(gc)("%s (" PTR_FORMAT ") has acquired lock (" PTR_FORMAT ") during SP synchronizing", owner_name, p2i(thread), p2i(this));
    }
}

template<bool IS_JAVA_THREAD, bool ALLOW_BLOCK>
void LinuxShenandoahLock::contended_lock(uint32_t &current) {
  while (current != unlocked) {
    if (Atomic::cmpxchg(&_state, locked, contended) == unlocked) {
      current = Atomic::xchg(&_state, contended); //An immediate attempt when _state is unlocked
    } else {
      if (IS_JAVA_THREAD && ALLOW_BLOCK) {
        // Prepare to block and allow safepoints while blocked
        ThreadBlockInVM block(JavaThread::current(), true);
        OSThreadWaitState osts(JavaThread::current()->osthread(), false /* not in Object.wait() */);
        Atomic::add(&_contenders, 1);
        futex_wait(&_state, contended);
        Atomic::add(&_contenders, -1);
        if (SafepointSynchronize::is_synchronizing()) {
          Thread* owner = Atomic::load(&_owner);
          const char* owner_name = owner == nullptr ? "Nobody" : (owner -> is_Java_thread() ? "JavaThread" : owner->name());
          log_info(gc)("Java thread (" PTR_FORMAT ") has been woken up during SP synchronizing, lock (" PTR_FORMAT ") is held by %s (" PTR_FORMAT ")", p2i(JavaThread::current()), p2i(this), owner_name, p2i(owner));
        }
      } else {
        Atomic::add(&_contenders, 1);
        futex_wait(&_state, contended);
        Atomic::add(&_contenders, -1);
      }
      current = Atomic::xchg(&_state, contended);
    }
  }
}

void LinuxShenandoahLock::unlock() {
  Thread* thread = Thread::current();
  assert(Atomic::load(&_owner) == thread, "sanity");
  Atomic::store(&_owner, (Thread*)nullptr);
  OrderAccess::fence();
  Atomic::xchg(&_state, unlocked);

  Thread* thread = Thread::current();
  if(SafepointSynchronize::is_synchronizing()) {
    const char* owner_name = thread -> is_Java_thread() ? "JavaThread" : thread->name();
    log_info(gc)("%s (" PTR_FORMAT ") has release lock (" PTR_FORMAT ") during SP synchronizing", owner_name, p2i(thread), p2i(this));
  }

  int i = os::is_MP() ? Atomic::load(&_contenders) : 0;
  for (;;) {
    if(i <= 0) break;
    if (Atomic::load(&_contenders) == 0) return;
    if (Atomic::load(&_state) != unlocked) {
      Atomic::cmpxchg(&_state, locked, contended);
      return;
    }
    SpinPause();
    i--;
  }
  if (Atomic::load(&_contenders) > 0) futex_wake(&_state, 1);
}
