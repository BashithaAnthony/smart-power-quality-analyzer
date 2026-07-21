#include "AcquisitionPolicy.h"

#if ACQUISITION_POLICY_HOST_TEST

#include <assert.h>
#include <deque>
#include <stdio.h>
#include <vector>

namespace {

class FlushModel {
 public:
  explicit FlushModel(uint32_t capacity) : capacity_(capacity) {}

  void prepareStorage(uint32_t segmentCount) {
    assert(!accepting_);
    preparationEraseSliceCount_ =
        segmentCount * AcquisitionPolicy::eraseSlicesForSegment(65536U);
    preparationEraseYieldCount_ = preparationEraseSliceCount_;
    prepared_ = true;
  }

  void startAdmission() {
    assert(prepared_);
    accepting_ = true;
  }

  bool offer(uint32_t sequence) {
    if (!accepting_) {
      return false;
    }
    ++offered_;
    if (fifo_.size() >= capacity_) {
      ++dropped_;
      return false;
    }
    fifo_.push_back(sequence);
    ++accepted_;
    return true;
  }

  bool serviceOne(bool writeSucceeds,
                  uint32_t sequenceArrivingDuringFlash = 0U) {
    if (fifo_.empty()) {
      return false;
    }
    workerInFlight_ = true;
    if (sequenceArrivingDuringFlash != 0U) {
      offer(sequenceArrivingDuringFlash);
    }
    if (!writeSucceeds) {
      workerInFlight_ = false;
      return false;
    }
    durable_.push_back(fifo_.front());
    fifo_.pop_front();
    workerInFlight_ = false;
    ++workerYieldCount_;
    return true;
  }

  void stopAdmission() { accepting_ = false; }

  bool sequencesAreConsecutive() const {
    for (size_t index = 1U; index < durable_.size(); ++index) {
      if (durable_[index] != durable_[index - 1U] + 1U) {
        return false;
      }
    }
    return true;
  }

  uint64_t offered() const { return offered_; }
  uint64_t accepted() const { return accepted_; }
  uint64_t dropped() const { return dropped_; }
  uint64_t pending() const { return fifo_.size(); }
  uint64_t inFlight() const { return workerInFlight_ ? 1U : 0U; }
  uint64_t stored() const { return durable_.size(); }
  uint32_t preparationEraseSliceCount() const {
    return preparationEraseSliceCount_;
  }
  uint32_t preparationEraseYieldCount() const {
    return preparationEraseYieldCount_;
  }
  uint32_t activeEraseSliceCount() const { return 0U; }
  uint32_t workerYieldCount() const { return workerYieldCount_; }

 private:
  uint32_t capacity_;
  bool prepared_ = false;
  bool accepting_ = false;
  bool workerInFlight_ = false;
  uint64_t offered_ = 0U;
  uint64_t accepted_ = 0U;
  uint64_t dropped_ = 0U;
  uint32_t preparationEraseSliceCount_ = 0U;
  uint32_t preparationEraseYieldCount_ = 0U;
  uint32_t workerYieldCount_ = 0U;
  std::deque<uint32_t> fifo_;
  std::vector<uint32_t> durable_;
};

void testContinuousArrivalAndBoundedFlush() {
  FlushModel model(700U);
  model.prepareStorage(191U);
  model.startAdmission();
  uint32_t nextSequence = 1U;
  for (; nextSequence <= 600U; ++nextSequence) {
    assert(model.offer(nextSequence));
  }

  // Model packets arriving during bounded record programs. Data-segment
  // erases have already completed before admission, so none occur here.
  bool stopRequested = false;
  while (model.pending() != 0U) {
    const uint32_t arrivalDuringProgram =
        !stopRequested && model.stored() < 43U ? nextSequence++ : 0U;
    assert(model.serviceOne(true, arrivalDuringProgram));
    if (model.stored() == 100U) {
      // Stop closes admission but does not discard the active flush.
      model.stopAdmission();
      stopRequested = true;
      assert(!model.offer(nextSequence));
    }
  }

  assert(model.accepted() == model.stored());
  assert(model.dropped() == 0U);
  assert(model.pending() == 0U);
  assert(model.inFlight() == 0U);
  assert(model.sequencesAreConsecutive());
  assert(model.workerYieldCount() == model.stored());
  assert(model.preparationEraseSliceCount() == 3056U);
  assert(model.preparationEraseSliceCount() ==
         model.preparationEraseYieldCount());
  assert(model.activeEraseSliceCount() == 0U);
  assert(AcquisitionPolicy::acceptedRecordsAccounted(
      model.accepted(), model.stored(), model.pending(), model.inFlight()));
}

void testFailurePreservesLeaseAndNewArrival() {
  FlushModel model(700U);
  model.prepareStorage(191U);
  model.startAdmission();
  for (uint32_t sequence = 1U; sequence <= 10U; ++sequence) {
    assert(model.offer(sequence));
  }
  assert(!model.serviceOne(false, 11U));
  assert(model.pending() == 11U);
  assert(model.stored() == 0U);
  assert(model.inFlight() == 0U);
  while (model.pending() != 0U) {
    assert(model.serviceOne(true));
  }
  assert(model.stored() == 11U);
  assert(model.sequencesAreConsecutive());
}

void testOverflowAndFinalPartialSegment() {
  FlushModel overflow(700U);
  overflow.prepareStorage(191U);
  overflow.startAdmission();
  for (uint32_t sequence = 1U; sequence <= 700U; ++sequence) {
    assert(overflow.offer(sequence));
  }
  assert(!overflow.offer(701U));
  assert(overflow.dropped() == 1U);
  assert(AcquisitionPolicy::offeredRecordsAccounted(
      overflow.offered(), overflow.stored(), overflow.dropped(),
      overflow.pending(), overflow.inFlight()));

  FlushModel partial(700U);
  partial.prepareStorage(191U);
  partial.startAdmission();
  for (uint32_t sequence = 1U; sequence <= 15U; ++sequence) {
    assert(partial.offer(sequence));
  }
  while (partial.pending() != 0U) {
    assert(partial.serviceOne(true));
  }
  assert(partial.stored() == 15U);
  assert(partial.sequencesAreConsecutive());
}

void testPreparationAndCapacityPolicies() {
  using Phase = AcquisitionStoragePhase;
  assert(!AcquisitionPolicy::packetAdmissionAllowed(
      Phase::PreparingStorage, false));
  assert(!AcquisitionPolicy::packetAdmissionAllowed(
      Phase::PreparingStorage, true));
  assert(AcquisitionPolicy::packetAdmissionAllowed(Phase::Active, true));
  assert(!AcquisitionPolicy::packetAdmissionAllowed(Phase::Active, false));

  assert(AcquisitionPolicy::dataSegmentEraseAllowed(
      Phase::PreparingStorage, true));
  assert(!AcquisitionPolicy::dataSegmentEraseAllowed(
      Phase::PreparingStorage, false));
  assert(!AcquisitionPolicy::dataSegmentEraseAllowed(Phase::Active, true));
  assert(!AcquisitionPolicy::dataSegmentEraseAllowed(Phase::Stopping, true));
  assert(!AcquisitionPolicy::dataSegmentEraseAllowed(
      Phase::Finalizing, true));

  assert(!AcquisitionPolicy::preparedCapacityReached(2673U, 2674U));
  assert(AcquisitionPolicy::preparedCapacityReached(2674U, 2674U));
  assert(AcquisitionPolicy::preparedCapacityReached(2675U, 2674U));
  assert(AcquisitionPolicy::sequenceGapMagnitude(100U, 101U) == 0U);
  assert(AcquisitionPolicy::sequenceGapMagnitude(100U, 221U) == 120U);
  assert(AcquisitionPolicy::sequenceGapMagnitude(UINT32_MAX, 0U) == 0U);
  assert(AcquisitionPolicy::sequenceGapMagnitude(100U, 100U) == 1U);
  assert(AcquisitionPolicy::sequenceGapMagnitude(100U, 2U) == 1U);

  FlushModel model(700U);
  assert(!model.offer(1U));
  assert(model.offered() == 0U);
  model.prepareStorage(191U);
  assert(!model.offer(2U));
  model.startAdmission();
  assert(model.offer(3U));
}

}  // namespace

int main() {
  assert(AcquisitionPolicy::recordsForInterval(1U) == 10U);
  assert(AcquisitionPolicy::recordsForInterval(5U) == 50U);
  assert(AcquisitionPolicy::recordsForInterval(10U) == 100U);
  assert(AcquisitionPolicy::recordsForInterval(60U) == 600U);

  assert(AcquisitionPolicy::uartBytesForPauseUs(250000U) == 23040U);
  assert(AcquisitionPolicy::uartBufferedPauseCapacityUs() >= 355000U);
  assert(AcquisitionPolicy::uartFifoInterruptHeadroomUs() >= 1000U);

  assert(AcquisitionPolicy::serviceSlicesForRecords(0U) == 0U);
  assert(AcquisitionPolicy::serviceSlicesForRecords(1U) == 1U);
  assert(AcquisitionPolicy::serviceSlicesForRecords(600U) == 600U);
  assert(AcquisitionPolicy::writeSlicesForBytes(4428U) == 2U);
  assert(AcquisitionPolicy::eraseSlicesForSegment(65536U) == 16U);
  assert(AcquisitionPolicy::segmentsTouched(600U, 14U) == 43U);

  assert(AcquisitionPolicy::acceptedRecordsAccounted(600U, 590U, 9U, 1U));
  assert(AcquisitionPolicy::offeredRecordsAccounted(
      603U, 590U, 3U, 9U, 1U));
  assert(AcquisitionPolicy::acceptedRecordsAccounted(600U, 600U, 0U, 0U));
  assert(AcquisitionPolicy::offeredRecordsAccounted(
      603U, 600U, 3U, 0U, 0U));
  assert(!AcquisitionPolicy::acceptedRecordsAccounted(
      600U, 590U, 8U, 1U));

  uint32_t previousSequence = 0U;
  uint32_t duplicateCount = 0U;
  uint32_t gapCount = 0U;
  for (uint32_t sequence = 1U; sequence <= 600U; ++sequence) {
    if (previousSequence != 0U) {
      if (sequence == previousSequence) {
        ++duplicateCount;
      } else if (sequence != previousSequence + 1U) {
        ++gapCount;
      }
    }
    previousSequence = sequence;
  }
  assert(duplicateCount == 0U);
  assert(gapCount == 0U);

  testContinuousArrivalAndBoundedFlush();
  testFailurePreservesLeaseAndNewArrival();
  testOverflowAndFinalPartialSegment();
  testPreparationAndCapacityPolicies();

  puts("Acquisition policy tests: PASS");
  return 0;
}

#endif
