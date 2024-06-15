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

#ifndef OS_LINUX_GC_SHENANDOAH_SHENANDOAHLOCK_LINUX_HPP
#define OS_LINUX_GC_SHENANDOAH_SHENANDOAHLOCK_LINUX_HPP

#include "runtime/os.hpp"
#include "runtime/atomic.hpp"

class LinuxShenandoahLock {
private:
  static const uint32_t unlocked = 0;
  static const uint32_t locked  = 1;
  static const uint32_t contended = 2;
  volatile uint32_t _state;
  volatile Thread* _owner;
  volatile int _contenders;

  uint32_t tryFastLock(int max_attempts) {
    assert(max_attempts > 0, "max_attempts must be greater than 0.");
    int ctr = os::is_MP() ? max_attempts : 1; //Only try cmpxchg once w/o spin when there is one processor.
    uint32_t current;
    while((current = Atomic::cmpxchg(&_state, unlocked, locked)) != unlocked && --ctr > 0) {
      SpinPause();
    }
    return current;
  }

public:
  LinuxShenandoahLock() : _state(0), _owner(nullptr), _contenders(0) {};
  void lock(bool allow_block_for_safepoint);
  void unlock();
  bool owned_by_self() {
    return _state != unlocked && _owner == Thread::current();
  }
};

#endif //OS_LINUX_GC_SHENANDOAH_SHENANDOAHLOCK_LINUX_HPP
