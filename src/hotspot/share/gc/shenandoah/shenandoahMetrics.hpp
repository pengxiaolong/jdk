/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHMETRICS_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHMETRICS_HPP

#include "gc/shenandoah/shenandoahHeap.hpp"

class ShenandoahMetricsSnapshot : public StackObj {
private:
  ShenandoahHeap* _heap;
  size_t _used_before, _used_after;
  double _if_before, _if_after;
  double _ef_before, _ef_after;

public:
  ShenandoahMetricsSnapshot();

  void snap_before();
  void snap_after();

  bool is_good_progress(ShenandoahGeneration *generation);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHMETRICS_HPP
