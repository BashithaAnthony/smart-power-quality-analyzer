#include "SessionLogger.h"

#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "AcquisitionDiagnostics.h"
#include "AcquisitionPolicy.h"

namespace {

constexpr uint32_t kWorkerStackSizeBytes = 8192U;
constexpr UBaseType_t kWorkerPriority = 1U;
constexpr uint32_t kRecordsPerWorkerBatch =
    ACQUISITION_FLASH_RECORDS_PER_SERVICE_SLICE;
constexpr uint32_t kEarlyFlushPercent = 80U;
constexpr uint32_t kEarlyFlushOccupancy =
    (SESSION_FIFO_CAPACITY * kEarlyFlushPercent) / 100U;
constexpr uint16_t kStm32PacketFormatVersion = 1U;
constexpr uint16_t kStage3RecordFlags = 0U;
constexpr uint32_t kWorkerNotification = 1U;

static_assert(kEarlyFlushOccupancy == 560U,
              "The 80 percent early-flush threshold must be 560 records");
static_assert(SESSION_PACKET_BYTES == FLASH_RECORD_PAYLOAD_BYTES,
              "FIFO packets must exactly match flash-record payloads");
static_assert(kRecordsPerWorkerBatch == 1U,
              "One worker pass must not drain an entire flash backlog");

#if SESSION_LOGGER_DEBUG
void printUint64(uint64_t value) {
  Serial.printf("%llu", static_cast<unsigned long long>(value));
}
#endif

uint32_t boundedBatch(uint32_t occupancy) {
  return occupancy < kRecordsPerWorkerBatch
             ? occupancy
             : kRecordsPerWorkerBatch;
}

}  // namespace

bool SessionLogger::begin(SessionStorage& storage,
                          WallClockService& wallClock) {
  portENTER_CRITICAL(&mux_);
  if (initialized_) {
    const bool sameServices = storage_ == &storage &&
                              wallClock_ == &wallClock;
    portEXIT_CRITICAL(&mux_);
    return sameServices;
  }
  storage_ = &storage;
  wallClock_ = &wallClock;
  portEXIT_CRITICAL(&mux_);

  if (!storage.isAvailable()) {
    portENTER_CRITICAL(&mux_);
    state_ = SessionLoggerState::Error;
    lastError_ = SessionLoggerError::StorageNotAvailable;
    portEXIT_CRITICAL(&mux_);
    Serial.println("Session logger error: pqlog storage is unavailable");
    return false;
  }

  const bool psramAvailable = psramFound();
  portENTER_CRITICAL(&mux_);
  psramAvailable_ = psramAvailable;
  portEXIT_CRITICAL(&mux_);

  if (!psramAvailable) {
    portENTER_CRITICAL(&mux_);
    state_ = SessionLoggerState::Error;
    lastError_ = SessionLoggerError::PsramNotAvailable;
    portEXIT_CRITICAL(&mux_);
    Serial.println("Session logger error: PSRAM is unavailable");
    return false;
  }

  if (!fifo_.begin(SESSION_FIFO_CAPACITY)) {
    portENTER_CRITICAL(&mux_);
    state_ = SessionLoggerState::Error;
    lastError_ = SessionLoggerError::PsramAllocationFailed;
    portEXIT_CRITICAL(&mux_);
    Serial.println("Session logger error: PSRAM FIFO allocation failed");
    return false;
  }

  packetCopyBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(
      SESSION_PACKET_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  encodedRecordBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(
      FLASH_RECORD_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  readbackBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(
      FLASH_RECORD_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  if (packetCopyBuffer_ == nullptr || encodedRecordBuffer_ == nullptr ||
      readbackBuffer_ == nullptr) {
    releaseWorkerBuffers();
    portENTER_CRITICAL(&mux_);
    state_ = SessionLoggerState::Error;
    lastError_ = SessionLoggerError::WorkerBufferAllocationFailed;
    portEXIT_CRITICAL(&mux_);
    Serial.println("Session logger error: storage worker buffer allocation failed");
    return false;
  }

  uint32_t bootId = esp_random();
  if (bootId == 0U) {
    bootId = 1U;
  }

  portENTER_CRITICAL(&mux_);
  clearSessionCountersLocked();
  bootId_ = bootId;
  lastError_ = SessionLoggerError::None;
  portEXIT_CRITICAL(&mux_);
  synchronizeRecoveredState(storage.getStatus());

  BaseType_t workerCore = tskNO_AFFINITY;
#if !CONFIG_FREERTOS_UNICORE
  const BaseType_t setupCore = xPortGetCoreID();
  workerCore = setupCore == 0 ? 1 : 0;
#endif

  const BaseType_t taskResult = xTaskCreatePinnedToCore(
      workerTaskEntry,
      "StorageWorker",
      kWorkerStackSizeBytes,
      this,
      kWorkerPriority,
      &workerTaskHandle_,
      workerCore);
  if (taskResult != pdPASS) {
    workerTaskHandle_ = nullptr;
    releaseWorkerBuffers();
    portENTER_CRITICAL(&mux_);
    state_ = SessionLoggerState::Error;
    lastError_ = SessionLoggerError::WorkerTaskCreationFailed;
    portEXIT_CRITICAL(&mux_);
    Serial.println("Session logger error: storage worker creation failed");
    return false;
  }

  portENTER_CRITICAL(&mux_);
  initialized_ = true;
  portEXIT_CRITICAL(&mux_);

#if SESSION_LOGGER_DEBUG
  Serial.printf(
      "Session logger initialized: FIFO %u, worker stack %u, priority %u, core %d\n",
      static_cast<unsigned>(SESSION_FIFO_CAPACITY),
      static_cast<unsigned>(kWorkerStackSizeBytes),
      static_cast<unsigned>(kWorkerPriority),
      static_cast<int>(workerCore));
#endif
  return true;
}

bool SessionLogger::startSession(uint32_t flushIntervalSeconds) {
  if (!isSupportedInterval(flushIntervalSeconds)) {
    setError(SessionLoggerError::InvalidInterval);
    return false;
  }

  portENTER_CRITICAL(&mux_);
  const bool canRequest = initialized_ && storage_ != nullptr &&
                          state_ == SessionLoggerState::Idle;
  portEXIT_CRITICAL(&mux_);
  if (!canRequest) {
    setError(SessionLoggerError::InvalidState);
    return false;
  }
  if (!storage_->isAvailable()) {
    setError(SessionLoggerError::StorageNotAvailable);
    return false;
  }
  if (!storage_->canStartNewSession()) {
    setError(SessionLoggerError::InvalidState);
    return false;
  }
  if (!fifo_.reset()) {
    setError(SessionLoggerError::FifoBusy);
    return false;
  }

  portENTER_CRITICAL(&mux_);
  if (state_ != SessionLoggerState::Idle) {
    lastError_ = SessionLoggerError::InvalidState;
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  clearSessionCountersLocked();
  selectedIntervalSeconds_ = flushIntervalSeconds;
  state_ = SessionLoggerState::PreparingStorage;
  stopDrainComplete_ = false;
  stoppingStateApplied_ = false;
  flushCycleActive_ = false;
  earlyFlushNotified_ = false;
  storageCapacityReached_ = false;
  lastError_ = SessionLoggerError::None;
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::resetSessionAdmissionWindow();
  AcquisitionDiagnostics::setPhase(
      AcquisitionRuntimePhase::PreparingStorage);
  notifyWorker();
  return true;
}

bool SessionLogger::submitValidatedPacket(
    const uint8_t* packetBytes,
    size_t packetLength,
    uint32_t stm32Sequence,
    uint64_t captureTimestampUs) {
  uint64_t logicalRecordIndex = 0U;

  portENTER_CRITICAL(&mux_);
  if (state_ != SessionLoggerState::Active) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  if (packetBytes == nullptr) {
    lastError_ = SessionLoggerError::NullPacket;
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  if (packetLength != SESSION_PACKET_BYTES) {
    ++invalidLengthRejectionCount_;
    lastError_ = SessionLoggerError::InvalidPacketLength;
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  ++validPacketsOffered_;
  if (producerInFlight_ != 0U) {
    ++droppedPacketCount_;
    lastError_ = SessionLoggerError::FifoBusy;
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  if (nextGlobalLogicalRecordIndex_ == UINT64_MAX) {
    ++droppedPacketCount_;
    state_ = SessionLoggerState::ErrorIncomplete;
    lastError_ = SessionLoggerError::CounterOverflow;
    portEXIT_CRITICAL(&mux_);
    notifyWorker();
    return false;
  }

  producerInFlight_ = 1U;
  logicalRecordIndex = nextGlobalLogicalRecordIndex_;
  portEXIT_CRITICAL(&mux_);

  const RamPacketFifo::PushResult result = fifo_.push(
      packetBytes,
      packetLength,
      captureTimestampUs,
      logicalRecordIndex,
      stm32Sequence);

  bool accepted = false;
  bool notifyStoppingWorker = false;
  bool capacityStopRequested = false;
  portENTER_CRITICAL(&mux_);
  if (result == RamPacketFifo::PushResult::ACCEPTED) {
    ++acceptedPacketCount_;
    ++nextGlobalLogicalRecordIndex_;
    accepted = true;

    if (!hasRecords_) {
      hasRecords_ = true;
      firstStm32Sequence_ = stm32Sequence;
      firstLogicalRecordIndex_ = logicalRecordIndex;
    }
    lastStm32Sequence_ = stm32Sequence;
    lastLogicalRecordIndex_ = logicalRecordIndex;
    if (AcquisitionPolicy::preparedCapacityReached(
            acceptedPacketCount_, SESSION_STORAGE_MAX_RECORDS)) {
      storageCapacityReached_ = true;
      state_ = SessionLoggerState::Stopping;
      stopDrainComplete_ = false;
      stoppingStateApplied_ = false;
      flushCycleActive_ = true;
      earlyFlushNotified_ = true;
      lastError_ = SessionLoggerError::StorageCapacityReached;
      capacityStopRequested = true;
    }
  } else if (result == RamPacketFifo::PushResult::FULL) {
    ++droppedPacketCount_;
    lastError_ = SessionLoggerError::FifoFull;
  } else if (result == RamPacketFifo::PushResult::BUSY) {
    ++droppedPacketCount_;
    lastError_ = SessionLoggerError::FifoBusy;
  } else {
    ++droppedPacketCount_;
    lastError_ = SessionLoggerError::NullPacket;
  }
  producerInFlight_ = 0U;
  notifyStoppingWorker = state_ == SessionLoggerState::Stopping;
  portEXIT_CRITICAL(&mux_);

  if (capacityStopRequested) {
    AcquisitionDiagnostics::endSessionAdmissionWindow();
    AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Stopping);
  }
  if (notifyStoppingWorker) {
    notifyWorker();
  }
  if (!accepted) {
    return false;
  }

  const RamPacketFifo::Status fifoStatus = fifo_.getStatus();
  if (fifoStatus.occupancy >= kEarlyFlushOccupancy) {
    bool shouldNotify = false;
    portENTER_CRITICAL(&mux_);
    if (state_ == SessionLoggerState::Active && !earlyFlushNotified_) {
      earlyFlushNotified_ = true;
      shouldNotify = true;
    }
    portEXIT_CRITICAL(&mux_);
    if (shouldNotify) {
      notifyWorker();
    }
  }
  return true;
}

bool SessionLogger::stopSession() {
  portENTER_CRITICAL(&mux_);
  if (state_ != SessionLoggerState::Active) {
    lastError_ = SessionLoggerError::InvalidState;
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  state_ = SessionLoggerState::Stopping;
  stopDrainComplete_ = false;
  stoppingStateApplied_ = false;
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::endSessionAdmissionWindow();
  AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Stopping);
  notifyWorker();
  return true;
}

bool SessionLogger::clearRetainedSession() {
  portENTER_CRITICAL(&mux_);
  const bool canClear = initialized_ && storage_ != nullptr &&
      state_ != SessionLoggerState::Starting &&
      state_ != SessionLoggerState::PreparingStorage &&
      state_ != SessionLoggerState::Active &&
      state_ != SessionLoggerState::Stopping &&
      state_ != SessionLoggerState::Finalizing &&
      state_ != SessionLoggerState::Clearing &&
      state_ != SessionLoggerState::Rescanning;
  if (!canClear) {
    lastError_ = SessionLoggerError::InvalidState;
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  state_ = SessionLoggerState::Clearing;
  stopDrainComplete_ = false;
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Clearing);
  notifyWorker();
  return true;
}

bool SessionLogger::rescanRetainedStorage() {
  portENTER_CRITICAL(&mux_);
  const bool canRescan = initialized_ && storage_ != nullptr &&
      state_ != SessionLoggerState::Starting &&
      state_ != SessionLoggerState::PreparingStorage &&
      state_ != SessionLoggerState::Active &&
      state_ != SessionLoggerState::Stopping &&
      state_ != SessionLoggerState::Finalizing &&
      state_ != SessionLoggerState::Clearing &&
      state_ != SessionLoggerState::Rescanning;
  if (!canRescan) {
    lastError_ = SessionLoggerError::InvalidState;
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  state_ = SessionLoggerState::Rescanning;
  portEXIT_CRITICAL(&mux_);
  notifyWorker();
  return true;
}

bool SessionLogger::isActive() const {
  portENTER_CRITICAL(&mux_);
  const bool active = state_ == SessionLoggerState::Active;
  portEXIT_CRITICAL(&mux_);
  return active;
}

SessionLoggerStatus SessionLogger::getStatus() const {
  SessionLoggerStatus status{};

  portENTER_CRITICAL(&mux_);
  status.initialized = initialized_;
  status.psramAvailable = psramAvailable_;
  status.hasRecords = hasRecords_;
  status.state = state_;
  status.startPending = state_ == SessionLoggerState::Starting ||
                        state_ == SessionLoggerState::PreparingStorage;
  status.storageCapacityReached = storageCapacityReached_;
  status.sessionId = sessionId_;
  status.selectedIntervalSeconds = selectedIntervalSeconds_;
  status.validPacketsOffered = validPacketsOffered_;
  status.acceptedPacketCount = acceptedPacketCount_;
  status.droppedPacketCount = droppedPacketCount_;
  status.invalidLengthRejectionCount = invalidLengthRejectionCount_;
  status.firstStm32Sequence = firstStm32Sequence_;
  status.lastStm32Sequence = lastStm32Sequence_;
  status.firstLogicalRecordIndex = firstLogicalRecordIndex_;
  status.lastLogicalRecordIndex = lastLogicalRecordIndex_;
  status.nextLogicalRecordIndex = nextGlobalLogicalRecordIndex_;
  status.producerInFlight = producerInFlight_;
  status.stopDrainComplete = stopDrainComplete_;
  status.lastError = lastError_;
  portEXIT_CRITICAL(&mux_);

  const RamPacketFifo::Status fifoStatus = fifo_.getStatus();
  status.fifoCapacity = fifoStatus.capacity;
  status.fifoOccupancy = fifoStatus.occupancy;
  status.fifoHighWaterMark = fifoStatus.highWaterMark;

  if (storage_ != nullptr) {
    const SessionStorageStatus storageStatus = storage_->getStatus();
    status.storageAvailable = storageStatus.available;
    status.preparationInProgress = storageStatus.preparationInProgress;
    status.dataAreaPrepared = storageStatus.dataAreaPrepared;
    status.storageCapacityReached = status.storageCapacityReached ||
                                    storageStatus.storageCapacityReached;
    status.metadataAValid = storageStatus.metadataAValid;
    status.metadataBValid = storageStatus.metadataBValid;
    status.selectedMetadataCopy = storageStatus.selectedMetadataCopy;
    status.selectedMetadataGeneration =
        storageStatus.selectedMetadataGeneration;
    status.recoveryPerformed = storageStatus.recoveryPerformed;
    status.recoveredInterrupted = storageStatus.recoveredInterrupted;
    status.countersPartial = storageStatus.countersPartial;
    status.corruptionOrGap = storageStatus.corruptionOrGap;
    status.storageTruncated = storageStatus.storageTruncated;
    status.finalized = storageStatus.finalized;
    status.storedRecordCount = storageStatus.storedRecordCount;
    status.totalStoredRecords = storageStatus.totalStoredRecords;
    status.retainedRecordCount = storageStatus.retainedRecordCount;
    status.overwrittenRecordCount = storageStatus.overwrittenRecordCount;
    if (storageStatus.droppedRecordCount > status.droppedPacketCount) {
      status.droppedPacketCount = storageStatus.droppedRecordCount;
    }
    status.unstoredPacketCount =
        status.acceptedPacketCount >= storageStatus.storedRecordCount
            ? status.acceptedPacketCount - storageStatus.storedRecordCount
            : 0U;
    status.flashEraseFailureCount =
        storageStatus.flashEraseFailureCount;
    status.flashWriteFailureCount =
        storageStatus.flashWriteFailureCount;
    status.flashReadFailureCount = storageStatus.flashReadFailureCount;
    status.codecValidationFailureCount =
        storageStatus.codecValidationFailureCount +
        storageStatus.metadataValidationFailureCount +
        storageStatus.segmentValidationFailureCount +
        storageStatus.recordValidationFailureCount;
    status.currentDataSegment = storageStatus.currentDataSegment;
    status.currentSlotInSegment = storageStatus.currentSlotInSegment;
    status.oldestPhysicalSegment = storageStatus.oldestPhysicalSegment;
    status.oldestPhysicalSlot = storageStatus.oldestPhysicalSlot;
    status.nextSegmentSequence = storageStatus.nextSegmentSequence;
    status.nextPartitionRelativeWriteOffset =
        storageStatus.nextPartitionRelativeWriteOffset;
    status.bytesWritten = storageStatus.bytesWritten;
    status.maximumRecords = storageStatus.maximumRecords;
    status.preparationSectorsCompleted =
        storageStatus.preparationSectorsCompleted;
    status.preparationSectorsTotal =
        storageStatus.preparationSectorsTotal;
    status.preparationSegmentsCompleted =
        storageStatus.preparationSegmentsCompleted;
    status.preparationSegmentsTotal =
        storageStatus.preparationSegmentsTotal;
    status.firstRetainedLogicalIndex =
        storageStatus.firstRetainedLogicalIndex;
    status.lastRetainedLogicalIndex =
        storageStatus.lastRetainedLogicalIndex;
    if (storageStatus.retainedRecordCount > 0U) {
      status.hasRecords = true;
      status.firstStm32Sequence = storageStatus.firstStm32Sequence;
      status.lastStm32Sequence = storageStatus.lastStm32Sequence;
      status.firstLogicalRecordIndex =
          storageStatus.firstRetainedLogicalIndex;
      status.lastLogicalRecordIndex =
          storageStatus.lastRetainedLogicalIndex;
      status.nextLogicalRecordIndex = storageStatus.nextLogicalRecordIndex;
    }
  }
  return status;
}

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
SessionFlushDiagnostics SessionLogger::getFlushDiagnostics() const {
  portENTER_CRITICAL(&mux_);
  SessionFlushDiagnostics diagnostics = flushDiagnostics_;
  diagnostics.validPacketsOffered = validPacketsOffered_;
  diagnostics.acceptedRecords = acceptedPacketCount_;
  diagnostics.loggerDroppedRecordCount = droppedPacketCount_;
  portEXIT_CRITICAL(&mux_);

  const AcquisitionDiagnosticsSnapshot acquisition =
      AcquisitionDiagnostics::getSnapshot();
  diagnostics.uartFifoOverflowCount = acquisition.uartFifoOverflows;
  diagnostics.uartRingBufferOverflowCount =
      acquisition.uartRingBufferOverflows;
  diagnostics.packetChecksumFailureCount =
      acquisition.packetChecksumFailures;
  diagnostics.packetResynchronizationCount =
      acquisition.packetResynchronizations;
  diagnostics.packetSequenceGapCount = acquisition.uartSequenceGaps;
  diagnostics.checksumValidPacketCount =
      acquisition.checksumValidUartPackets;
  diagnostics.checksumValidAtFlushStart =
      acquisition.flushStartChecksumValidPackets;
  diagnostics.checksumValidAtFlushEnd =
      acquisition.flushEndChecksumValidPackets;
  diagnostics.checksumValidDuringFlush =
      acquisition.flushEndChecksumValidPackets >=
              acquisition.flushStartChecksumValidPackets
          ? acquisition.flushEndChecksumValidPackets -
                acquisition.flushStartChecksumValidPackets
          : 0U;
  diagnostics.loggerSubmissionAttempts =
      acquisition.loggerSubmissionAttempts;
  diagnostics.loggerAcceptedSubmissions =
      acquisition.loggerAcceptedPackets;
  diagnostics.loggerRejectedSubmissions =
      acquisition.loggerRejectedPackets;
  diagnostics.lastSequenceBeforeFlush =
      acquisition.lastSequenceBeforeFlush;
  diagnostics.hasLastSequenceBeforeFlush =
      acquisition.hasLastSequenceBeforeFlush;
  diagnostics.firstSequenceAfterFlush =
      acquisition.firstSequenceAfterFlush;
  diagnostics.hasFirstSequenceAfterFlush =
      acquisition.hasFirstSequenceAfterFlush;
  diagnostics.sequenceGapsDuringFlush =
      acquisition.flushEndSequenceGaps >=
              acquisition.flushStartSequenceGaps
          ? acquisition.flushEndSequenceGaps -
                acquisition.flushStartSequenceGaps
          : 0U;

  const RamPacketFifo::Status fifoStatus = fifo_.getStatus();
  diagnostics.fifoHighWaterMark = fifoStatus.highWaterMark;
  diagnostics.workerInFlightRecords = fifoStatus.consumerInFlight;
  diagnostics.fifoPendingRecords =
      fifoStatus.occupancy >= fifoStatus.consumerInFlight
          ? fifoStatus.occupancy - fifoStatus.consumerInFlight
          : 0U;
  if (storage_ != nullptr) {
    diagnostics.successfullyWrittenRecords =
        storage_->getStatus().storedRecordCount;
  }
  diagnostics.acceptedInvariantSatisfied =
      AcquisitionPolicy::acceptedRecordsAccounted(
          diagnostics.acceptedRecords,
          diagnostics.successfullyWrittenRecords,
          diagnostics.fifoPendingRecords,
          diagnostics.workerInFlightRecords);
  diagnostics.offeredInvariantSatisfied =
      AcquisitionPolicy::offeredRecordsAccounted(
          diagnostics.validPacketsOffered,
          diagnostics.successfullyWrittenRecords,
          diagnostics.loggerDroppedRecordCount,
          diagnostics.fifoPendingRecords,
          diagnostics.workerInFlightRecords);
  return diagnostics;
}
#endif

bool SessionLogger::getRecordMetadata(
    uint32_t ordinal,
    RamPacketRecordMetadata& metadata) const {
  return fifo_.getRecordMetadata(ordinal, metadata);
}

bool SessionLogger::copyPacketBytes(
    uint32_t ordinal,
    uint8_t* destination,
    size_t destinationLength) const {
  return fifo_.copyPacketBytes(ordinal, destination, destinationLength);
}

bool SessionLogger::packetBytesEqual(
    uint32_t ordinal,
    const uint8_t* expected,
    size_t expectedLength) const {
  return fifo_.packetBytesEqual(ordinal, expected, expectedLength);
}

void SessionLogger::workerTaskEntry(void* context) {
  static_cast<SessionLogger*>(context)->workerTask();
}

void SessionLogger::workerTask() {
  for (;;) {
    SessionLoggerState state;
    uint32_t deadlineMs;

    portENTER_CRITICAL(&mux_);
    state = state_;
    deadlineMs = nextFlushDeadlineMs_;
    portEXIT_CRITICAL(&mux_);

    TickType_t waitTicks = portMAX_DELAY;
    if (state == SessionLoggerState::Active) {
      const int32_t remainingMs =
          static_cast<int32_t>(deadlineMs - millis());
      waitTicks = remainingMs <= 0
                      ? 0
                      : pdMS_TO_TICKS(static_cast<uint32_t>(remainingMs));
      if (remainingMs > 0 && waitTicks == 0) {
        waitTicks = 1;
      }
    } else if (state == SessionLoggerState::Starting ||
               state == SessionLoggerState::PreparingStorage ||
               state == SessionLoggerState::Clearing ||
               state == SessionLoggerState::Rescanning ||
               state == SessionLoggerState::Finalizing) {
      waitTicks = 0;
    } else if (state == SessionLoggerState::Stopping) {
      uint32_t producerInFlight;
      bool stoppingStateApplied;
      portENTER_CRITICAL(&mux_);
      producerInFlight = producerInFlight_;
      stoppingStateApplied = stoppingStateApplied_;
      portEXIT_CRITICAL(&mux_);
      const uint32_t occupancy = fifo_.getStatus().occupancy;
      waitTicks = stoppingStateApplied && occupancy == 0U &&
                          producerInFlight != 0U
                      ? portMAX_DELAY
                      : 0;
    }

    uint32_t notificationValue = 0U;
    xTaskNotifyWait(0U, UINT32_MAX, &notificationValue, waitTicks);

    portENTER_CRITICAL(&mux_);
    state = state_;
    deadlineMs = nextFlushDeadlineMs_;
    portEXIT_CRITICAL(&mux_);

    if (state == SessionLoggerState::Starting ||
        state == SessionLoggerState::PreparingStorage) {
      handleStartRequest();
      continue;
    }
    if (state == SessionLoggerState::Clearing) {
      handleClearRequest();
      continue;
    }
    if (state == SessionLoggerState::Rescanning) {
      handleRescanRequest();
      continue;
    }
    if (state == SessionLoggerState::Finalizing) {
      finishStopIfDrained();
      continue;
    }

    if (state == SessionLoggerState::Active) {
      const RamPacketFifo::Status fifoStatus = fifo_.getStatus();
      const bool deadlineReached =
          static_cast<int32_t>(millis() - deadlineMs) >= 0;
      bool flushCycleActive;
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
      bool startedFlushCycle = false;
#endif
      portENTER_CRITICAL(&mux_);
      flushCycleActive = flushCycleActive_;
      if (!flushCycleActive &&
          (deadlineReached ||
           fifoStatus.occupancy >= kEarlyFlushOccupancy)) {
        flushCycleActive_ = true;
        flushCycleActive = true;
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
        startedFlushCycle = true;
#endif
      }
      portEXIT_CRITICAL(&mux_);

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
      if (startedFlushCycle) {
        beginFlushDiagnostics(fifoStatus.occupancy);
      }
#endif

      if (!flushCycleActive) {
        continue;
      }
      if (fifoStatus.occupancy > 0U &&
          !drainRecords(boundedBatch(fifoStatus.occupancy))) {
        continue;
      }

      if (fifo_.getStatus().occupancy > 0U) {
        notifyWorker();
      } else {
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
        completeFlushDiagnostics(true);
#endif
        portENTER_CRITICAL(&mux_);
        if (state_ == SessionLoggerState::Active) {
          nextFlushDeadlineMs_ =
              millis() + selectedIntervalSeconds_ * 1000U;
          earlyFlushNotified_ = false;
          flushCycleActive_ = false;
        }
        portEXIT_CRITICAL(&mux_);
      }
      continue;
    }

    if (state == SessionLoggerState::Stopping) {
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
      beginFlushDiagnostics(fifo_.getStatus().occupancy);
#endif
      bool stoppingStateApplied;
      uint64_t dropped;
      portENTER_CRITICAL(&mux_);
      stoppingStateApplied = stoppingStateApplied_;
      dropped = droppedPacketCount_;
      portEXIT_CRITICAL(&mux_);

      if (!stoppingStateApplied) {
        const SessionStorageError stopResult =
            storage_->markStopping(dropped);
        if (stopResult != SessionStorageError::None) {
          transitionToStorageError(stopResult);
          continue;
        }
        portENTER_CRITICAL(&mux_);
        if (state_ == SessionLoggerState::Stopping) {
          stoppingStateApplied_ = true;
        }
        portEXIT_CRITICAL(&mux_);
      }

      const RamPacketFifo::Status fifoStatus = fifo_.getStatus();
      if (fifoStatus.occupancy > 0U &&
          !drainRecords(boundedBatch(fifoStatus.occupancy))) {
        continue;
      }
      if (fifo_.getStatus().occupancy > 0U) {
        notifyWorker();
        continue;
      }
      finishStopIfDrained();
    }
  }
}

void SessionLogger::handleStartRequest() {
  uint32_t interval;
  uint32_t bootId;
  portENTER_CRITICAL(&mux_);
  interval = selectedIntervalSeconds_;
  bootId = bootId_;
  portEXIT_CRITICAL(&mux_);

  const SessionStorageError preparationResult =
      storage_->prepareEmptyDataArea();
  if (preparationResult != SessionStorageError::None) {
    transitionToStorageError(preparationResult);
    return;
  }

  const WallClockSnapshot startAnchor = wallClock_->capture(bootId);
  SessionStorageStartInfo startInfo{};
  const SessionStorageError result = storage_->startNewSession(
      interval,
      bootId,
      startAnchor.uptimeUs,
      startInfo,
      startAnchor.unixEpochMs,
      startAnchor.valid);
  if (result != SessionStorageError::None) {
    transitionToStorageError(result);
    return;
  }

  portENTER_CRITICAL(&mux_);
  if (state_ == SessionLoggerState::Starting ||
      state_ == SessionLoggerState::PreparingStorage) {
    sessionId_ = startInfo.sessionId;
    nextGlobalLogicalRecordIndex_ = startInfo.firstLogicalRecordIndex;
    nextFlushDeadlineMs_ = millis() + selectedIntervalSeconds_ * 1000U;
    state_ = SessionLoggerState::Active;
    lastError_ = SessionLoggerError::None;
  }
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::beginSessionAdmissionWindow();
  AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Active);
}

void SessionLogger::handleClearRequest() {
  const SessionStorageError result = storage_->clearRetainedSession();
  if (result != SessionStorageError::None) {
    transitionToStorageError(result);
    return;
  }
  if (!fifo_.reset()) {
    portENTER_CRITICAL(&mux_);
    state_ = SessionLoggerState::ErrorIncomplete;
    lastError_ = SessionLoggerError::FifoBusy;
    portEXIT_CRITICAL(&mux_);
    Serial.println("Session logger error: FIFO could not reset after retained-session clear");
    return;
  }

  portENTER_CRITICAL(&mux_);
  clearSessionCountersLocked();
  sessionId_ = 0U;
  selectedIntervalSeconds_ = 0U;
  stopDrainComplete_ = false;
  storageCapacityReached_ = false;
  portEXIT_CRITICAL(&mux_);
  synchronizeRecoveredState(storage_->getStatus());
}

void SessionLogger::handleRescanRequest() {
  const SessionStorageError result = storage_->rescanRetainedStorage();
  if (result != SessionStorageError::None) {
    transitionToStorageError(result);
    return;
  }
  synchronizeRecoveredState(storage_->getStatus());
}

bool SessionLogger::drainRecords(uint32_t recordLimit) {
  for (uint32_t processed = 0U; processed < recordLimit; ++processed) {
    RamPacketRecordMetadata metadata{};
    RamPacketFifo::ConsumerToken token{};
    const RamPacketFifo::ConsumeResult copyResult = fifo_.copyOldest(
        packetCopyBuffer_, SESSION_PACKET_BYTES, metadata, token);

    if (copyResult == RamPacketFifo::ConsumeResult::Empty) {
      return true;
    }
    if (copyResult != RamPacketFifo::ConsumeResult::Copied) {
      portENTER_CRITICAL(&mux_);
      state_ = SessionLoggerState::ErrorIncomplete;
      stopDrainComplete_ = false;
      lastError_ = SessionLoggerError::FifoConsumerError;
      portEXIT_CRITICAL(&mux_);
      Serial.println("Session logger error: FIFO consumer reservation failed");
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
      completeFlushDiagnostics(false);
#endif
      return false;
    }

    uint64_t sessionId;
    uint32_t bootId;
    portENTER_CRITICAL(&mux_);
    sessionId = sessionId_;
    bootId = bootId_;
    portEXIT_CRITICAL(&mux_);

    const SessionStorageError storageResult = storage_->appendRecord(
        packetCopyBuffer_,
        SESSION_PACKET_BYTES,
        sessionId,
        metadata.logicalRecordIndex,
        metadata.captureTimestampUs,
        metadata.stm32Sequence,
        kStm32PacketFormatVersion,
        kStage3RecordFlags,
        bootId,
        encodedRecordBuffer_,
        FLASH_RECORD_BYTES,
        readbackBuffer_,
        FLASH_RECORD_BYTES);

    if (storageResult != SessionStorageError::None) {
      bool leaseRecovered = fifo_.cancelOldest(token);
      if (!leaseRecovered) {
        leaseRecovered = fifo_.recoverOldestLease();
      }
      if (!leaseRecovered) {
        portENTER_CRITICAL(&mux_);
        state_ = SessionLoggerState::ErrorIncomplete;
        stopDrainComplete_ = false;
        lastError_ = SessionLoggerError::FifoConsumerError;
        portEXIT_CRITICAL(&mux_);
        Serial.println(
            "Session logger error: FIFO lease recovery failed after storage error");
        return false;
      }
      transitionToStorageError(storageResult);
      return false;
    }

    if (!fifo_.commitOldest(token)) {
      // The record is already durable. Republish only so a later explicit
      // clear can reset the FIFO; this error state never retries the record.
      fifo_.recoverOldestLease();
      portENTER_CRITICAL(&mux_);
      state_ = SessionLoggerState::ErrorIncomplete;
      stopDrainComplete_ = false;
      lastError_ = SessionLoggerError::FifoConsumerError;
      portEXIT_CRITICAL(&mux_);
      Serial.println(
          "Session logger error: durable record could not be removed from FIFO");
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
      completeFlushDiagnostics(false);
#endif
      return false;
    }

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
    noteFlushRecordWritten();
#endif

    if ((processed + 1U) % kRecordsPerWorkerBatch == 0U) {
      vTaskDelay(ACQUISITION_FLASH_SERVICE_YIELD_TICKS);
    }
  }
  return true;
}

void SessionLogger::finishStopIfDrained() {
  if (fifo_.getStatus().occupancy != 0U) {
    return;
  }

  uint32_t producerInFlight;
  uint64_t dropped;
  portENTER_CRITICAL(&mux_);
  producerInFlight = producerInFlight_;
  dropped = droppedPacketCount_;
  portEXIT_CRITICAL(&mux_);
  if (producerInFlight != 0U) {
    return;
  }

  uint32_t bootId;
  portENTER_CRITICAL(&mux_);
  if (state_ != SessionLoggerState::Stopping &&
      state_ != SessionLoggerState::Finalizing) {
    portEXIT_CRITICAL(&mux_);
    return;
  }
  state_ = SessionLoggerState::Finalizing;
  bootId = bootId_;
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Finalizing);
  const WallClockSnapshot endAnchor = wallClock_->capture(bootId);
  const SessionStorageError result = storage_->finalizeSession(
      dropped,
      endAnchor.uptimeUs,
      endAnchor.unixEpochMs,
      endAnchor.valid);
  if (result != SessionStorageError::None) {
    transitionToStorageError(result);
    return;
  }

  const SessionStorageStatus storageStatus = storage_->getStatus();
  portENTER_CRITICAL(&mux_);
  if (state_ == SessionLoggerState::Finalizing) {
    state_ = SessionLoggerState::Finalized;
    stopDrainComplete_ = true;
    earlyFlushNotified_ = false;
    flushCycleActive_ = false;
    lastError_ = SessionLoggerError::None;
    sessionId_ = storageStatus.sessionId;
  }
  portEXIT_CRITICAL(&mux_);
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  completeFlushDiagnostics(true);
#endif
}

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
void SessionLogger::beginFlushDiagnostics(uint32_t recordsRequested) {
  portENTER_CRITICAL(&mux_);
  const bool alreadyActive = flushDiagnostics_.flushActive;
  portEXIT_CRITICAL(&mux_);
  if (alreadyActive || storage_ == nullptr) {
    return;
  }

  const SessionStorageStatus storageStatus = storage_->getStatus();
  storage_->resetIoDiagnostics();
  AcquisitionDiagnostics::beginFlushWindow();
  const AcquisitionDiagnosticsSnapshot acquisition =
      AcquisitionDiagnostics::getSnapshot();
  const uint64_t startedUs = static_cast<uint64_t>(esp_timer_get_time());
  const uint32_t uartBytesBefore = static_cast<uint32_t>(Serial1.available());

  portENTER_CRITICAL(&mux_);
  flushDiagnostics_ = SessionFlushDiagnostics{};
  flushDiagnostics_.available = true;
  flushDiagnostics_.flushActive = true;
  flushDiagnostics_.flushStartTimestampUs = startedUs;
  flushDiagnostics_.recordsRequested = recordsRequested;
  flushDiagnostics_.uartBytesAvailableBefore = uartBytesBefore;
  flushDiagnostics_.lastSequenceBeforeFlush =
      acquisition.lastSequenceBeforeFlush;
  flushDiagnostics_.hasLastSequenceBeforeFlush =
      acquisition.hasLastSequenceBeforeFlush;
  flushDiagnostics_.checksumValidAtFlushStart =
      acquisition.flushStartChecksumValidPackets;
  erasedSegmentsAtFlushStart_ = storageStatus.erasedSegmentCount;
  portEXIT_CRITICAL(&mux_);
}

void SessionLogger::completeFlushDiagnostics(bool successful) {
  portENTER_CRITICAL(&mux_);
  const bool active = flushDiagnostics_.flushActive;
  const uint64_t startedUs = flushDiagnostics_.flushStartTimestampUs;
  portEXIT_CRITICAL(&mux_);
  if (!active || storage_ == nullptr) {
    return;
  }

  const uint64_t endedUs = static_cast<uint64_t>(esp_timer_get_time());
  const uint32_t uartBytesAfter = static_cast<uint32_t>(Serial1.available());
  AcquisitionDiagnostics::endFlushWindow();
  const AcquisitionDiagnosticsSnapshot acquisition =
      AcquisitionDiagnostics::getSnapshot();
  const SessionStorageStatus storageStatus = storage_->getStatus();
  const SessionStorageIoDiagnostics io = storage_->getIoDiagnostics();

  portENTER_CRITICAL(&mux_);
  if (flushDiagnostics_.flushActive &&
      flushDiagnostics_.flushStartTimestampUs == startedUs) {
    flushDiagnostics_.flushActive = false;
    flushDiagnostics_.lastFlushSuccessful = successful;
    flushDiagnostics_.flushEndTimestampUs = endedUs;
    flushDiagnostics_.flushDurationUs = endedUs - startedUs;
    flushDiagnostics_.maximumWriteDurationUs =
        io.maximumWriteDurationUs;
    flushDiagnostics_.maximumEraseDurationUs =
        io.maximumEraseDurationUs;
    flushDiagnostics_.segmentsErased =
        storageStatus.erasedSegmentCount >= erasedSegmentsAtFlushStart_
            ? storageStatus.erasedSegmentCount - erasedSegmentsAtFlushStart_
            : 0U;
    flushDiagnostics_.uartBytesAvailableAfter = uartBytesAfter;
    flushDiagnostics_.checksumValidAtFlushEnd =
        acquisition.flushEndChecksumValidPackets;
    flushDiagnostics_.checksumValidDuringFlush =
        acquisition.flushEndChecksumValidPackets >=
                acquisition.flushStartChecksumValidPackets
            ? acquisition.flushEndChecksumValidPackets -
                  acquisition.flushStartChecksumValidPackets
            : 0U;
    flushDiagnostics_.sequenceGapsDuringFlush =
        acquisition.flushEndSequenceGaps >=
                acquisition.flushStartSequenceGaps
            ? acquisition.flushEndSequenceGaps -
                  acquisition.flushStartSequenceGaps
            : 0U;
  }
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Retained);
}

void SessionLogger::noteFlushRecordWritten() {
  portENTER_CRITICAL(&mux_);
  if (flushDiagnostics_.flushActive) {
    ++flushDiagnostics_.recordsSuccessfullyWritten;
  }
  portEXIT_CRITICAL(&mux_);
}
#endif

void SessionLogger::transitionToStorageError(SessionStorageError error) {
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  completeFlushDiagnostics(false);
#endif
  portENTER_CRITICAL(&mux_);
  state_ = SessionLoggerState::ErrorIncomplete;
  stopDrainComplete_ = false;
  earlyFlushNotified_ = false;
  flushCycleActive_ = false;
  lastError_ = mapStorageError(error);
  portEXIT_CRITICAL(&mux_);
  AcquisitionDiagnostics::endSessionAdmissionWindow();
  AcquisitionDiagnostics::setPhase(AcquisitionRuntimePhase::Error);
  Serial.print("Session logger storage error: ");
  Serial.println(static_cast<unsigned>(error));
}

void SessionLogger::synchronizeRecoveredState(
    const SessionStorageStatus& storageStatus) {
  SessionLoggerState mappedState = SessionLoggerState::ErrorIncomplete;
  SessionLoggerError mappedError = SessionLoggerError::None;

  if (storageStatus.recoveryBlocked) {
    mappedState = SessionLoggerState::ErrorIncomplete;
    mappedError = SessionLoggerError::RecoveryFailed;
  } else {
    switch (storageStatus.persistentSessionState) {
      case PersistentSessionState::Empty:
        mappedState = SessionLoggerState::Idle;
        break;
      case PersistentSessionState::Finalized:
        mappedState = SessionLoggerState::Finalized;
        break;
      case PersistentSessionState::RecoveredIncomplete:
        mappedState = SessionLoggerState::RecoveredIncomplete;
        break;
      case PersistentSessionState::ErrorIncomplete:
        mappedState = SessionLoggerState::ErrorIncomplete;
        mappedError = SessionLoggerError::RecoveryFailed;
        break;
      case PersistentSessionState::Active:
      case PersistentSessionState::Stopping:
      default:
        mappedState = SessionLoggerState::ErrorIncomplete;
        mappedError = SessionLoggerError::RecoveryFailed;
        break;
    }
  }

  portENTER_CRITICAL(&mux_);
  state_ = mappedState;
  lastError_ = mappedError;
  sessionId_ = storageStatus.sessionId;
  selectedIntervalSeconds_ = storageStatus.selectedIntervalSeconds;
  nextGlobalLogicalRecordIndex_ = storageStatus.nextLogicalRecordIndex;
  stopDrainComplete_ = storageStatus.stopDrainComplete;
  hasRecords_ = storageStatus.retainedRecordCount > 0U;
  firstStm32Sequence_ = storageStatus.firstStm32Sequence;
  lastStm32Sequence_ = storageStatus.lastStm32Sequence;
  firstLogicalRecordIndex_ = storageStatus.firstRetainedLogicalIndex;
  lastLogicalRecordIndex_ = storageStatus.lastRetainedLogicalIndex;
  storageCapacityReached_ = storageStatus.storageCapacityReached;
  portEXIT_CRITICAL(&mux_);

  AcquisitionRuntimePhase phase = AcquisitionRuntimePhase::Error;
  if (mappedState == SessionLoggerState::Idle) {
    phase = AcquisitionRuntimePhase::Idle;
  } else if (mappedState == SessionLoggerState::Finalized ||
             mappedState == SessionLoggerState::RecoveredIncomplete) {
    phase = AcquisitionRuntimePhase::Retained;
  }
  AcquisitionDiagnostics::setPhase(phase);
}

void SessionLogger::notifyWorker() {
  if (workerTaskHandle_ != nullptr) {
    xTaskNotify(workerTaskHandle_, kWorkerNotification, eSetBits);
  }
}

void SessionLogger::releaseWorkerBuffers() {
  if (packetCopyBuffer_ != nullptr) {
    heap_caps_free(packetCopyBuffer_);
    packetCopyBuffer_ = nullptr;
  }
  if (encodedRecordBuffer_ != nullptr) {
    heap_caps_free(encodedRecordBuffer_);
    encodedRecordBuffer_ = nullptr;
  }
  if (readbackBuffer_ != nullptr) {
    heap_caps_free(readbackBuffer_);
    readbackBuffer_ = nullptr;
  }
}

bool SessionLogger::isSupportedInterval(uint32_t seconds) {
  return seconds == 1U || seconds == 5U ||
         seconds == 10U || seconds == 60U;
}

SessionLoggerError SessionLogger::mapStorageError(
    SessionStorageError error) {
  switch (error) {
    case SessionStorageError::FlashEraseFailed:
      return SessionLoggerError::FlashEraseFailed;
    case SessionStorageError::FlashWriteFailed:
    case SessionStorageError::CommitWriteFailed:
      return SessionLoggerError::FlashWriteFailed;
    case SessionStorageError::FlashReadFailed:
      return SessionLoggerError::FlashReadFailed;
    case SessionStorageError::CodecEncodeFailed:
      return SessionLoggerError::CodecEncodeFailed;
    case SessionStorageError::ReadbackValidationFailed:
    case SessionStorageError::SegmentHeaderValidationFailed:
      return SessionLoggerError::CodecValidationFailed;
    case SessionStorageError::RecordMetadataMismatch:
    case SessionStorageError::LogicalIndexMismatch:
    case SessionStorageError::SessionIdMismatch:
      return SessionLoggerError::StorageMetadataMismatch;
    case SessionStorageError::MetadataEncodeFailed:
    case SessionStorageError::MetadataValidationFailed:
    case SessionStorageError::MetadataGenerationExhausted:
    case SessionStorageError::MetadataCopiesConflict:
      return SessionLoggerError::MetadataCommitFailed;
    case SessionStorageError::RecoveryBlocked:
    case SessionStorageError::RecordGapOrCorruption:
      return SessionLoggerError::RecoveryFailed;
    case SessionStorageError::StoragePreparationRequired:
      return SessionLoggerError::StoragePreparationFailed;
    case SessionStorageError::StorageCapacityReached:
      return SessionLoggerError::StorageCapacityReached;
    case SessionStorageError::None:
      return SessionLoggerError::None;
    default:
      return SessionLoggerError::StorageStartFailed;
  }
}

void SessionLogger::setError(SessionLoggerError error) {
  portENTER_CRITICAL(&mux_);
  lastError_ = error;
  portEXIT_CRITICAL(&mux_);
}

void SessionLogger::clearSessionCountersLocked() {
  validPacketsOffered_ = 0U;
  acceptedPacketCount_ = 0U;
  droppedPacketCount_ = 0U;
  invalidLengthRejectionCount_ = 0U;
  firstStm32Sequence_ = 0U;
  lastStm32Sequence_ = 0U;
  firstLogicalRecordIndex_ = 0U;
  lastLogicalRecordIndex_ = 0U;
  nextGlobalLogicalRecordIndex_ = 0U;
  producerInFlight_ = 0U;
  hasRecords_ = false;
  storageCapacityReached_ = false;
}

#if SESSION_LOGGER_DEBUG
void SessionLogger::printDebugSummary() const {
  const SessionLoggerStatus status = getStatus();
  Serial.print("Session logger session: ");
  printUint64(status.sessionId);
  Serial.print(", accepted/stored/retained/dropped: ");
  printUint64(status.acceptedPacketCount);
  Serial.print("/");
  printUint64(status.totalStoredRecords);
  Serial.print("/");
  printUint64(status.retainedRecordCount);
  Serial.print("/");
  printUint64(status.droppedPacketCount);
  Serial.printf(", FIFO %u/%u, state %u, error %u\n",
                static_cast<unsigned>(status.fifoOccupancy),
                static_cast<unsigned>(status.fifoCapacity),
                static_cast<unsigned>(status.state),
                static_cast<unsigned>(status.lastError));
}
#endif
