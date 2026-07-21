#pragma once

#include <Arduino.h>

#include "TestConsoleConfig.h"

#ifndef SESSION_LOGGER_DEBUG
#define SESSION_LOGGER_DEBUG 0
#endif

constexpr size_t SESSION_PACKET_BYTES = 4354;
constexpr uint32_t SESSION_FIFO_CAPACITY = 700;
constexpr size_t SESSION_FIFO_SLOT_BYTES = 4376;
constexpr size_t SESSION_FIFO_ALLOCATION_BYTES =
    SESSION_FIFO_CAPACITY * SESSION_FIFO_SLOT_BYTES;

enum class SessionLoggerState : uint8_t {
  Disabled = 0,
  Idle,
  Starting,
  PreparingStorage,
  Active,
  Stopping,
  Finalizing,
  Finalized,
  RecoveredIncomplete,
  Clearing,
  Rescanning,
  ErrorIncomplete,
  Error
};

enum class SessionLoggerError : uint8_t {
  None = 0,
  PsramNotAvailable,
  PsramAllocationFailed,
  InvalidState,
  InvalidInterval,
  NullPacket,
  InvalidPacketLength,
  FifoFull,
  FifoBusy,
  StorageNotAvailable,
  StoragePreparationFailed,
  StorageCapacityReached,
  StorageStartFailed,
  WorkerBufferAllocationFailed,
  WorkerTaskCreationFailed,
  FifoConsumerError,
  FlashEraseFailed,
  FlashWriteFailed,
  FlashReadFailed,
  CodecEncodeFailed,
  CodecValidationFailed,
  StorageMetadataMismatch,
  MetadataCommitFailed,
  RecoveryFailed,
  ClearFailed,
  FinalizationFailed,
  CounterOverflow
};

struct SessionLoggerStatus {
  bool initialized;
  bool psramAvailable;
  bool storageAvailable;
  bool hasRecords;
  bool stopDrainComplete;
  bool startPending;
  bool preparationInProgress;
  bool dataAreaPrepared;
  bool storageCapacityReached;
  bool recoveryPerformed;
  bool recoveredInterrupted;
  bool countersPartial;
  bool corruptionOrGap;
  bool storageTruncated;
  bool finalized;
  bool metadataAValid;
  bool metadataBValid;
  uint8_t selectedMetadataCopy;
  SessionLoggerState state;
  uint64_t sessionId;
  uint32_t selectedIntervalSeconds;
  uint32_t fifoCapacity;
  uint32_t fifoOccupancy;
  uint32_t fifoHighWaterMark;
  uint64_t validPacketsOffered;
  uint64_t acceptedPacketCount;
  uint64_t droppedPacketCount;
  uint64_t invalidLengthRejectionCount;
  uint64_t storedRecordCount;
  uint64_t unstoredPacketCount;
  uint64_t totalStoredRecords;
  uint64_t retainedRecordCount;
  uint64_t overwrittenRecordCount;
  uint64_t flashEraseFailureCount;
  uint64_t flashWriteFailureCount;
  uint64_t flashReadFailureCount;
  uint64_t codecValidationFailureCount;
  uint32_t currentDataSegment;
  uint32_t currentSlotInSegment;
  uint32_t oldestPhysicalSegment;
  uint32_t oldestPhysicalSlot;
  uint32_t nextPartitionRelativeWriteOffset;
  uint64_t nextSegmentSequence;
  uint64_t selectedMetadataGeneration;
  uint64_t bytesWritten;
  uint32_t maximumRecords;
  uint32_t preparationSectorsCompleted;
  uint32_t preparationSectorsTotal;
  uint32_t preparationSegmentsCompleted;
  uint32_t preparationSegmentsTotal;
  uint32_t firstStm32Sequence;
  uint32_t lastStm32Sequence;
  uint64_t firstLogicalRecordIndex;
  uint64_t lastLogicalRecordIndex;
  uint64_t firstRetainedLogicalIndex;
  uint64_t lastRetainedLogicalIndex;
  uint64_t nextLogicalRecordIndex;
  uint32_t producerInFlight;
  SessionLoggerError lastError;
};

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
struct SessionFlushDiagnostics {
  bool available;
  bool flushActive;
  bool lastFlushSuccessful;
  bool hasLastSequenceBeforeFlush;
  bool hasFirstSequenceAfterFlush;
  bool acceptedInvariantSatisfied;
  bool offeredInvariantSatisfied;
  uint64_t flushStartTimestampUs;
  uint64_t flushEndTimestampUs;
  uint64_t flushDurationUs;
  uint64_t maximumWriteDurationUs;
  uint64_t maximumEraseDurationUs;
  uint32_t recordsRequested;
  uint32_t recordsSuccessfullyWritten;
  uint32_t segmentsErased;
  uint32_t uartBytesAvailableBefore;
  uint32_t uartBytesAvailableAfter;
  uint64_t uartFifoOverflowCount;
  uint64_t uartRingBufferOverflowCount;
  uint64_t packetChecksumFailureCount;
  uint64_t packetResynchronizationCount;
  uint64_t packetSequenceGapCount;
  uint64_t sequenceGapsDuringFlush;
  uint64_t checksumValidPacketCount;
  uint64_t checksumValidAtFlushStart;
  uint64_t checksumValidAtFlushEnd;
  uint64_t checksumValidDuringFlush;
  uint64_t loggerSubmissionAttempts;
  uint64_t loggerAcceptedSubmissions;
  uint64_t loggerRejectedSubmissions;
  uint32_t lastSequenceBeforeFlush;
  uint32_t firstSequenceAfterFlush;
  uint32_t fifoHighWaterMark;
  uint64_t loggerDroppedRecordCount;
  uint64_t validPacketsOffered;
  uint64_t acceptedRecords;
  uint64_t successfullyWrittenRecords;
  uint64_t fifoPendingRecords;
  uint64_t workerInFlightRecords;
};
#endif

struct RamPacketRecordMetadata {
  uint64_t captureTimestampUs;
  uint64_t logicalRecordIndex;
  uint32_t stm32Sequence;
};

static_assert(sizeof(uint8_t) == 1, "A byte must be 8 bits");
static_assert(sizeof(uint32_t) == 4, "uint32_t must be 32 bits");
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 64 bits");
static_assert(SESSION_FIFO_ALLOCATION_BYTES == 3063200,
              "The 700-slot PSRAM allocation size must remain explicit");
