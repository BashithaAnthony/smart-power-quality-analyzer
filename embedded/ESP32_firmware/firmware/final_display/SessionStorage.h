#pragma once

#include <Arduino.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "FlashRecordCodec.h"
#include "SessionStorageFormats.h"
#include "TestConsoleConfig.h"

#ifndef SESSION_STORAGE_DEBUG
#define SESSION_STORAGE_DEBUG 0
#endif

constexpr uint32_t SESSION_STORAGE_PARTITION_BYTES = 0x00C00000U;
constexpr uint32_t SESSION_STORAGE_RESERVED_BYTES = 0x00010000U;
constexpr uint32_t SESSION_STORAGE_SEGMENT_BYTES = 0x00010000U;
constexpr uint32_t SESSION_STORAGE_SEGMENT_HEADER_BYTES =
    SESSION_SEGMENT_HEADER_BYTES;
constexpr uint32_t SESSION_STORAGE_RECORDS_PER_SEGMENT =
    SESSION_RECORDS_PER_SEGMENT;
constexpr uint32_t SESSION_STORAGE_DATA_SEGMENTS =
    SESSION_DATA_SEGMENT_COUNT;
constexpr uint32_t SESSION_STORAGE_MAX_RECORDS =
    SESSION_MAX_RETAINED_RECORDS;

static_assert(SESSION_STORAGE_RESERVED_BYTES == SESSION_STORAGE_SEGMENT_BYTES,
              "The first 64 KiB pqlog segment is reserved for metadata");
static_assert(
    (SESSION_STORAGE_PARTITION_BYTES / SESSION_STORAGE_SEGMENT_BYTES) - 1U ==
        SESSION_STORAGE_DATA_SEGMENTS,
    "pqlog must contain exactly 191 Stage 3 data segments");
static_assert(SESSION_STORAGE_SEGMENT_HEADER_BYTES +
                      SESSION_STORAGE_RECORDS_PER_SEGMENT *
                          FLASH_RECORD_BYTES ==
                  0x0000F2E0U,
              "Fourteen records must end at segment offset 0xF2E0");
static_assert(SESSION_STORAGE_SEGMENT_HEADER_BYTES +
                      (SESSION_STORAGE_RECORDS_PER_SEGMENT + 1U) *
                          FLASH_RECORD_BYTES >
                  SESSION_STORAGE_SEGMENT_BYTES,
              "A fifteenth record must not fit in a data segment");
static_assert(SESSION_STORAGE_DATA_SEGMENTS *
                      SESSION_STORAGE_RECORDS_PER_SEGMENT ==
                  SESSION_STORAGE_MAX_RECORDS,
              "Stage 3 maximum retained record count must be 2674");
static_assert(FLASH_RECORD_COMMIT_OFFSET + 4U == FLASH_RECORD_BYTES,
              "The record commit marker must be the final four bytes");
static_assert(SESSION_METADATA_COPY_B_OFFSET +
                      SESSION_METADATA_SECTOR_BYTES <=
                  SESSION_STORAGE_RESERVED_BYTES,
              "Both metadata sectors must remain in reserved pqlog space");
static_assert(SESSION_METADATA_COPY_A_OFFSET == 0U &&
                  SESSION_METADATA_COPY_B_OFFSET ==
                      SESSION_METADATA_SECTOR_BYTES,
              "Metadata copies must occupy the first two distinct sectors");
static_assert(SESSION_STORAGE_RESERVED_BYTES % 0x1000U == 0U &&
                  SESSION_STORAGE_SEGMENT_BYTES % 0x1000U == 0U,
              "Metadata and data geometry must be 4 KiB aligned");
static_assert(SESSION_METADATA_COPY_A_OFFSET % 0x1000U == 0U &&
                  SESSION_METADATA_COPY_B_OFFSET % 0x1000U == 0U &&
                  SESSION_STORAGE_SEGMENT_BYTES % 0x1000U == 0U,
              "Every erase boundary must be 4 KiB aligned");
static_assert(SESSION_STORAGE_RESERVED_BYTES +
                      SESSION_STORAGE_DATA_SEGMENTS *
                          SESSION_STORAGE_SEGMENT_BYTES ==
                  SESSION_STORAGE_PARTITION_BYTES,
              "Reserved metadata and all data segments must fill pqlog");

enum class SessionStorageError : uint8_t {
  None = 0,
  PartitionNotFound,
  AddressMismatch,
  SizeMismatch,
  EndAddressMismatch,
  EncryptedPartitionUnsupported,
  NotAvailable,
  NotPrepared,
  StoragePreparationRequired,
  StorageCapacityReached,
  RetainedSessionExists,
  NoRetainedSession,
  InvalidPersistentState,
  InvalidInterval,
  RecoveryBlocked,
  MetadataCopiesConflict,
  MetadataGenerationExhausted,
  AllocationFailed,
  NullArgument,
  BufferTooShort,
  GeometryOutOfBounds,
  LogicalIndexMismatch,
  SessionIdMismatch,
  CodecEncodeFailed,
  FlashEraseFailed,
  FlashWriteFailed,
  CommitWriteFailed,
  FlashReadFailed,
  ReadbackValidationFailed,
  RecordMetadataMismatch,
  MetadataEncodeFailed,
  MetadataValidationFailed,
  SegmentHeaderEncodeFailed,
  SegmentHeaderValidationFailed,
  RecordGapOrCorruption,
  ReaderTokenExhausted,
  ReaderAlreadyOpen,
  ReaderNotOpen,
  ReaderStale,
  ReaderEnd,
  InvalidSynchronizationTransition,
  InvalidTimeAnchor
};

struct SessionStorageStartInfo {
  uint64_t sessionId;
  uint64_t firstLogicalRecordIndex;
  uint64_t firstSegmentSequence;
};

struct SessionStorageStatus {
  bool found;
  bool available;
  bool prepared;
  bool preparationInProgress;
  bool dataAreaPrepared;
  bool storageCapacityReached;
  bool recoveryBlocked;
  bool metadataAValid;
  bool metadataBValid;
  uint8_t selectedMetadataCopy;  // 0 none, 1 copy A, 2 copy B.
  bool recoveryPerformed;
  bool recoveredInterrupted;
  bool countersPartial;
  bool corruptionOrGap;
  bool storageTruncated;
  bool finalized;
  bool stopDrainComplete;
  bool readerOpen;
  char partitionLabel[17];
  uint8_t type;
  uint8_t subtype;
  uint32_t address;
  uint32_t size;
  uint64_t endAddress;
  uint64_t selectedMetadataGeneration;
  uint64_t sourceMetadataGeneration;
  PersistentSessionState persistentSessionState;
  SessionSynchronizationState synchronizationState;
  uint64_t sessionId;
  uint32_t selectedIntervalSeconds;
  uint32_t bootId;
  uint64_t startUptimeUs;
  uint64_t endUptimeUs;
  uint64_t startWallClockUnixMs;
  uint64_t endWallClockUnixMs;
  bool startWallClockValid;
  bool endWallClockValid;
  uint64_t sessionStartLogicalIndex;
  uint64_t firstRetainedLogicalIndex;
  uint64_t lastRetainedLogicalIndex;
  uint64_t nextLogicalRecordIndex;
  uint32_t firstStm32Sequence;
  uint32_t lastStm32Sequence;
  uint64_t storedRecordCount;
  uint64_t totalStoredRecords;
  uint64_t retainedRecordCount;
  uint64_t overwrittenRecordCount;
  uint64_t droppedRecordCount;
  uint16_t currentDataSegment;
  uint16_t currentSlotInSegment;
  uint16_t oldestPhysicalSegment;
  uint16_t oldestPhysicalSlot;
  uint64_t nextSegmentSequence;
  uint32_t nextPartitionRelativeWriteOffset;
  uint32_t maximumRecords;
  uint32_t validSegmentCount;
  uint32_t erasedSegmentCount;
  uint32_t preparationSectorsCompleted;
  uint32_t preparationSectorsTotal;
  uint32_t preparationSegmentsCompleted;
  uint32_t preparationSegmentsTotal;
  uint64_t bytesWritten;
  uint64_t flashEraseFailureCount;
  uint64_t flashWriteFailureCount;
  uint64_t flashReadFailureCount;
  uint64_t codecValidationFailureCount;
  uint64_t metadataValidationFailureCount;
  uint64_t segmentValidationFailureCount;
  uint64_t recordValidationFailureCount;
  int32_t lastEspError;
  SessionStorageError validationError;
  SessionStorageError lastError;
  FlashRecordCodecError lastCodecError;
  SessionStorageFormatError lastFormatError;
};

struct SessionStorageIoDiagnostics {
  uint64_t eraseOperationCount;
  uint64_t writeOperationCount;
  uint64_t totalEraseDurationUs;
  uint64_t totalWriteDurationUs;
  uint64_t maximumEraseDurationUs;
  uint64_t maximumWriteDurationUs;
};

struct RetainedSessionInfo {
  bool available;
  bool finalized;
  bool recoveredIncomplete;
  bool storageTruncated;
  bool countersPartial;
  bool corruptionOrGap;
  uint64_t metadataGeneration;
  uint64_t sourceMetadataGeneration;
  uint64_t sessionId;
  SessionSynchronizationState synchronizationState;
  uint32_t selectedIntervalSeconds;
  uint32_t bootId;
  uint64_t startUptimeUs;
  uint64_t endUptimeUs;
  uint64_t startWallClockUnixMs;
  uint64_t endWallClockUnixMs;
  bool startWallClockValid;
  bool endWallClockValid;
  uint64_t firstRetainedLogicalIndex;
  uint64_t lastRetainedLogicalIndex;
  uint64_t totalStoredRecords;
  uint64_t retainedRecordCount;
  uint64_t overwrittenRecordCount;
  uint64_t droppedRecordCount;
  uint32_t firstStm32Sequence;
  uint32_t lastStm32Sequence;
};

struct SessionStorageReader {
  bool open;
  bool storageTruncated;
  uint64_t readerToken;
  uint64_t metadataGeneration;
  uint64_t sessionId;
  uint64_t nextLogicalRecordIndex;
  uint64_t remainingRecords;
};

class SessionStorage {
 public:
  SessionStorage() = default;
  ~SessionStorage();

  SessionStorage(const SessionStorage&) = delete;
  SessionStorage& operator=(const SessionStorage&) = delete;

  // Discovers pqlog and performs recovery scanning. If pqlog has no
  // Stage 3 metadata and no valid Stage 3 segment headers, begin() commits one
  // Empty metadata generation. A valid but unaccounted Stage 3 segment header
  // blocks new sessions until clearRetainedSession() is called explicitly.
  bool begin();
  bool isAvailable() const;
  bool canStartNewSession() const;

  // These transition methods perform synchronous raw-flash operations and
  // must be invoked by the storage worker, never the UART/UI path.
  // prepareEmptyDataArea() is allowed only while persistent storage is Empty.
  // It erases all data sectors before packet admission; it never touches the
  // reserved metadata area or retained-session data.
  SessionStorageError prepareEmptyDataArea();
  SessionStorageError startNewSession(
      uint32_t selectedIntervalSeconds,
      uint32_t bootId,
      uint64_t startUptimeUs,
      SessionStorageStartInfo& startInfo,
      uint64_t startWallClockUnixMs = 0U,
      bool startWallClockValid = false);
  SessionStorageError markStopping(uint64_t droppedRecordCount);
  SessionStorageError finalizeSession(
      uint64_t droppedRecordCount,
      uint64_t endUptimeUs,
      uint64_t endWallClockUnixMs = 0U,
      bool endWallClockValid = false);
  SessionStorageError clearRetainedSession();
  SessionStorageError rescanRetainedStorage();

  SessionStorageError appendRecord(
      const uint8_t* packetBytes,
      uint32_t packetLength,
      uint64_t sessionId,
      uint64_t logicalRecordIndex,
      uint64_t captureTimestampUs,
      uint32_t stm32Sequence,
      uint16_t packetFormatVersion,
      uint16_t flags,
      uint32_t bootId,
      uint8_t* encodedRecordBuffer,
      uint32_t encodedRecordBufferLength,
      uint8_t* readbackBuffer,
      uint32_t readbackBufferLength);

  RetainedSessionInfo getRetainedSessionInfo() const;
  SessionStorageError openChronologicalReader(SessionStorageReader& reader);
  SessionStorageError readNextEncodedRecord(
      SessionStorageReader& reader,
      uint8_t* destination,
      uint32_t destinationLength,
      FlashRecordMetadata& recordMetadata);
  SessionStorageError readNextRecord(
      SessionStorageReader& reader,
      uint8_t* recordScratchBuffer,
      uint32_t recordScratchBufferLength,
      uint8_t* packetDestination,
      uint32_t packetDestinationLength,
      FlashRecordMetadata& recordMetadata);
  SessionStorageError transitionSynchronizationState(
      SessionStorageReader& reader,
      SessionSynchronizationState desiredState);
  void closeReader(SessionStorageReader& reader);

  SessionStorageStatus getStatus() const;
  void resetIoDiagnostics();
  SessionStorageIoDiagnostics getIoDiagnostics() const;

 private:
  static constexpr uint32_t kEraseSectorBytes = 0x1000U;
  static constexpr uint32_t kEraseSectorsPerSegment =
      SESSION_STORAGE_SEGMENT_BYTES / kEraseSectorBytes;
  static constexpr uint32_t kPreparationSectorCount =
      SESSION_STORAGE_DATA_SEGMENTS * kEraseSectorsPerSegment;

  static_assert(kEraseSectorsPerSegment == 16U,
                "Each 64 KiB data segment has sixteen erase sectors");
  static_assert(kPreparationSectorCount == 3056U,
                "The pqlog data area has 3056 erase sectors");

  static uint32_t segmentOffset(uint16_t dataSegmentIndex);
  static uint32_t recordOffset(uint16_t dataSegmentIndex,
                               uint16_t slotInSegment);
  bool partitionRangeValid(uint32_t offset, uint32_t length) const;
  bool metadataSectorRangeValid(uint32_t offset, uint32_t length) const;
  bool dataRangeValid(uint32_t offset, uint32_t length) const;
  bool recordRangeValid(uint16_t dataSegmentIndex,
                        uint16_t slotInSegment,
                        uint32_t offset) const;
  esp_err_t erasePartitionRange(uint32_t offset, uint32_t length);
  esp_err_t writePartitionRange(uint32_t offset,
                                const void* source,
                                uint32_t length);

  SessionStorageError loadMetadataCopies(
      MetadataSelectionResult& selection);
  SessionStorageError commitMetadata(PersistentSessionMetadata& metadata);
  SessionStorageError scanSegmentHeaders(bool filterCurrentSession,
                                         bool& anyValidStage3Header);
  SessionStorageError reconstructCurrentSession(bool commitRecoveryState);
  SessionStorageError openCurrentSegment(uint64_t firstLogicalRecordIndex,
                                         uint64_t creationUptimeUs,
                                         uint32_t bootId);
  SessionStorageError readValidatedRecord(uint16_t physicalSegment,
                                          uint16_t slot,
                                          uint8_t* destination,
                                          FlashRecordMetadata& metadata,
                                          bool countFailure);
  SessionStorageError readNextRecordLocked(
      SessionStorageReader& reader,
      uint8_t* recordBuffer,
      uint32_t recordBufferLength,
      uint8_t* packetDestination,
      uint32_t packetDestinationLength,
      FlashRecordMetadata& recordMetadata);

  int16_t findSummaryByPhysical(uint16_t physicalSegment) const;
  void removeSummaryAt(uint16_t summaryIndex);
  bool addSummary(const StorageSegmentSummary& summary);
  void applyMetadataToStatusLocked();
  void setError(SessionStorageError error,
                int32_t espError = ESP_OK,
                FlashRecordCodecError codecError =
                    FlashRecordCodecError::Ok,
                SessionStorageFormatError formatError =
                    SessionStorageFormatError::Ok);
  void pauseAfterFailure(SessionStorageError error,
                         int32_t espError = ESP_OK,
                         FlashRecordCodecError codecError =
                             FlashRecordCodecError::Ok,
                         SessionStorageFormatError formatError =
                             SessionStorageFormatError::Ok);
  void releaseBuffers();

  const esp_partition_t* partition_ = nullptr;
  bool begun_ = false;
  bool currentSegmentOpen_ = false;
  bool dataAreaPrepared_ = false;
  bool readerOpen_ = false;
  uint64_t activeReaderToken_ = 0U;
  uint64_t activeReaderGeneration_ = 0U;
  uint64_t activeReaderSessionId_ = 0U;
  uint64_t nextReaderToken_ = 1U;
  bool segmentScanCorruption_ = false;
  bool copyAValid_ = false;
  bool copyBValid_ = false;
  uint64_t copyAGeneration_ = 0U;
  uint64_t copyBGeneration_ = 0U;
  uint8_t selectedMetadataCopy_ = 0U;
  uint64_t discoveredNextSessionIdFloor_ = 1U;
  uint64_t discoveredNextLogicalIndexFloor_ = 0U;
  uint64_t discoveredNextSegmentSequenceFloor_ = 1U;
  uint16_t segmentSummaryCount_ = 0U;
  StorageSegmentSummary segmentSummaries_[SESSION_STORAGE_DATA_SEGMENTS]{};
  uint32_t segmentBootIds_[SESSION_STORAGE_DATA_SEGMENTS]{};
  StorageRecordLocation* recoveryLocations_ = nullptr;
  uint8_t* recoveryRecordBuffer_ = nullptr;
  SemaphoreHandle_t operationMutex_ = nullptr;
  PersistentSessionMetadata metadata_{};
  SessionStorageStatus status_{};
  SessionStorageIoDiagnostics ioDiagnostics_{};
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
