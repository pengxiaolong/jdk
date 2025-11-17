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

#include "gc/shenandoah/shenandoahAllocator.hpp"
#include "gc/shared/plab.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shared/workerThread.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"

ShenandoahAllocator::ShenandoahAllocator(ShenandoahFreeSet* free_set, ShenandoahFreeSetPartitionId alloc_partition_id, bool yield_to_safepoint):
  _alloc_region(nullptr),_retained_alloc_region(nullptr), _free_set(free_set), _alloc_partition_id(alloc_partition_id), _yield_to_safepoint(yield_to_safepoint) {
}

HeapWord* ShenandoahAllocator::attempt_allocation(ShenandoahAllocRequest& req, bool& in_new_region) {

  HeapWord* obj = attempt_allocation_in_alloc_regions(req, in_new_region);
  if (obj != nullptr) {
    return obj;
  }
  {
    ShenandoahHeapLocker locker(ShenandoahHeap::heap()->lock(), _yield_to_safepoint);
    obj = new_alloc_regions_and_allocate(&req, &in_new_region);
    if (obj != nullptr) {
      return obj;
    }
    // bail out, we are out of heap regions with enough space for the allocation reqeust.
    return nullptr;
  }
}

HeapWord* ShenandoahAllocator::attempt_allocation_in_alloc_regions(ShenandoahAllocRequest &req,
                                                                   bool &in_new_region) {
  HeapWord *obj = nullptr;
  ShenandoahHeapRegion* retained_alloc_region = _retained_alloc_region;
  if (retained_alloc_region != nullptr) {
    obj = atomic_allocate_in(retained_alloc_region, req, in_new_region);
    if (obj != nullptr) {
      return obj;
    }
  }

  ShenandoahHeapRegion* alloc_region = _alloc_region;
  if (alloc_region != nullptr) {
    obj = atomic_allocate_in(alloc_region, req, in_new_region);
    if (obj != nullptr) {
      return obj;
    }
  }

  return nullptr;
}

inline HeapWord* ShenandoahAllocator::atomic_allocate_in(ShenandoahHeapRegion* region, ShenandoahAllocRequest &req, bool &in_new_region) {
  HeapWord* obj = nullptr;
  size_t actual_size = req.size();
  if (req.is_lab_alloc()) {
    obj = region->allocate_lab_atomic(req, actual_size);
  } else {
    obj = region->allocate_atomic(actual_size, req);
  }
  if (obj != nullptr) {
    assert(actual_size > 0, "Must be");
    req.set_actual_size(actual_size);
    if (pointer_delta(obj, region->bottom()) == actual_size) {
      // Set to true if it is the first object/tlab allocated in the region.
      in_new_region = true;
    }
    if (req.is_gc_alloc()) {
      // For GC allocations, we advance update_watermark because the objects relocated into this memory during
      // evacuation are not updated during evacuation.  For both young and old regions r, it is essential that all
      // PLABs be made parsable at the end of evacuation.  This is enabled by retiring all plabs at end of evacuation.
      // TODO double check if race condition here could cause problem?
      region->set_update_watermark(region->top());
    }
  }
  return obj;
}

class ShenandoahHeapAccountingsUpdater : public StackObj {
  ShenandoahFreeSet*                _free_set;
  ShenandoahFreeSetPartitionId      _partition;

public:
  ShenandoahHeapAccountingsUpdater(ShenandoahFreeSet* free_set, ShenandoahFreeSetPartitionId partition) : _free_set(free_set), _partition(partition) { }

  ~ShenandoahHeapAccountingsUpdater() {
    switch (_partition) {
      case ShenandoahFreeSetPartitionId::Mutator:
        _free_set->recompute_total_used</* UsedByMutatorChanged */ true,
                             /* UsedByCollectorChanged */ false, /* UsedByOldCollectorChanged */ false>();
        _free_set->recompute_total_affiliated</* MutatorEmptiesChanged */ true, /* CollectorEmptiesChanged */ false,
                                   /* OldCollectorEmptiesChanged */ false, /* MutatorSizeChanged */ false,
                                   /* CollectorSizeChanged */ false, /* OldCollectorSizeChanged */ false,
                                   /* AffiliatedChangesAreYoungNeutral */ false, /* AffiliatedChangesAreGlobalNeutral */ false,
                                   /* UnaffiliatedChangesAreYoungNeutral */ false>();
        break;
      case ShenandoahFreeSetPartitionId::Collector:
        _free_set->recompute_total_used</* UsedByMutatorChanged */ false,
                             /* UsedByCollectorChanged */ true, /* UsedByOldCollectorChanged */ false>();
        _free_set->recompute_total_affiliated</* MutatorEmptiesChanged */ false, /* CollectorEmptiesChanged */ true,
                                   /* OldCollectorEmptiesChanged */ false, /* MutatorSizeChanged */ false,
                                   /* CollectorSizeChanged */ false, /* OldCollectorSizeChanged */ false,
                                   /* AffiliatedChangesAreYoungNeutral */ false, /* AffiliatedChangesAreGlobalNeutral */ false,
                                   /* UnaffiliatedChangesAreYoungNeutral */ false>();
        break;
      case ShenandoahFreeSetPartitionId::OldCollector:
        _free_set->recompute_total_used</* UsedByMutatorChanged */ false,
                             /* UsedByCollectorChanged */ false, /* UsedByOldCollectorChanged */ true>();
        _free_set->recompute_total_affiliated</* MutatorEmptiesChanged */ false, /* CollectorEmptiesChanged */ false,
                                   /* OldCollectorEmptiesChanged */ true, /* MutatorSizeChanged */ false,
                                   /* CollectorSizeChanged */ false, /* OldCollectorSizeChanged */ false,
                                   /* AffiliatedChangesAreYoungNeutral */ true, /* AffiliatedChangesAreGlobalNeutral */ false,
                                   /* UnaffiliatedChangesAreYoungNeutral */ true>();
        break;
      case ShenandoahFreeSetPartitionId::NotFree:
      default:
        assert(false, "won't happen");
    }
  }

};

HeapWord* ShenandoahAllocator::new_alloc_regions_and_allocate(ShenandoahAllocRequest* req,
                                                              bool* in_new_region) {
  ResourceMark rm;
  shenandoah_assert_heaplocked();
  assert(req == nullptr || in_new_region != nullptr, "Must be");
  HeapWord* obj = nullptr;
  if (req != nullptr) {
    obj = attempt_allocation_in_alloc_regions(*req, *in_new_region);
    if (obj != nullptr) {
      return obj;
    }
  }

  ShenandoahHeapAccountingsUpdater accountings_updater(_free_set, _alloc_partition_id);

  size_t min_req_byte_size = PLAB::max_size() * HeapWordSize;
  if (req != nullptr) {
    min_req_byte_size = (req->is_lab_alloc() ? req->min_size() : req->size()) * HeapWordSize;
  }

  ShenandoahHeapRegion* new_alloc_region = _free_set->reserve_new_alloc_region(_alloc_partition_id, min_req_byte_size);
  if (new_alloc_region != nullptr) {
    if (req != nullptr) {
      obj = atomic_allocate_in(new_alloc_region, *req, *in_new_region);
      assert(obj != nullptr, "Always succeed to allocate in new alloc region.");
      if (new_alloc_region->free() < PLAB::min_size_bytes()) {
        new_alloc_region->unset_active_alloc_region();
        return obj;
      }
    }

    ShenandoahHeapRegion* original_alloc_region = _alloc_region;
    OrderAccess::storestore();
    _alloc_region = new_alloc_region;

    if (_retained_alloc_region != nullptr && _retained_alloc_region->free() < PLAB::min_size_bytes()) {
      _retained_alloc_region->unset_active_alloc_region();
      _retained_alloc_region = nullptr;
    }

    if (original_alloc_region != nullptr) {
      if (original_alloc_region->free() >= PLAB::min_size_bytes()) {
        ShenandoahHeapRegion* region_to_unretire = original_alloc_region;
        ShenandoahHeapRegion* original_retained_alloc_region = _retained_alloc_region;
        if (original_retained_alloc_region == nullptr || original_retained_alloc_region->free() < original_alloc_region->free()) {
          _retained_alloc_region = original_alloc_region;
          region_to_unretire = original_retained_alloc_region;
        }

        if (region_to_unretire != nullptr) {
          region_to_unretire->unset_active_alloc_region();
          _free_set->partitions()->decrease_used(_alloc_partition_id, region_to_unretire->free());
          _free_set->partitions()->increase_region_counts(_alloc_partition_id, 1);
          _free_set->partitions()->unretire_to_partition(region_to_unretire, _alloc_partition_id);
        }
      } else {
        original_alloc_region->unset_active_alloc_region();
      }
    }
  }

  return obj;
}

HeapWord* ShenandoahAllocator::allocate(ShenandoahAllocRequest &req, bool &in_new_region) {
#ifdef ASSERT
  verify(req);
#endif // ASSERT
  if (ShenandoahHeapRegion::requires_humongous(req.size())) {
    in_new_region = true;
    {
      ShenandoahHeapLocker locker(ShenandoahHeap::heap()->lock(), _yield_to_safepoint);
      return _free_set->allocate_contiguous(req, req.type() != ShenandoahAllocRequest::_alloc_cds /*is_humongous*/);
    }
  } else {
    return attempt_allocation(req, in_new_region);
  }
  return nullptr;
}

void ShenandoahAllocator::release_alloc_regions() {
  assert_at_safepoint();
  shenandoah_assert_heaplocked();

  if (_retained_alloc_region != nullptr) {
    ShenandoahHeapRegion* r = _retained_alloc_region;
    _retained_alloc_region = nullptr;
    r->unset_active_alloc_region();
    size_t free_bytes = r->free();
    if (free_bytes >= PLAB::min_size_bytes()) {
      assert(free_bytes != ShenandoahHeapRegion::region_size_bytes(), "Can't be empty.");
      _free_set->partitions()->decrease_used(_alloc_partition_id, free_bytes);
      _free_set->partitions()->increase_region_counts(_alloc_partition_id, 1);
      _free_set->partitions()->unretire_to_partition(r, _alloc_partition_id);
    }
  }

  if (_alloc_region != nullptr) {
    ShenandoahHeapRegion* r = _alloc_region;
    _alloc_region = nullptr;
    r->unset_active_alloc_region();
    size_t free_bytes = r->free();
    if (free_bytes >= PLAB::min_size_bytes()) {
      if (free_bytes == ShenandoahHeapRegion::region_size_bytes()) {
        r->make_empty();
        r->set_affiliation(FREE);
        _free_set->partitions()->increase_empty_region_counts(_alloc_partition_id, 1);
      }
      _free_set->partitions()->decrease_used(_alloc_partition_id, free_bytes);
      _free_set->partitions()->increase_region_counts(_alloc_partition_id, 1);
      _free_set->partitions()->unretire_to_partition(r, _alloc_partition_id);
    }
  }
}

void ShenandoahAllocator::reserve_alloc_regions() {
  // shenandoah_assert_heaplocked();
  // new_alloc_regions_and_allocate(nullptr, nullptr);
}

ShenandoahMutatorAllocator::ShenandoahMutatorAllocator(ShenandoahFreeSet* free_set) :
  ShenandoahAllocator(free_set, ShenandoahFreeSetPartitionId::Mutator, true/* yield_to_safepoint*/) {
}

#ifdef ASSERT
void ShenandoahMutatorAllocator::verify(ShenandoahAllocRequest& req) {
  assert(req.is_mutator_alloc(), "Must be mutator alloc request.");
}

void ShenandoahCollectorAllocator::verify(ShenandoahAllocRequest& req) {
  assert(req.is_gc_alloc() && req.affiliation() == YOUNG_GENERATION, "Must be gc alloc request in young gen.");
}


void ShenandoahOldCollectorAllocator::verify(ShenandoahAllocRequest& req) {
  assert(req.is_gc_alloc() && req.affiliation() == OLD_GENERATION, "Must be gc alloc request in young gen.");
}
#endif // ASSERT


ShenandoahCollectorAllocator::ShenandoahCollectorAllocator(ShenandoahFreeSet* free_set) :
  ShenandoahAllocator(free_set, ShenandoahFreeSetPartitionId::Collector, false /* yield_to_safepoint*/) {
}

ShenandoahOldCollectorAllocator::ShenandoahOldCollectorAllocator(ShenandoahFreeSet* free_set) :
  ShenandoahAllocator(free_set, ShenandoahFreeSetPartitionId::OldCollector, false /* yield_to_safepoint*/) {
}