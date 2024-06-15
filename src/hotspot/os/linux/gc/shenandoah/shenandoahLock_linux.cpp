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
//    const struct timespec *timeout,  /*or: uint32_t val2 */
//    uint32_t *uaddr2, uint32_t val3);
static long futex_wake(volatile uint32_t *addr, uint32_t val) {
  return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, val, nullptr, nullptr, 0);
}

static long futex_wait(volatile uint32_t *addr, uint32_t val) {
  return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, nullptr, nullptr, 0);
}

void LinuxShenandoahLock::lock(bool allow_block_for_safepoint) {
    Thread* thread = Thread::current();
    assert(Atomic::load(&_owner) != thread, "reentrant locking attempt, would deadlock");
    // Try fast lock
    uint32_t current = tryFastLock(32);
    // When fast lock fails dive into contented lock.
    if (current != unlocked) {
        // Set lock state to contended if locked by others.
        if (Atomic::load(&_state) == locked)
            current = Atomic::xchg(&_state, contended);
        while (current != unlocked) {
            futex_wait(&_state, contended);
            current = Atomic::xchg(&_state, contended);
        }
    }
    assert(Atomic::load(&_state) != unlocked, "must not be unlocked");
    assert(Atomic::load(&_owner) == nullptr, "must not be owned");
    OrderAccess::fence();
    DEBUG_ONLY(Atomic::store(&_owner, Thread::current());)
}

void LinuxShenandoahLock::unlock() {
    assert(Atomic::load(&_owner) == Thread::current(), "sanity");
    DEBUG_ONLY(Atomic::store(&_owner, (Thread*)nullptr);)
    OrderAccess::fence();
    bool wake_up_some_one = false;
    if(Atomic::xchg(&_state, unlocked) == contended && os::is_MP()) {
        //Some threads are waiting, should be have been woken up after lock sate change,
        // now spin a little bit and hope some thread get the lock.
        int i = 64;
        while(i-- > 0) { 
            if(Atomic::load(&_state) != unlocked) {
                if (Atomic::cmpxchg(&_state, locked, contended) == locked) {
                    break;
                }
            }
            SpinPause();
        }
        wake_up_some_one = i < 0;
    }
    if (wake_up_some_one) {
        futex_wake(&_state, 1);
    }
}
