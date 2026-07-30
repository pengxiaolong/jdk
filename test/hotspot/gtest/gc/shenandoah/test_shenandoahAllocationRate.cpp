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

#include "unittest.hpp"
#include "gc/shared/gc_globals.hpp"

#include "gc/shenandoah/shenandoahAllocRate.inline.hpp"
#include "gc/shenandoah/shenandoahStripedCounter.inline.hpp"
#include "runtime/atomic.hpp"
#include "threadHelper.inline.hpp"

class ShenandoahMockClock {
public:
  static volatile jlong Counter;
  static jlong elapsed_counter() {
    const jlong result = Counter;
    Counter += NANOSECS_PER_SEC;
    return result;
  }

  static jlong elapsed_frequency() {
    return NANOSECS_PER_SEC;
  }
};

volatile jlong ShenandoahMockClock::Counter = 0;

class ShenandoahAllocationRateTest : public testing::Test {
protected:
  ShenandoahAllocationRateTest() {
    ShenandoahMockClock::Counter = 0;
  }

  template<typename Clock>
  static void allocate(ShenandoahAllocRate<Clock>& rate, size_t quantity) {
    rate.allocated(quantity);
  }
};

constexpr uint BASELINE_SAMPLES = 100;
constexpr uint RECENT_SAMPLES = 8;
constexpr uint MOMENTARY_SAMPLES = 2;
constexpr uint MINIMUM_SAMPLE_SIZE = 1024;

// The per-stripe sample trigger is clamped to a floor of 256K -- the smallest a max-TLAB cap can
// be. Event-driven sampling only fires once a single stripe crosses this per-stripe threshold, so
// a stripe must accumulate at least this many bytes before a sample is taken. Tests that exercise
// the sampling path therefore allocate in multiples of it, and their recorded rates are multiples
// of it rather than the old byte-granular values.
constexpr size_t SAMPLE_TRIGGER = 256 * K;

TEST_VM_F(ShenandoahAllocationRateTest, ignore_too_small_sample) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  rate.allocated(512);
  EXPECT_DOUBLE_EQ(rate.weighted_average(), 0);
}

TEST_VM_F(ShenandoahAllocationRateTest, two_second_average) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  allocate(rate, SAMPLE_TRIGGER); // t = 1
  allocate(rate, SAMPLE_TRIGGER); // t = 2
  EXPECT_DOUBLE_EQ(rate.weighted_average(), static_cast<double>(SAMPLE_TRIGGER));
}

TEST_VM_F(ShenandoahAllocationRateTest, accelerated_consumption_small_number_of_samples) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  allocate(rate, 1024);
  ShenandoahAnticipatedConsumption consumption = rate.snapshot(100, 1);
  EXPECT_DOUBLE_EQ(consumption.acceleration(), 0.0);
  EXPECT_DOUBLE_EQ(consumption.predicted_rate(), 0.0);
  EXPECT_EQ(consumption.accelerated_consumption(), 0UL);
}

TEST_VM_F(ShenandoahAllocationRateTest, accelerated_consumption_uniform_rate) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  for (uint i = 0; i < BASELINE_SAMPLES; ++i) {
    allocate(rate, SAMPLE_TRIGGER);
  }

  const double rate_per_tick = static_cast<double>(SAMPLE_TRIGGER);
  ShenandoahAnticipatedConsumption consumption = rate.snapshot(100, 1);
  EXPECT_DOUBLE_EQ(rate.weighted_average(), rate_per_tick);  // Average rate, one trigger per tick
  EXPECT_DOUBLE_EQ(consumption.acceleration(), 0.0);   // No acceleration, rate is constant
  EXPECT_DOUBLE_EQ(consumption.momentary_rate(), rate_per_tick);  // Momentary rate is the same as the average
  EXPECT_EQ(consumption.momentary_consumption(), 100 * SAMPLE_TRIGGER); // 100 clock ticks
  EXPECT_EQ(consumption.accelerated_consumption(), 0UL);
}

TEST_VM_F(ShenandoahAllocationRateTest, accelerated_consumption_momentary_spike) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  for (uint i = 0; i < BASELINE_SAMPLES; ++i) {
    allocate(rate, 2 * SAMPLE_TRIGGER);
  }

  for (uint i = 0; i < RECENT_SAMPLES; ++i) {
    allocate(rate, SAMPLE_TRIGGER);
  }

  for (uint i = 0; i < MOMENTARY_SAMPLES + 1; ++i) {
    allocate(rate, 2 * SAMPLE_TRIGGER);
  }

  // Here we simulate a situation where we are returning from a lull (avg one trigger/s) back
  // to the baseline average allocation rate (two triggers/s). The momentary rate will reflect
  // the recent samples, but we will not consider this to be an acceleration.
  ShenandoahAnticipatedConsumption consumption = rate.snapshot(100, 1);
  EXPECT_DOUBLE_EQ(consumption.acceleration(), 0.0);
  EXPECT_DOUBLE_EQ(consumption.momentary_rate(), static_cast<double>(2 * SAMPLE_TRIGGER));
  EXPECT_EQ(consumption.momentary_consumption(), 100 * 2 * SAMPLE_TRIGGER);
  EXPECT_EQ(consumption.accelerated_consumption(), 0UL);
}

TEST_VM_F(ShenandoahAllocationRateTest, event_driven_sampling_single_dominant_allocator) {
  // Single mutator: one stripe allocates, other stripes stay empty.
  ShenandoahStripedCounter stripes;
  if (stripes.num_stripes() == 1) {
    // Regression requires multiple stripes.
    return;
  }

  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  // Multiple epochs prove the allocation-path trigger re-fires without force_update(). Each add
  // crosses the single dominant stripe's per-stripe threshold, so every epoch takes a sample.
  constexpr size_t alloc_size = SAMPLE_TRIGGER;
  constexpr size_t epochs = 4;
  for (size_t i = 0; i < epochs; ++i) {
    allocate(rate, alloc_size);
  }

  // Old one-shot trigger left the average at zero until force_update().
  EXPECT_GT(rate.weighted_average(), 0.0);
}

TEST_VM_F(ShenandoahAllocationRateTest, event_driven_sampling_rearms_when_floor_lowered) {
  // Lowering the floor must re-arm sampling. The per-stripe trigger is clamped to SAMPLE_TRIGGER,
  // so a crossing under a high floor still fires the trigger but bails on the sum()-below-floor
  // check; once the floor is lowered, the next crossing takes the sample.
  constexpr size_t high_floor = 4 * SAMPLE_TRIGGER;
  constexpr size_t low_floor = MINIMUM_SAMPLE_SIZE;

  ShenandoahAllocRate<ShenandoahMockClock> rate(high_floor, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);

  // Cross one per-stripe trigger, but stay below the high floor: the trigger fires and bails.
  allocate(rate, SAMPLE_TRIGGER);
  EXPECT_DOUBLE_EQ(rate.weighted_average(), 0.0);  // nothing drained yet

  // A GC lowers the floor.
  rate.set_minimum_sample_size(low_floor);

  // New crossings under the lowered floor must sample without force_update(). The first recorded
  // sample carries zero weight, so cross the trigger a few times to move the weighted average.
  for (uint i = 0; i < 3; ++i) {
    allocate(rate, SAMPLE_TRIGGER);
  }

  EXPECT_GT(rate.weighted_average(), 0.0);
}

// Concurrent multi-threaded sampling. Many threads drive allocated() past the aggregate floor at
// the same time, so distinct JavaThreads spread across stripes and stay hot simultaneously. This is
// the regime the sampling guard is written for: contended try_lock (multiple threads cross their
// per-stripe share at once, only one wins the lock), multi-stripe sum() aggregation (the floor is
// reached by several occupied stripes, not one), and the drain-race clause (one thread's add()
// captures a stripe value that another thread drains before the first takes the lock).
class ConcurrentAllocators {
public:
  static constexpr int kThreads = 8;
  static constexpr size_t kPerThreadEpochs = 500;
  static constexpr size_t kAllocSize = 64;
  // Every thread allocates this many bytes; the grand total spans many minimum-sample-size epochs.
  static constexpr size_t kPerThreadBytes = MINIMUM_SAMPLE_SIZE * kPerThreadEpochs;
};

TEST_VM_F(ShenandoahAllocationRateTest, event_driven_sampling_concurrent_allocators) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);

  auto worker = [&](Thread*, int) {
    for (size_t allocated = 0; allocated < ConcurrentAllocators::kPerThreadBytes;
         allocated += ConcurrentAllocators::kAllocSize) {
      rate.allocated(ConcurrentAllocators::kAllocSize);
    }
  };
  TestThreadGroup<decltype(worker)> ttg(worker, ConcurrentAllocators::kThreads);
  ttg.doit();
  ttg.join();

  // No force_update() was called: every sample came from the contended allocation path. Across
  // thousands of epochs driven by all threads, sampling must have fired and drained repeatedly.
  EXPECT_GT(rate.weighted_average(), 0.0);
}

// Concurrent skew: a few threads hold their stripes just below the per-stripe share and keep them
// hot (spinning at the barrier), while a heavy thread pushes the aggregate over the floor. The
// sample can then only be taken because sum() aggregates the heavy stripe with the held stripes --
// exercising the multi-stripe floor crossing, not a single dominant stripe.
class ConcurrentSkew {
public:
  static constexpr int kHolderThreads = 6;
  static constexpr size_t kHeavyEpochs = 300;
  static constexpr size_t kAllocSize = SAMPLE_TRIGGER;
};

TEST_VM_F(ShenandoahAllocationRateTest, event_driven_sampling_concurrent_skew) {
  ShenandoahStripedCounter stripes;
  if (stripes.num_stripes() == 1) {
    // A multi-stripe aggregate crossing is only meaningful with more than one stripe.
    return;
  }

  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);

  // Each holder adds just under the per-stripe share once, then stays live for the whole run, so
  // several stripes remain simultaneously occupied below their individual share. Their adds never
  // cross a share alone, but they contend on the counter and feed sum().
  Atomic<bool> stop(false);
  const size_t per_stripe_share = MINIMUM_SAMPLE_SIZE / stripes.num_stripes();
  const size_t holder_target = per_stripe_share > 2 ? per_stripe_share - 1 : 1;
  auto holder = [&](Thread*, int) {
    rate.allocated(holder_target);
    while (!stop.load_relaxed()) { /* keep the thread (and its stripe) live */ }
  };
  TestThreadGroup<decltype(holder)> holders(holder, ConcurrentSkew::kHolderThreads);
  holders.doit();

  // Heavy stream on the main thread's own stripe. Each add crosses a per-stripe trigger, and added
  // to the held stripes takes sum() over the floor; the re-armed trigger must sample off the
  // allocation path.
  for (size_t epoch = 0; epoch < ConcurrentSkew::kHeavyEpochs; ++epoch) {
    allocate(rate, ConcurrentSkew::kAllocSize);
  }

  stop.store_relaxed(true);
  holders.join();

  EXPECT_GT(rate.weighted_average(), 0.0);
}

TEST_VM_F(ShenandoahAllocationRateTest, accelerated_consumption_accelerating) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  for (uint i = 0; i < BASELINE_SAMPLES; ++i) {
    allocate(rate, SAMPLE_TRIGGER);
  }

  for (uint i = 0; i < RECENT_SAMPLES; ++i) {
    allocate(rate, 2 * SAMPLE_TRIGGER);
  }

  for (uint i = 0; i < MOMENTARY_SAMPLES + 1; ++i) {
    allocate(rate, 4 * SAMPLE_TRIGGER);
  }

  // A rising rate (one -> two -> four triggers per tick). This evaluates the acceleration.
  ShenandoahAnticipatedConsumption consumption = rate.snapshot(100, 1);
  EXPECT_GE(consumption.acceleration(), 180.0 * SAMPLE_TRIGGER / 512);
  // Predicted rate approaches the last recorded rate (four triggers), modulo fp slack.
  EXPECT_GE(consumption.predicted_rate(), static_cast<double>(4 * SAMPLE_TRIGGER) - 1.0);
  EXPECT_GE(consumption.accelerated_consumption(), 100 * 2 * SAMPLE_TRIGGER);
  EXPECT_EQ(consumption.momentary_consumption(), 0UL);
}

TEST_VM_F(ShenandoahAllocationRateTest, accelerated_consumption_decelerating) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  for (uint i = 0; i < RECENT_SAMPLES; ++i) {
    allocate(rate, 2 * SAMPLE_TRIGGER);
  }

  for (uint i = 0; i < MOMENTARY_SAMPLES + 1; ++i) {
    allocate(rate, SAMPLE_TRIGGER);
  }

  // In this setup, the allocation rate is declining.
  ShenandoahAnticipatedConsumption consumption = rate.snapshot(100, 1);
  EXPECT_DOUBLE_EQ(consumption.acceleration(), 0.0);
  EXPECT_DOUBLE_EQ(consumption.momentary_rate(), static_cast<double>(SAMPLE_TRIGGER));
  EXPECT_EQ(consumption.momentary_consumption(), 100 * SAMPLE_TRIGGER);
}

TEST_VM_F(ShenandoahAllocationRateTest, force_updates) {
  ShenandoahAllocRate<ShenandoahMockClock> rate(MINIMUM_SAMPLE_SIZE, BASELINE_SAMPLES, RECENT_SAMPLES, MOMENTARY_SAMPLES);
  for (uint i = 0; i < BASELINE_SAMPLES; ++i) {
    allocate(rate, SAMPLE_TRIGGER);
  }
  EXPECT_DOUBLE_EQ(rate.weighted_average(), static_cast<double>(SAMPLE_TRIGGER));

  // Now simulate an equal number of seconds passing without any allocations. This
  // should decay our baseline average back to zero.
  for (uint i = 0; i < BASELINE_SAMPLES; ++i) {
    rate.force_update();
  }
  EXPECT_DOUBLE_EQ(rate.weighted_average(), 0.0);
}

