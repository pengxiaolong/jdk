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
 *
 */

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHPARTITIONALLOCATOR_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHPARTITIONALLOCATOR_HPP

#include "gc/shared/tlab_globals.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "memory/allocation.hpp"
#include "utilities/powerOfTwo.hpp"

// ShenandoahPartitionAllocator allocates memory for one free-set partition. The fast path is
// lock-free, it caches a small set of "alloc regions" (a stripe array) and bump-allocates within
// them using CAS on the region's atomic top. To spread CAS contention, each thread maps to a
// per-thread slot (stored in thread-local data) and the fast path probes ONLY that slot.
// When the fast path fails, the thread try to acquire heap lock, it probes sibling slots if the
// lock is currently held by other thread. After acquiring heap-locked, the slow path takes
// a fresh region from the free set, allocates from it, and installs it into the thread's
// slot for subsequent lock-free use.
template<ShenandoahFreeSetPartitionId PARTITION>
class ShenandoahPartitionAllocator : public CHeapObj<mtGC> {
  friend class VMStructs;

public:
  static constexpr uint32_t MAX_ALLOC_REGIONS = 32;

private:
  ShenandoahFreeSet* const _free_set;

  // Clamp to [1, MAX_ALLOC_REGIONS] and round down to a power of 2, so _alloc_region_count is
  // always safe to use as a bitmask size regardless of what the caller (or an ergonomic
  // derivation with a non-power-of-2 input, e.g. ParallelGCThreads) passed in.
  static uint32_t clamped_alloc_region_count(uint32_t alloc_region_count) {
    return round_down_power_of_2(MIN2(MAX2(alloc_region_count, 1u), MAX_ALLOC_REGIONS));
  }

  // Number of alloc-region stripe slots in use for this partition, a power of two.
  uint32_t const _alloc_region_count;
  uint32_t const _alloc_region_slot_mask;

  // Stripe array of cached alloc regions. Each slot holds a region with remaining capacity that is
  // bump-allocated lock-free via CAS, or nullptr when the slot is empty. A slot is cleared under the
  // heap lock after an allocation attempt observes too little remaining capacity, or explicitly by
  // release_alloc_region.
  Atomic<ShenandoahHeapRegion*> _alloc_regions[MAX_ALLOC_REGIONS];

  // Return this thread's stripe slot, assigning a stable per-thread slot on first use so different
  // threads map to different alloc regions. `thread` is the already-resolved current thread, passed
  // in to avoid a repeated Thread::current() on the allocation fast path.
  uint32_t alloc_region_slot(Thread* thread);

  // Under-lock scan of `count` stripe slots starting at start_slot and wrapping around the slot
  // array, used when the free set has no region of its own to hand out: a sibling slot may still
  // have room. Callers pass start_slot = own_slot + 1 and count = _alloc_region_count - 1 to scan
  // every OTHER slot (the own slot was already probed lock-free and can only have been retired
  // since). Collector partitions call this before stealing from the mutator; the mutator calls it as
  // a last resort. Returns the allocation, or nullptr if no slot in the range could satisfy it.
  template<bool HEAP_LOCKED>
  HeapWord* try_allocate_in_alloc_regions(ShenandoahAllocRequest& req, bool& in_new_region, uint32_t start_slot, uint32_t count);

  // Uninstall the occupant from the stripe slot.
  void uninstall_alloc_region(uint32_t slot, ShenandoahHeapRegion* occupant);

  // Try to install freshly-allocated new_region into stripe slot as the active alloc region(heap lock held).
  // Returns true if new_region became the slot's active alloc region.
  bool try_install_alloc_region(uint32_t slot, ShenandoahHeapRegion* occupant, ShenandoahHeapRegion* new_region);

  // Allocate within a single region; the caller must guarantee the region has enough free
  // capacity for the request. Handles LAB sizing, updates partition accounting via
  // ShenandoahFreeSet, and retires the region if remaining capacity drops below PLAB::min_size().
  // retired_after_alloc is set to true if the region is retired.
  HeapWord* allocate_in(ShenandoahHeapRegion* r,
                        ShenandoahAllocRequest& req,
                        bool& retired_after_alloc);

  // Try a lock-free CAS allocation in region r. This helper only performs the bump; retirement is
  // handled separately from a post-attempt free-space snapshot after the heap lock is acquired.
  HeapWord* try_atomic_allocate_in(ShenandoahHeapRegion* r, ShenandoahAllocRequest& req);

  // Retire (deactivate + reconcile) the region in stripe slot; heap lock held.
  void release_alloc_region(uint32_t slot);

public:
  ShenandoahPartitionAllocator(ShenandoahFreeSet* free_set, uint32_t alloc_region_count);

  uint32_t alloc_region_count() const { return _alloc_region_count; }

  // Allocate from this partition. Returns nullptr if partition cannot satisfy the request.
  HeapWord* allocate(ShenandoahAllocRequest& req, bool& in_new_region);

  // Drop all cached alloc regions. Must be called before the free set is rebuilt,
  // since rebuild can change region affiliation/membership and invalidate the cache.
  void release_alloc_regions();

  // Proactively reserve regions for all empty stripe slots. This is used for mutators after the
  // final-mark free-set rebuild, while the heap lock is held, so post-pause TLAB replenishment can
  // use the lock-free allocation path instead of contending for the heap lock.
  void reserve_alloc_regions();

  // Return the remaining free bytes in this thread's stripe alloc region, or max_tlab_size if the
  // region is empty/absent or too small for a TLAB. Used by unsafe_max_tlab_alloc() to hint TLAB
  // sizing: if the current region can still fit a TLAB, report its free space (capped at max_tlab)
  // so the TLAB machinery doesn't request an oversized refill that would waste the region tail.
  size_t unsafe_max_tlab_alloc(Thread* thread) {
    uint32_t slot = alloc_region_slot(thread);
    ShenandoahHeapRegion* r = _alloc_regions[slot].load_relaxed();
    if (r != nullptr) {
      size_t free_bytes = r->free_relaxed();
      if (free_bytes >= MinTLABSize) {
        return MIN2(free_bytes, ShenandoahHeapRegion::max_tlab_size_bytes());
      }
    }
    return ShenandoahHeapRegion::max_tlab_size_bytes();
  }

  // Total remaining bytes of all the alloc regions held by the allocator.
  // This is a best-effort estimate consumed by saturating-subtraction accounting readers, so it uses
  // fully relaxed reads on the hottest scan path: load_relaxed for the slot pointer and free_relaxed()
  // (relaxed _atomic_top) for its free bytes.
  size_t remnant_bytes() const {
    const size_t min_free_bytes = ShenandoahHeap::plab_min_size() * HeapWordSize;
    size_t total = 0;
    for (uint32_t i = 0; i < _alloc_region_count; i++) {
      ShenandoahHeapRegion* r = _alloc_regions[i].load_relaxed();
      if (r != nullptr) {
        size_t free_bytes = r->free_relaxed();
        if (free_bytes >= min_free_bytes) {
          total += free_bytes;
        }
      }
    }
    return total;
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHPARTITIONALLOCATOR_HPP
