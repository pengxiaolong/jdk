/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHALLOCATOR_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHALLOCATOR_HPP

#include "gc/shared/gc_globals.hpp"
#include "utilities/globalDefinitions.hpp"

class ShenandoahFreeSet;
class ShenandoahRegionPartitions;
enum class ShenandoahFreeSetPartitionId : uint8_t;
class ShenandoahAllocRequest;

class ShenandoahAllocator : public CHeapObj<mtGC> {
protected:

  ShenandoahHeapRegion* volatile    _alloc_region;
  ShenandoahHeapRegion* volatile    _retained_alloc_region;
  ShenandoahFreeSet*                _free_set;
  ShenandoahFreeSetPartitionId      _alloc_partition_id;
  bool                              _yield_to_safepoint;

  // Attempt to allocate
  // It will try to allocate in alloc regions first, if fails it will try to get new alloc regions from free-set
  // and allocate with in the region got from free-set.
  HeapWord* attempt_allocation(ShenandoahAllocRequest& req, bool& in_new_region);

  // Attempt to allocate in shared alloc regions, the allocation attempt is done with atomic operation w/o
  // holding heap lock.
  HeapWord* attempt_allocation_in_alloc_regions(ShenandoahAllocRequest& req, bool& in_new_region);

  // Allocate in a region with atomic.
  HeapWord* atomic_allocate_in(ShenandoahHeapRegion* region, ShenandoahAllocRequest &req, bool &in_new_region);

  // Refill new alloc regions, allocate the object in the new alloc region.
  HeapWord* new_alloc_regions_and_allocate(ShenandoahAllocRequest* req, bool* in_new_region);
#ifdef ASSERT
  virtual void verify(ShenandoahAllocRequest& req) { }
#endif

public:
  ShenandoahAllocator(ShenandoahFreeSet* free_set, ShenandoahFreeSetPartitionId alloc_partition_id, bool yield_to_safepoint);
  virtual ~ShenandoahAllocator() { }

  // Handle the allocation request.
  virtual HeapWord* allocate(ShenandoahAllocRequest& req, bool& in_new_region);
  void release_alloc_regions();
  void reserve_alloc_regions();
};

/*
 * Allocator impl for mutator
 */
class ShenandoahMutatorAllocator : public ShenandoahAllocator {
#ifdef ASSERT
  void verify(ShenandoahAllocRequest& req) override;
#endif // ASSERT

public:
  ShenandoahMutatorAllocator(ShenandoahFreeSet* free_set);
};

class ShenandoahCollectorAllocator : public ShenandoahAllocator {
#ifdef ASSERT
  void verify(ShenandoahAllocRequest& req) override;
#endif // ASSERT
public:
  ShenandoahCollectorAllocator(ShenandoahFreeSet* free_set);
};

// TODO ShenandoahOldCollectorAllocator doesn't handle plab alloc property yet because of
class ShenandoahOldCollectorAllocator : public ShenandoahAllocator {
#ifdef ASSERT
  void verify(ShenandoahAllocRequest& req) override;
#endif // ASSERT
public:
  ShenandoahOldCollectorAllocator(ShenandoahFreeSet* free_set);
};

#endif //SHARE_GC_SHENANDOAH_SHENANDOAHALLOCATOR_HPP
