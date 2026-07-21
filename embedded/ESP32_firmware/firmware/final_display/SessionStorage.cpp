#include "SessionStorage.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>

#include "AcquisitionDiagnostics.h"
#include "AcquisitionPolicy.h"
#include "WallClockTypes.h"

namespace {

constexpr esp_partition_type_t kPqlogType =
    static_cast<esp_partition_type_t>(0x40);
constexpr esp_partition_subtype_t kPqlogSubtype =
    static_cast<esp_partition_subtype_t>(0x00);
constexpr char kPqlogLabel[] = "pqlog";
constexpr uint32_t kExpectedAddress = 0x00400000U;
constexpr uint64_t kExpectedEndAddress = 0x01000000ULL;

static_assert(ACQUISITION_FLASH_ERASE_SLICE_BYTES == 0x1000U,
              "Segment erases must remain bounded to one flash sector");

bool hasMagic(const uint8_t* bytes,
              char first,
              char second,
              char third,
              char fourth) {
  return bytes != nullptr && bytes[0] == static_cast<uint8_t>(first) &&
         bytes[1] == static_cast<uint8_t>(second) &&
         bytes[2] == static_cast<uint8_t>(third) &&
         bytes[3] == static_cast<uint8_t>(fourth);
}

uint64_t maximum64(uint64_t left, uint64_t right) {
  return left > right ? left : right;
}

bool checkedSuccessor(uint64_t value, uint64_t& successor) {
  if (value == UINT64_MAX) {
    return false;
  }
  successor = value + 1U;
  return true;
}

bool checkedAdd(uint64_t value, uint64_t increment, uint64_t& result) {
  if (value > UINT64_MAX - increment) {
    return false;
  }
  result = value + increment;
  return true;
}

bool intervalSupported(uint32_t seconds) {
  return seconds == 1U || seconds == 5U || seconds == 10U ||
         seconds == 60U;
}

bool isRetainedState(PersistentSessionState state) {
  return state == PersistentSessionState::Finalized ||
         state == PersistentSessionState::RecoveredIncomplete;
}

bool synchronizationStateValid(SessionSynchronizationState state) {
  switch (state) {
    case SessionSynchronizationState::NotSynced:
    case SessionSynchronizationState::Synced:
    case SessionSynchronizationState::Uploading:
    case SessionSynchronizationState::SyncError:
      return true;
    default:
      return false;
  }
}

bool metadataPayloadEqual(const PersistentSessionMetadata& left,
                          const PersistentSessionMetadata& right) {
  return left.sourceMetadataGeneration == right.sourceMetadataGeneration &&
         left.sessionId == right.sessionId &&
         left.state == right.state &&
         left.synchronizationState == right.synchronizationState &&
         left.flags == right.flags &&
         left.selectedFlushIntervalSeconds ==
             right.selectedFlushIntervalSeconds &&
         left.bootId == right.bootId &&
         left.startUptimeUs == right.startUptimeUs &&
         left.endUptimeUs == right.endUptimeUs &&
         left.startWallClockUnixMs == right.startWallClockUnixMs &&
         left.endWallClockUnixMs == right.endWallClockUnixMs &&
         left.sessionStartLogicalIndex == right.sessionStartLogicalIndex &&
         left.firstRetainedLogicalIndex ==
             right.firstRetainedLogicalIndex &&
         left.lastRetainedLogicalIndex == right.lastRetainedLogicalIndex &&
         left.nextGlobalLogicalIndex == right.nextGlobalLogicalIndex &&
         left.totalStoredRecords == right.totalStoredRecords &&
         left.retainedRecordCount == right.retainedRecordCount &&
         left.overwrittenRecordCount == right.overwrittenRecordCount &&
         left.droppedRecordCount == right.droppedRecordCount &&
         left.nextSessionId == right.nextSessionId &&
         left.nextSegmentSequence == right.nextSegmentSequence &&
         left.firstStm32Sequence == right.firstStm32Sequence &&
         left.lastStm32Sequence == right.lastStm32Sequence &&
         left.oldestPhysicalSegment == right.oldestPhysicalSegment &&
         left.oldestPhysicalSlot == right.oldestPhysicalSlot &&
         left.nextWritePhysicalSegment == right.nextWritePhysicalSegment &&
         left.nextWriteSlot == right.nextWriteSlot;
}

class SemaphoreGuard {
 public:
  explicit SemaphoreGuard(SemaphoreHandle_t semaphore)
      : semaphore_(semaphore),
        locked_(semaphore != nullptr &&
                xSemaphoreTake(semaphore, portMAX_DELAY) == pdTRUE) {}

  ~SemaphoreGuard() {
    if (locked_) {
      xSemaphoreGive(semaphore_);
    }
  }

  bool locked() const { return locked_; }

 private:
  SemaphoreHandle_t semaphore_;
  bool locked_;
};

}  // namespace

SessionStorage::~SessionStorage() {
  releaseBuffers();
}

bool SessionStorage::begin() {
  if (begun_) {
    return isAvailable();
  }
  begun_ = true;

  portENTER_CRITICAL(&mux_);
  status_ = SessionStorageStatus{};
  status_.persistentSessionState = PersistentSessionState::Empty;
  status_.oldestPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
  status_.oldestPhysicalSlot = SESSION_INVALID_PHYSICAL_INDEX;
  status_.currentDataSegment = SESSION_INVALID_PHYSICAL_INDEX;
  status_.currentSlotInSegment = SESSION_INVALID_PHYSICAL_INDEX;
  status_.maximumRecords = SESSION_STORAGE_MAX_RECORDS;
  status_.preparationSectorsTotal = kPreparationSectorCount;
  status_.preparationSegmentsTotal = SESSION_STORAGE_DATA_SEGMENTS;
  status_.lastEspError = ESP_OK;
  status_.lastCodecError = FlashRecordCodecError::Ok;
  status_.lastFormatError = SessionStorageFormatError::Ok;
  portEXIT_CRITICAL(&mux_);

  partition_ = esp_partition_find_first(
      kPqlogType, kPqlogSubtype, kPqlogLabel);
  if (partition_ == nullptr) {
    portENTER_CRITICAL(&mux_);
    status_.validationError = SessionStorageError::PartitionNotFound;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::PartitionNotFound);
    return false;
  }

  portENTER_CRITICAL(&mux_);
  status_.found = true;
  status_.type = static_cast<uint8_t>(partition_->type);
  status_.subtype = static_cast<uint8_t>(partition_->subtype);
  status_.address = partition_->address;
  status_.size = partition_->size;
  status_.endAddress = static_cast<uint64_t>(partition_->address) +
                       static_cast<uint64_t>(partition_->size);
  status_.partitionLabel[0] = '\0';
  if (partition_->label != nullptr) {
    strncpy(status_.partitionLabel,
            partition_->label,
            sizeof(status_.partitionLabel) - 1U);
    status_.partitionLabel[sizeof(status_.partitionLabel) - 1U] = '\0';
  }
  portEXIT_CRITICAL(&mux_);

  SessionStorageError geometryError = SessionStorageError::None;
  if (partition_->address != kExpectedAddress) {
    geometryError = SessionStorageError::AddressMismatch;
  } else if (partition_->size != SESSION_STORAGE_PARTITION_BYTES) {
    geometryError = SessionStorageError::SizeMismatch;
  } else if (static_cast<uint64_t>(partition_->address) +
                 static_cast<uint64_t>(partition_->size) !=
             kExpectedEndAddress) {
    geometryError = SessionStorageError::EndAddressMismatch;
  } else if (partition_->encrypted) {
    geometryError = SessionStorageError::EncryptedPartitionUnsupported;
  }
  if (geometryError != SessionStorageError::None) {
    portENTER_CRITICAL(&mux_);
    status_.validationError = geometryError;
    portEXIT_CRITICAL(&mux_);
    setError(geometryError);
    return false;
  }

  portENTER_CRITICAL(&mux_);
  status_.available = true;
  status_.validationError = SessionStorageError::None;
  portEXIT_CRITICAL(&mux_);

  operationMutex_ = xSemaphoreCreateMutex();
  if (operationMutex_ == nullptr) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::AllocationFailed);
    return true;
  }

  recoveryLocations_ = static_cast<StorageRecordLocation*>(
      heap_caps_malloc(sizeof(StorageRecordLocation) *
                           SESSION_STORAGE_MAX_RECORDS,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  recoveryRecordBuffer_ = static_cast<uint8_t*>(
      heap_caps_malloc(FLASH_RECORD_BYTES,
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (recoveryLocations_ == nullptr || recoveryRecordBuffer_ == nullptr) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::AllocationFailed);
    return true;
  }

  const SessionStorageError recoveryResult = rescanRetainedStorage();
  if (recoveryResult != SessionStorageError::None) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    portEXIT_CRITICAL(&mux_);
  }

#if SESSION_STORAGE_DEBUG
  const SessionStorageStatus snapshot = getStatus();
  Serial.print("Session storage: pqlog available=");
  Serial.print(snapshot.available ? "yes" : "no");
  Serial.print(", metadata generation=");
  Serial.print(static_cast<unsigned long long>(
      snapshot.selectedMetadataGeneration));
  Serial.print(", persistent state=");
  Serial.println(static_cast<uint32_t>(snapshot.persistentSessionState));
#endif

  return true;
}

bool SessionStorage::isAvailable() const {
  portENTER_CRITICAL(&mux_);
  const bool available = status_.available;
  portEXIT_CRITICAL(&mux_);
  return available;
}

bool SessionStorage::canStartNewSession() const {
  portENTER_CRITICAL(&mux_);
  const bool allowed = status_.available && !status_.prepared &&
                       !status_.preparationInProgress &&
                       !status_.recoveryBlocked && !readerOpen_ &&
                       recoveryLocations_ != nullptr &&
                       recoveryRecordBuffer_ != nullptr &&
                       metadata_.state == PersistentSessionState::Empty;
  portEXIT_CRITICAL(&mux_);
  return allowed;
}

SessionStorageError SessionStorage::prepareEmptyDataArea() {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  portENTER_CRITICAL(&mux_);
  const bool allowed = status_.available && !status_.prepared &&
                       !status_.recoveryBlocked && !readerOpen_ &&
                       metadata_.state == PersistentSessionState::Empty;
  if (allowed) {
    status_.preparationInProgress = true;
    status_.dataAreaPrepared = false;
    status_.storageCapacityReached = false;
    status_.preparationSectorsCompleted = 0U;
    status_.preparationSectorsTotal = kPreparationSectorCount;
    status_.preparationSegmentsCompleted = 0U;
    status_.preparationSegmentsTotal = SESSION_STORAGE_DATA_SEGMENTS;
    status_.lastError = SessionStorageError::None;
  }
  dataAreaPrepared_ = false;
  portEXIT_CRITICAL(&mux_);
  if (!allowed) {
    setError(SessionStorageError::InvalidPersistentState);
    return SessionStorageError::InvalidPersistentState;
  }

  // The Empty metadata state is the user's destructive authorization. Erase
  // the data area once, before Active packet admission. After a power loss a
  // later Start repeats preparation rather than trusting a partial erase.
  for (uint16_t segment = 0U;
       segment < SESSION_STORAGE_DATA_SEGMENTS;
       ++segment) {
    const uint32_t baseOffset = segmentOffset(segment);
    if (!dataRangeValid(baseOffset, SESSION_STORAGE_SEGMENT_BYTES)) {
      portENTER_CRITICAL(&mux_);
      status_.preparationInProgress = false;
      portEXIT_CRITICAL(&mux_);
      setError(SessionStorageError::GeometryOutOfBounds);
      return SessionStorageError::GeometryOutOfBounds;
    }
    for (uint32_t sector = 0U;
         sector < kEraseSectorsPerSegment;
         ++sector) {
      const uint32_t sectorOffset =
          baseOffset + sector * kEraseSectorBytes;
      if (!dataRangeValid(sectorOffset, kEraseSectorBytes)) {
        portENTER_CRITICAL(&mux_);
        status_.preparationInProgress = false;
        portEXIT_CRITICAL(&mux_);
        setError(SessionStorageError::GeometryOutOfBounds);
        return SessionStorageError::GeometryOutOfBounds;
      }
      const esp_err_t eraseResult = erasePartitionRange(
          sectorOffset, kEraseSectorBytes);
      if (eraseResult != ESP_OK) {
        portENTER_CRITICAL(&mux_);
        ++status_.flashEraseFailureCount;
        status_.preparationInProgress = false;
        portEXIT_CRITICAL(&mux_);
        setError(SessionStorageError::FlashEraseFailed, eraseResult);
        return SessionStorageError::FlashEraseFailed;
      }
      portENTER_CRITICAL(&mux_);
      ++status_.preparationSectorsCompleted;
      portEXIT_CRITICAL(&mux_);
      vTaskDelay(ACQUISITION_FLASH_SERVICE_YIELD_TICKS);
    }
    portENTER_CRITICAL(&mux_);
    ++status_.preparationSegmentsCompleted;
    portEXIT_CRITICAL(&mux_);
  }

  segmentSummaryCount_ = 0U;
  memset(segmentSummaries_, 0, sizeof(segmentSummaries_));
  memset(segmentBootIds_, 0, sizeof(segmentBootIds_));
  currentSegmentOpen_ = false;
  segmentScanCorruption_ = false;
  portENTER_CRITICAL(&mux_);
  dataAreaPrepared_ = true;
  status_.preparationInProgress = false;
  status_.dataAreaPrepared = true;
  status_.storageCapacityReached = false;
  status_.validSegmentCount = 0U;
  status_.erasedSegmentCount = 0U;
  status_.lastError = SessionStorageError::None;
  status_.lastEspError = ESP_OK;
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::startNewSession(
    uint32_t selectedIntervalSeconds,
    uint32_t bootId,
    uint64_t startUptimeUs,
    SessionStorageStartInfo& startInfo,
    uint64_t startWallClockUnixMs,
    bool startWallClockValid) {
  startInfo = SessionStorageStartInfo{};
  if (!intervalSupported(selectedIntervalSeconds)) {
    setError(SessionStorageError::InvalidInterval);
    return SessionStorageError::InvalidInterval;
  }
  const WallClockSnapshot startAnchor{
      startWallClockValid ? startWallClockUnixMs : 0U,
      startUptimeUs,
      bootId,
      startWallClockValid};
  if (!HistoricalTime::isValidSnapshot(startAnchor)) {
    setError(SessionStorageError::InvalidTimeAnchor);
    return SessionStorageError::InvalidTimeAnchor;
  }

  portENTER_CRITICAL(&mux_);
  const bool available = status_.available;
  const bool blocked = status_.recoveryBlocked;
  const bool prepared = status_.prepared;
  const bool dataAreaPrepared = dataAreaPrepared_;
  const bool readerOpen = readerOpen_;
  const PersistentSessionMetadata previous = metadata_;
  portEXIT_CRITICAL(&mux_);
  if (!available) {
    setError(SessionStorageError::NotAvailable);
    return SessionStorageError::NotAvailable;
  }
  if (recoveryLocations_ == nullptr || recoveryRecordBuffer_ == nullptr) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }
  if (blocked) {
    setError(SessionStorageError::RecoveryBlocked);
    return SessionStorageError::RecoveryBlocked;
  }
  if (!dataAreaPrepared) {
    setError(SessionStorageError::StoragePreparationRequired);
    return SessionStorageError::StoragePreparationRequired;
  }
  if (prepared || readerOpen ||
      previous.state != PersistentSessionState::Empty) {
    setError(SessionStorageError::RetainedSessionExists);
    return SessionStorageError::RetainedSessionExists;
  }

  PersistentSessionMetadata next{};
  next.state = PersistentSessionState::Active;
  next.synchronizationState =
      SessionSynchronizationState::NotSynced;
  next.selectedFlushIntervalSeconds = selectedIntervalSeconds;
  next.bootId = bootId;
  next.startUptimeUs = startUptimeUs;
  next.startWallClockUnixMs =
      startWallClockValid ? startWallClockUnixMs : 0U;
  if (startWallClockValid) {
    next.flags |= SessionMetadataStartWallTimeValid;
  }

  next.sessionId = maximum64(
      previous.nextSessionId, discoveredNextSessionIdFloor_);
  if (next.sessionId == 0U) {
    next.sessionId = 1U;
  }
  if (next.sessionId == UINT64_MAX) {
    setError(SessionStorageError::MetadataGenerationExhausted);
    return SessionStorageError::MetadataGenerationExhausted;
  }
  next.nextSessionId = next.sessionId + 1U;
  next.sessionStartLogicalIndex = maximum64(
      previous.nextGlobalLogicalIndex,
      discoveredNextLogicalIndexFloor_);
  if (next.sessionStartLogicalIndex == UINT64_MAX) {
    setError(SessionStorageError::MetadataGenerationExhausted);
    return SessionStorageError::MetadataGenerationExhausted;
  }
  next.nextGlobalLogicalIndex = next.sessionStartLogicalIndex;
  next.firstRetainedLogicalIndex = 0U;
  next.lastRetainedLogicalIndex = 0U;
  next.oldestPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
  next.oldestPhysicalSlot = SESSION_INVALID_PHYSICAL_INDEX;
  next.nextWritePhysicalSegment = 0U;
  next.nextWriteSlot = 0U;
  next.nextSegmentSequence = maximum64(
      previous.nextSegmentSequence,
      discoveredNextSegmentSequenceFloor_);
  if (next.nextSegmentSequence == 0U) {
    next.nextSegmentSequence = 1U;
  }
  if (next.nextSegmentSequence == UINT64_MAX) {
    setError(SessionStorageError::MetadataGenerationExhausted);
    return SessionStorageError::MetadataGenerationExhausted;
  }

  const uint64_t firstSegmentSequence = next.nextSegmentSequence;
  const SessionStorageError commitResult = commitMetadata(next);
  if (commitResult != SessionStorageError::None) {
    return commitResult;
  }

  segmentSummaryCount_ = 0U;
  memset(segmentSummaries_, 0, sizeof(segmentSummaries_));
  memset(segmentBootIds_, 0, sizeof(segmentBootIds_));
  currentSegmentOpen_ = false;
  segmentScanCorruption_ = false;
  portENTER_CRITICAL(&mux_);
  status_.prepared = true;
  status_.dataAreaPrepared = true;
  status_.storageCapacityReached = false;
  status_.recoveryBlocked =
      recoveryLocations_ == nullptr || recoveryRecordBuffer_ == nullptr;
  status_.recoveryPerformed = false;
  status_.stopDrainComplete = false;
  status_.erasedSegmentCount = 0U;
  status_.bytesWritten = 0U;
  status_.lastError = SessionStorageError::None;
  status_.lastEspError = ESP_OK;
  status_.lastCodecError = FlashRecordCodecError::Ok;
  status_.lastFormatError = SessionStorageFormatError::Ok;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);

  startInfo.sessionId = next.sessionId;
  startInfo.firstLogicalRecordIndex = next.sessionStartLogicalIndex;
  startInfo.firstSegmentSequence = firstSegmentSequence;
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::markStopping(
    uint64_t droppedRecordCount) {
  portENTER_CRITICAL(&mux_);
  const bool prepared = status_.prepared;
  PersistentSessionMetadata next = metadata_;
  portEXIT_CRITICAL(&mux_);
  if (!prepared || next.state != PersistentSessionState::Active) {
    setError(SessionStorageError::InvalidPersistentState);
    return SessionStorageError::InvalidPersistentState;
  }
  next.state = PersistentSessionState::Stopping;
  next.droppedRecordCount = droppedRecordCount;
  // Do not erase/program a metadata sector while acquisition is Active or
  // Stopping. The last durable state remains Active, which the existing boot
  // recovery path correctly treats as an interrupted session. Finalization
  // commits the complete Stopped metadata only after admission and draining.
  portENTER_CRITICAL(&mux_);
  metadata_ = next;
  status_.prepared = true;
  status_.stopDrainComplete = false;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::finalizeSession(
    uint64_t droppedRecordCount,
    uint64_t endUptimeUs,
    uint64_t endWallClockUnixMs,
    bool endWallClockValid) {
  portENTER_CRITICAL(&mux_);
  const bool prepared = status_.prepared;
  PersistentSessionMetadata next = metadata_;
  portEXIT_CRITICAL(&mux_);
  if (!prepared || next.state != PersistentSessionState::Stopping) {
    setError(SessionStorageError::InvalidPersistentState);
    return SessionStorageError::InvalidPersistentState;
  }
  const WallClockSnapshot startAnchor{
      (next.flags & SessionMetadataStartWallTimeValid) != 0U
          ? next.startWallClockUnixMs
          : 0U,
      next.startUptimeUs,
      next.bootId,
      (next.flags & SessionMetadataStartWallTimeValid) != 0U};
  const WallClockSnapshot endAnchor{
      endWallClockValid ? endWallClockUnixMs : 0U,
      endUptimeUs,
      next.bootId,
      endWallClockValid};
  if (!HistoricalTime::isValidEndSnapshot(startAnchor, endAnchor)) {
    setError(SessionStorageError::InvalidTimeAnchor);
    return SessionStorageError::InvalidTimeAnchor;
  }
  if (next.retainedRecordCount > SESSION_STORAGE_MAX_RECORDS ||
      next.totalStoredRecords < next.retainedRecordCount ||
      next.totalStoredRecords - next.retainedRecordCount !=
          next.overwrittenRecordCount) {
    pauseAfterFailure(SessionStorageError::RecordGapOrCorruption);
    return SessionStorageError::RecordGapOrCorruption;
  }

  next.state = PersistentSessionState::Finalized;
  next.droppedRecordCount = droppedRecordCount;
  next.endUptimeUs = endUptimeUs;
  next.endWallClockUnixMs =
      endWallClockValid ? endWallClockUnixMs : 0U;
  next.flags |= SessionMetadataFinalized;
  next.flags &= ~(SessionMetadataInterruptedRecovered |
                  SessionMetadataCounterPartial |
                  SessionMetadataCorruptionOrGap);
  if (endWallClockValid) {
    next.flags |= SessionMetadataEndWallTimeValid;
  } else {
    next.flags &= ~SessionMetadataEndWallTimeValid;
  }

  const SessionStorageError result = commitMetadata(next);
  if (result != SessionStorageError::None) {
    pauseAfterFailure(result);
    return result;
  }
  portENTER_CRITICAL(&mux_);
  status_.prepared = false;
  dataAreaPrepared_ = false;
  status_.dataAreaPrepared = false;
  status_.stopDrainComplete = true;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::clearRetainedSession() {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  portENTER_CRITICAL(&mux_);
  const bool available = status_.available;
  const bool prepared = status_.prepared;
  const bool readerOpen = readerOpen_;
  const PersistentSessionMetadata previous = metadata_;
  portEXIT_CRITICAL(&mux_);
  if (!available) {
    setError(SessionStorageError::NotAvailable);
    return SessionStorageError::NotAvailable;
  }
  if (prepared || readerOpen ||
      previous.state == PersistentSessionState::Active ||
      previous.state == PersistentSessionState::Stopping) {
    setError(SessionStorageError::InvalidPersistentState);
    return SessionStorageError::InvalidPersistentState;
  }

  PersistentSessionMetadata empty{};
  empty.state = PersistentSessionState::Empty;
  empty.synchronizationState =
      SessionSynchronizationState::NotSynced;
  empty.nextSessionId = maximum64(
      previous.nextSessionId, discoveredNextSessionIdFloor_);
  if (empty.nextSessionId == 0U) {
    empty.nextSessionId = 1U;
  }
  empty.nextGlobalLogicalIndex = maximum64(
      previous.nextGlobalLogicalIndex,
      discoveredNextLogicalIndexFloor_);
  empty.nextSegmentSequence = maximum64(
      previous.nextSegmentSequence,
      discoveredNextSegmentSequenceFloor_);
  if (empty.nextSegmentSequence == 0U) {
    empty.nextSegmentSequence = 1U;
  }
  empty.oldestPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
  empty.oldestPhysicalSlot = SESSION_INVALID_PHYSICAL_INDEX;
  empty.nextWritePhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
  empty.nextWriteSlot = SESSION_INVALID_PHYSICAL_INDEX;

  const SessionStorageError result = commitMetadata(empty);
  if (result != SessionStorageError::None) {
    return result;
  }

  segmentSummaryCount_ = 0U;
  memset(segmentSummaries_, 0, sizeof(segmentSummaries_));
  memset(segmentBootIds_, 0, sizeof(segmentBootIds_));
  currentSegmentOpen_ = false;
  segmentScanCorruption_ = false;
  portENTER_CRITICAL(&mux_);
  status_.prepared = false;
  dataAreaPrepared_ = false;
  status_.dataAreaPrepared = false;
  status_.preparationInProgress = false;
  status_.storageCapacityReached = false;
  status_.recoveryBlocked =
      recoveryLocations_ == nullptr || recoveryRecordBuffer_ == nullptr;
  status_.recoveryPerformed = false;
  status_.recoveredInterrupted = false;
  status_.countersPartial = false;
  status_.corruptionOrGap = false;
  status_.stopDrainComplete = false;
  status_.validSegmentCount = 0U;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::rescanRetainedStorage() {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  portENTER_CRITICAL(&mux_);
  const bool available = status_.available;
  const bool busy = status_.prepared || readerOpen_;
  portEXIT_CRITICAL(&mux_);
  if (!available) {
    setError(SessionStorageError::NotAvailable);
    return SessionStorageError::NotAvailable;
  }
  if (busy) {
    setError(SessionStorageError::InvalidPersistentState);
    return SessionStorageError::InvalidPersistentState;
  }
  if (recoveryLocations_ == nullptr || recoveryRecordBuffer_ == nullptr) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  MetadataSelectionResult selection = MetadataSelectionResult::NoneValid;
  const SessionStorageError metadataResult = loadMetadataCopies(selection);
  if (metadataResult != SessionStorageError::None) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    portEXIT_CRITICAL(&mux_);
    return metadataResult;
  }

  const bool filterCurrentSession =
      selection == MetadataSelectionResult::CopyA ||
      selection == MetadataSelectionResult::CopyB;
  bool anyValidStage3Header = false;
  const SessionStorageError scanResult = scanSegmentHeaders(
      filterCurrentSession &&
          metadata_.state != PersistentSessionState::Empty,
      anyValidStage3Header);
  if (scanResult != SessionStorageError::None) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    portEXIT_CRITICAL(&mux_);
    return scanResult;
  }

  if (selection == MetadataSelectionResult::Conflict) {
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = true;
    status_.corruptionOrGap = true;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::MetadataCopiesConflict);
    return SessionStorageError::MetadataCopiesConflict;
  }

  if (selection == MetadataSelectionResult::NoneValid) {
    if (anyValidStage3Header) {
      portENTER_CRITICAL(&mux_);
      status_.recoveryBlocked = true;
      status_.corruptionOrGap = true;
      portEXIT_CRITICAL(&mux_);
      setError(SessionStorageError::RecoveryBlocked);
      return SessionStorageError::RecoveryBlocked;
    }

    PersistentSessionMetadata empty{};
    empty.state = PersistentSessionState::Empty;
    empty.synchronizationState =
        SessionSynchronizationState::NotSynced;
    empty.nextSessionId = discoveredNextSessionIdFloor_;
    empty.nextGlobalLogicalIndex = discoveredNextLogicalIndexFloor_;
    empty.nextSegmentSequence = discoveredNextSegmentSequenceFloor_;
    if (empty.nextSessionId == 0U) {
      empty.nextSessionId = 1U;
    }
    if (empty.nextSegmentSequence == 0U) {
      empty.nextSegmentSequence = 1U;
    }
    empty.oldestPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
    empty.oldestPhysicalSlot = SESSION_INVALID_PHYSICAL_INDEX;
    empty.nextWritePhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
    empty.nextWriteSlot = SESSION_INVALID_PHYSICAL_INDEX;
    const SessionStorageError createResult = commitMetadata(empty);
    if (createResult != SessionStorageError::None) {
      portENTER_CRITICAL(&mux_);
      status_.recoveryBlocked = true;
      portEXIT_CRITICAL(&mux_);
      return createResult;
    }
    portENTER_CRITICAL(&mux_);
    status_.recoveryBlocked = false;
    status_.recoveryPerformed = true;
    status_.prepared = false;
    dataAreaPrepared_ = false;
    status_.dataAreaPrepared = false;
    portEXIT_CRITICAL(&mux_);
    return SessionStorageError::None;
  }

  if (metadata_.state == PersistentSessionState::Empty) {
    segmentSummaryCount_ = 0U;
    memset(segmentSummaries_, 0, sizeof(segmentSummaries_));
    memset(segmentBootIds_, 0, sizeof(segmentBootIds_));
    portENTER_CRITICAL(&mux_);
    status_.validSegmentCount = 0U;
    status_.prepared = false;
    dataAreaPrepared_ = false;
    status_.dataAreaPrepared = false;
    status_.recoveryBlocked = false;
    status_.recoveryPerformed = true;
    applyMetadataToStatusLocked();
    portEXIT_CRITICAL(&mux_);
    return SessionStorageError::None;
  }

  const SessionStorageError reconstruction =
      reconstructCurrentSession(true);
  portENTER_CRITICAL(&mux_);
  status_.recoveryPerformed = true;
  status_.prepared = false;
  dataAreaPrepared_ = false;
  status_.dataAreaPrepared = false;
  status_.recoveryBlocked =
      reconstruction != SessionStorageError::None;
  portEXIT_CRITICAL(&mux_);
  return reconstruction;
}

SessionStorageError SessionStorage::appendRecord(
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
    uint32_t readbackBufferLength) {
  if (packetBytes == nullptr || encodedRecordBuffer == nullptr ||
      readbackBuffer == nullptr) {
    pauseAfterFailure(SessionStorageError::NullArgument);
    return SessionStorageError::NullArgument;
  }
  if (packetLength != FLASH_RECORD_PAYLOAD_BYTES ||
      encodedRecordBufferLength < FLASH_RECORD_BYTES ||
      readbackBufferLength < FLASH_RECORD_BYTES) {
    pauseAfterFailure(SessionStorageError::BufferTooShort);
    return SessionStorageError::BufferTooShort;
  }
  if (logicalRecordIndex == UINT64_MAX) {
    pauseAfterFailure(SessionStorageError::LogicalIndexMismatch);
    return SessionStorageError::LogicalIndexMismatch;
  }

  portENTER_CRITICAL(&mux_);
  const bool prepared = status_.prepared;
  const PersistentSessionState state = metadata_.state;
  const uint64_t expectedSessionId = metadata_.sessionId;
  const uint64_t expectedLogicalIndex = metadata_.nextGlobalLogicalIndex;
  const uint16_t physicalSegment = metadata_.nextWritePhysicalSegment;
  const uint16_t slot = metadata_.nextWriteSlot;
  const uint64_t totalStoredRecords = metadata_.totalStoredRecords;
  portEXIT_CRITICAL(&mux_);
  if (!prepared ||
      (state != PersistentSessionState::Active &&
       state != PersistentSessionState::Stopping)) {
    setError(SessionStorageError::NotPrepared);
    return SessionStorageError::NotPrepared;
  }
  if (sessionId != expectedSessionId) {
    pauseAfterFailure(SessionStorageError::SessionIdMismatch);
    return SessionStorageError::SessionIdMismatch;
  }
  if (logicalRecordIndex != expectedLogicalIndex ||
      totalStoredRecords == UINT64_MAX) {
    pauseAfterFailure(SessionStorageError::LogicalIndexMismatch);
    return SessionStorageError::LogicalIndexMismatch;
  }
  if (totalStoredRecords >= SESSION_STORAGE_MAX_RECORDS) {
    portENTER_CRITICAL(&mux_);
    status_.storageCapacityReached = true;
    status_.lastError = SessionStorageError::StorageCapacityReached;
    portEXIT_CRITICAL(&mux_);
    return SessionStorageError::StorageCapacityReached;
  }
  if (physicalSegment >= SESSION_STORAGE_DATA_SEGMENTS ||
      slot >= SESSION_STORAGE_RECORDS_PER_SEGMENT) {
    pauseAfterFailure(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }

  const FlashRecordCodecError encodeResult = FlashRecordCodec::encode(
      encodedRecordBuffer,
      encodedRecordBufferLength,
      packetBytes,
      packetLength,
      sessionId,
      logicalRecordIndex,
      captureTimestampUs,
      stm32Sequence,
      packetFormatVersion,
      flags,
      bootId);
  if (encodeResult != FlashRecordCodecError::Ok) {
    portENTER_CRITICAL(&mux_);
    ++status_.codecValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::CodecEncodeFailed,
                      ESP_OK,
                      encodeResult);
    return SessionStorageError::CodecEncodeFailed;
  }

  if (!currentSegmentOpen_) {
    const SessionStorageError openResult = openCurrentSegment(
        logicalRecordIndex, captureTimestampUs, bootId);
    if (openResult != SessionStorageError::None) {
      return openResult;
    }
  }

  portENTER_CRITICAL(&mux_);
  const uint16_t writeSegment = metadata_.nextWritePhysicalSegment;
  const uint16_t writeSlot = metadata_.nextWriteSlot;
  portEXIT_CRITICAL(&mux_);
  const uint32_t writeOffset = recordOffset(writeSegment, writeSlot);
  if (!recordRangeValid(writeSegment, writeSlot, writeOffset)) {
    pauseAfterFailure(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }

  uint32_t recordBodyWritten = 0U;
  while (recordBodyWritten < FLASH_RECORD_COMMIT_OFFSET) {
    const uint32_t remaining =
        FLASH_RECORD_COMMIT_OFFSET - recordBodyWritten;
    const uint32_t writeBytes =
        remaining < ACQUISITION_FLASH_WRITE_SLICE_BYTES
            ? remaining
            : ACQUISITION_FLASH_WRITE_SLICE_BYTES;
    const esp_err_t bodyWriteResult = writePartitionRange(
        writeOffset + recordBodyWritten,
        encodedRecordBuffer + recordBodyWritten,
        writeBytes);
    if (bodyWriteResult != ESP_OK) {
      portENTER_CRITICAL(&mux_);
      ++status_.flashWriteFailureCount;
      portEXIT_CRITICAL(&mux_);
      pauseAfterFailure(
          SessionStorageError::FlashWriteFailed, bodyWriteResult);
      return SessionStorageError::FlashWriteFailed;
    }
    recordBodyWritten += writeBytes;
    // Leave every partial record uncommitted while the UART path gets a
    // scheduling opportunity between bounded flash program operations.
    vTaskDelay(ACQUISITION_FLASH_SERVICE_YIELD_TICKS);
  }

  const uint32_t commitOffset = writeOffset + FLASH_RECORD_COMMIT_OFFSET;
  if (!dataRangeValid(commitOffset, 4U)) {
    pauseAfterFailure(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }
  esp_err_t espResult = writePartitionRange(
      commitOffset,
      encodedRecordBuffer + FLASH_RECORD_COMMIT_OFFSET,
      4U);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashWriteFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::CommitWriteFailed, espResult);
    return SessionStorageError::CommitWriteFailed;
  }

  espResult = esp_partition_read(
      partition_, writeOffset, readbackBuffer, FLASH_RECORD_BYTES);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashReadFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::FlashReadFailed, espResult);
    return SessionStorageError::FlashReadFailed;
  }

  const FlashRecordCodecError validation =
      FlashRecordCodec::validate(readbackBuffer, FLASH_RECORD_BYTES);
  if (validation != FlashRecordCodecError::Ok ||
      memcmp(encodedRecordBuffer,
             readbackBuffer,
             FLASH_RECORD_BYTES) != 0) {
    portENTER_CRITICAL(&mux_);
    ++status_.codecValidationFailureCount;
    ++status_.recordValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::ReadbackValidationFailed,
                      ESP_OK,
                      validation);
    return SessionStorageError::ReadbackValidationFailed;
  }

  FlashRecordMetadata decoded{};
  const FlashRecordCodecError decodeResult =
      FlashRecordCodec::decodeMetadata(
          readbackBuffer, FLASH_RECORD_BYTES, decoded);
  if (decodeResult != FlashRecordCodecError::Ok ||
      decoded.sessionId != sessionId ||
      decoded.logicalRecordIndex != logicalRecordIndex ||
      decoded.captureTimestampUs != captureTimestampUs ||
      decoded.stm32Sequence != stm32Sequence ||
      decoded.packetFormatVersion != packetFormatVersion ||
      decoded.flags != flags || decoded.bootId != bootId) {
    portENTER_CRITICAL(&mux_);
    ++status_.codecValidationFailureCount;
    ++status_.recordValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::RecordMetadataMismatch,
                      ESP_OK,
                      decodeResult);
    return SessionStorageError::RecordMetadataMismatch;
  }

  const int16_t summaryIndex = findSummaryByPhysical(writeSegment);
  if (summaryIndex < 0) {
    pauseAfterFailure(SessionStorageError::SegmentHeaderValidationFailed);
    return SessionStorageError::SegmentHeaderValidationFailed;
  }

  portENTER_CRITICAL(&mux_);
  StorageSegmentSummary& summary = segmentSummaries_[summaryIndex];
  summary.validRecordMask = static_cast<uint16_t>(
      summary.validRecordMask | (1U << writeSlot));
  const bool firstStoredRecord = metadata_.totalStoredRecords == 0U;
  const bool firstRetainedRecord = metadata_.retainedRecordCount == 0U;
  ++metadata_.totalStoredRecords;
  ++metadata_.retainedRecordCount;
  metadata_.nextGlobalLogicalIndex = logicalRecordIndex + 1U;
  metadata_.lastRetainedLogicalIndex = logicalRecordIndex;
  metadata_.lastStm32Sequence = stm32Sequence;
  metadata_.flags |= SessionMetadataHasRetainedRecords;
  if (firstStoredRecord) {
    metadata_.firstStm32Sequence = stm32Sequence;
  }
  if (firstRetainedRecord) {
    metadata_.firstRetainedLogicalIndex = logicalRecordIndex;
    metadata_.oldestPhysicalSegment = writeSegment;
    metadata_.oldestPhysicalSlot = writeSlot;
  }

  ++metadata_.nextWriteSlot;
  if (metadata_.nextWriteSlot >= SESSION_STORAGE_RECORDS_PER_SEGMENT) {
    metadata_.nextWriteSlot = 0U;
    metadata_.nextWritePhysicalSegment =
        SessionStorageFormats::nextPhysicalSegment(writeSegment);
    currentSegmentOpen_ = false;
  }
  status_.bytesWritten += FLASH_RECORD_BYTES;
  status_.lastError = SessionStorageError::None;
  status_.lastEspError = ESP_OK;
  status_.lastCodecError = FlashRecordCodecError::Ok;
  status_.lastFormatError = SessionStorageFormatError::Ok;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);

  return SessionStorageError::None;
}

RetainedSessionInfo SessionStorage::getRetainedSessionInfo() const {
  portENTER_CRITICAL(&mux_);
  RetainedSessionInfo info{};
  info.available = isRetainedState(metadata_.state);
  info.finalized = metadata_.state == PersistentSessionState::Finalized;
  info.recoveredIncomplete =
      metadata_.state == PersistentSessionState::RecoveredIncomplete;
  info.storageTruncated =
      (metadata_.flags & SessionMetadataStorageTruncated) != 0U;
  info.countersPartial =
      (metadata_.flags & SessionMetadataCounterPartial) != 0U;
  info.corruptionOrGap =
      (metadata_.flags & SessionMetadataCorruptionOrGap) != 0U;
  info.metadataGeneration = metadata_.generation;
  info.sourceMetadataGeneration =
      metadata_.sourceMetadataGeneration != 0U
          ? metadata_.sourceMetadataGeneration
          : metadata_.generation;
  info.sessionId = metadata_.sessionId;
  info.synchronizationState = metadata_.synchronizationState;
  info.selectedIntervalSeconds =
      metadata_.selectedFlushIntervalSeconds;
  info.bootId = metadata_.bootId;
  info.startUptimeUs = metadata_.startUptimeUs;
  info.endUptimeUs = metadata_.endUptimeUs;
  info.startWallClockUnixMs = metadata_.startWallClockUnixMs;
  info.endWallClockUnixMs = metadata_.endWallClockUnixMs;
  info.startWallClockValid =
      (metadata_.flags & SessionMetadataStartWallTimeValid) != 0U;
  info.endWallClockValid =
      (metadata_.flags & SessionMetadataEndWallTimeValid) != 0U;
  info.firstRetainedLogicalIndex =
      metadata_.firstRetainedLogicalIndex;
  info.lastRetainedLogicalIndex = metadata_.lastRetainedLogicalIndex;
  info.totalStoredRecords = metadata_.totalStoredRecords;
  info.retainedRecordCount = metadata_.retainedRecordCount;
  info.overwrittenRecordCount = metadata_.overwrittenRecordCount;
  info.droppedRecordCount = metadata_.droppedRecordCount;
  info.firstStm32Sequence = metadata_.firstStm32Sequence;
  info.lastStm32Sequence = metadata_.lastStm32Sequence;
  portEXIT_CRITICAL(&mux_);
  return info;
}

SessionStorageError SessionStorage::openChronologicalReader(
    SessionStorageReader& reader) {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  portENTER_CRITICAL(&mux_);
  if (readerOpen_) {
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::ReaderAlreadyOpen);
    return SessionStorageError::ReaderAlreadyOpen;
  }
  if (!isRetainedState(metadata_.state)) {
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::NoRetainedSession);
    return SessionStorageError::NoRetainedSession;
  }
  if (nextReaderToken_ == UINT64_MAX) {
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::ReaderTokenExhausted);
    return SessionStorageError::ReaderTokenExhausted;
  }
  reader = SessionStorageReader{};
  reader.open = true;
  reader.readerToken = nextReaderToken_++;
  reader.storageTruncated =
      (metadata_.flags & SessionMetadataStorageTruncated) != 0U;
  reader.metadataGeneration = metadata_.generation;
  reader.sessionId = metadata_.sessionId;
  reader.nextLogicalRecordIndex =
      metadata_.firstRetainedLogicalIndex;
  reader.remainingRecords = metadata_.retainedRecordCount;
  readerOpen_ = true;
  activeReaderToken_ = reader.readerToken;
  activeReaderGeneration_ = reader.metadataGeneration;
  activeReaderSessionId_ = reader.sessionId;
  status_.readerOpen = true;
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::readNextEncodedRecord(
    SessionStorageReader& reader,
    uint8_t* destination,
    uint32_t destinationLength,
    FlashRecordMetadata& recordMetadata) {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  return readNextRecordLocked(reader,
                              destination,
                              destinationLength,
                              nullptr,
                              0U,
                              recordMetadata);
}

SessionStorageError SessionStorage::readNextRecord(
    SessionStorageReader& reader,
    uint8_t* recordScratchBuffer,
    uint32_t recordScratchBufferLength,
    uint8_t* packetDestination,
    uint32_t packetDestinationLength,
    FlashRecordMetadata& recordMetadata) {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  if (packetDestination == nullptr) {
    setError(SessionStorageError::NullArgument);
    return SessionStorageError::NullArgument;
  }

  return readNextRecordLocked(reader,
                              recordScratchBuffer,
                              recordScratchBufferLength,
                              packetDestination,
                              packetDestinationLength,
                              recordMetadata);
}

SessionStorageError SessionStorage::readNextRecordLocked(
    SessionStorageReader& reader,
    uint8_t* recordBuffer,
    uint32_t recordBufferLength,
    uint8_t* packetDestination,
    uint32_t packetDestinationLength,
    FlashRecordMetadata& recordMetadata) {
  if (recordBuffer == nullptr) {
    setError(SessionStorageError::NullArgument);
    return SessionStorageError::NullArgument;
  }
  if (recordBufferLength < FLASH_RECORD_BYTES ||
      (packetDestination != nullptr &&
       packetDestinationLength < FLASH_RECORD_PAYLOAD_BYTES)) {
    setError(SessionStorageError::BufferTooShort);
    return SessionStorageError::BufferTooShort;
  }

  uint16_t physicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
  uint16_t slot = SESSION_INVALID_PHYSICAL_INDEX;
  portENTER_CRITICAL(&mux_);
  if (!readerOpen_ || !reader.open ||
      reader.readerToken != activeReaderToken_ ||
      reader.metadataGeneration != activeReaderGeneration_ ||
      reader.sessionId != activeReaderSessionId_) {
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::ReaderNotOpen);
    return SessionStorageError::ReaderNotOpen;
  }
  if (reader.metadataGeneration != metadata_.generation ||
      reader.sessionId != metadata_.sessionId ||
      !isRetainedState(metadata_.state)) {
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::ReaderStale);
    return SessionStorageError::ReaderStale;
  }
  if (reader.remainingRecords == 0U) {
    portEXIT_CRITICAL(&mux_);
    return SessionStorageError::ReaderEnd;
  }
  const uint64_t logicalRecordIndex = reader.nextLogicalRecordIndex;
  const uint64_t readerSessionId = reader.sessionId;
  portEXIT_CRITICAL(&mux_);
  bool found = false;
  uint64_t selectedSegmentSequence = 0U;
  for (uint16_t index = 0U; index < segmentSummaryCount_; ++index) {
    const StorageSegmentSummary& summary = segmentSummaries_[index];
    if (!summary.valid || summary.sessionId != readerSessionId ||
        logicalRecordIndex < summary.firstLogicalRecordIndex) {
      continue;
    }
    const uint64_t delta =
        logicalRecordIndex - summary.firstLogicalRecordIndex;
    if (delta >= SESSION_STORAGE_RECORDS_PER_SEGMENT ||
        (summary.validRecordMask & (1U << delta)) == 0U ||
        (found && summary.segmentSequence <= selectedSegmentSequence)) {
      continue;
    }
    found = true;
    selectedSegmentSequence = summary.segmentSequence;
    physicalSegment = summary.physicalSegmentIndex;
    slot = static_cast<uint16_t>(delta);
  }
  if (!found) {
    setError(SessionStorageError::RecordGapOrCorruption);
    return SessionStorageError::RecordGapOrCorruption;
  }

  FlashRecordMetadata decoded{};
  const SessionStorageError readResult = readValidatedRecord(
      physicalSegment, slot, recordBuffer, decoded, true);
  if (readResult != SessionStorageError::None) {
    return readResult;
  }
  if (decoded.sessionId != readerSessionId ||
      decoded.logicalRecordIndex != logicalRecordIndex) {
    setError(SessionStorageError::RecordMetadataMismatch,
             ESP_OK,
             FlashRecordCodecError::Ok);
    return SessionStorageError::RecordMetadataMismatch;
  }
  if (packetDestination != nullptr) {
    const FlashRecordCodecError copyResult = FlashRecordCodec::copyPayload(
        recordBuffer,
        FLASH_RECORD_BYTES,
        packetDestination,
        packetDestinationLength);
    if (copyResult != FlashRecordCodecError::Ok) {
      portENTER_CRITICAL(&mux_);
      ++status_.codecValidationFailureCount;
      portEXIT_CRITICAL(&mux_);
      setError(SessionStorageError::ReadbackValidationFailed,
               ESP_OK,
               copyResult);
      return SessionStorageError::ReadbackValidationFailed;
    }
  }

  portENTER_CRITICAL(&mux_);
  if (!readerOpen_ || !reader.open ||
      reader.readerToken != activeReaderToken_ ||
      reader.metadataGeneration != metadata_.generation ||
      reader.sessionId != metadata_.sessionId) {
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::ReaderStale);
    return SessionStorageError::ReaderStale;
  }
  --reader.remainingRecords;
  if (reader.remainingRecords > 0U) {
    if (reader.nextLogicalRecordIndex == UINT64_MAX) {
      portEXIT_CRITICAL(&mux_);
      setError(SessionStorageError::RecordGapOrCorruption);
      return SessionStorageError::RecordGapOrCorruption;
    }
    ++reader.nextLogicalRecordIndex;
  }
  recordMetadata = decoded;
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::transitionSynchronizationState(
    SessionStorageReader& reader,
    SessionSynchronizationState desiredState) {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }
  if (!synchronizationStateValid(desiredState)) {
    setError(SessionStorageError::InvalidSynchronizationTransition);
    return SessionStorageError::InvalidSynchronizationTransition;
  }

  portENTER_CRITICAL(&mux_);
  const bool ownsActiveReader = readerOpen_ && reader.open &&
      reader.readerToken == activeReaderToken_ &&
      reader.metadataGeneration == activeReaderGeneration_ &&
      reader.sessionId == activeReaderSessionId_;
  const bool storageReady = status_.available && !status_.prepared &&
      !status_.recoveryBlocked && isRetainedState(metadata_.state) &&
      metadata_.retainedRecordCount > 0U;
  const bool readerCurrent = ownsActiveReader &&
      reader.metadataGeneration == metadata_.generation &&
      reader.sessionId == metadata_.sessionId;
  const SessionSynchronizationState currentState =
      metadata_.synchronizationState;
  PersistentSessionMetadata next = metadata_;
  portEXIT_CRITICAL(&mux_);

  if (!storageReady) {
    setError(SessionStorageError::InvalidPersistentState);
    return SessionStorageError::InvalidPersistentState;
  }
  if (!readerCurrent) {
    setError(ownsActiveReader ? SessionStorageError::ReaderStale
                             : SessionStorageError::ReaderNotOpen);
    return ownsActiveReader ? SessionStorageError::ReaderStale
                            : SessionStorageError::ReaderNotOpen;
  }
  if (desiredState == SessionSynchronizationState::NotSynced &&
      currentState != SessionSynchronizationState::NotSynced) {
    setError(SessionStorageError::InvalidSynchronizationTransition);
    return SessionStorageError::InvalidSynchronizationTransition;
  }
  if (desiredState == currentState) {
    return SessionStorageError::None;
  }

  if (next.sourceMetadataGeneration == 0U) {
    next.sourceMetadataGeneration = next.generation;
  }
  next.synchronizationState = desiredState;
  const SessionStorageError commitResult = commitMetadata(next);
  if (commitResult != SessionStorageError::None) {
    return commitResult;
  }

  portENTER_CRITICAL(&mux_);
  reader.metadataGeneration = next.generation;
  activeReaderGeneration_ = next.generation;
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

void SessionStorage::closeReader(SessionStorageReader& reader) {
  SemaphoreGuard operation(operationMutex_);
  if (!operation.locked()) {
    return;
  }

  portENTER_CRITICAL(&mux_);
  const bool ownsActiveReader = readerOpen_ && reader.open &&
      reader.readerToken == activeReaderToken_ &&
      reader.metadataGeneration == activeReaderGeneration_ &&
      reader.sessionId == activeReaderSessionId_;
  reader = SessionStorageReader{};
  if (ownsActiveReader) {
    readerOpen_ = false;
    activeReaderToken_ = 0U;
    activeReaderGeneration_ = 0U;
    activeReaderSessionId_ = 0U;
    status_.readerOpen = false;
  }
  portEXIT_CRITICAL(&mux_);
}

SessionStorageStatus SessionStorage::getStatus() const {
  portENTER_CRITICAL(&mux_);
  const SessionStorageStatus snapshot = status_;
  portEXIT_CRITICAL(&mux_);
  return snapshot;
}

void SessionStorage::resetIoDiagnostics() {
  portENTER_CRITICAL(&mux_);
  ioDiagnostics_ = SessionStorageIoDiagnostics{};
  portEXIT_CRITICAL(&mux_);
}

SessionStorageIoDiagnostics SessionStorage::getIoDiagnostics() const {
  portENTER_CRITICAL(&mux_);
  const SessionStorageIoDiagnostics snapshot = ioDiagnostics_;
  portEXIT_CRITICAL(&mux_);
  return snapshot;
}

uint32_t SessionStorage::segmentOffset(uint16_t dataSegmentIndex) {
  return SESSION_STORAGE_RESERVED_BYTES +
         static_cast<uint32_t>(dataSegmentIndex) *
             SESSION_STORAGE_SEGMENT_BYTES;
}

uint32_t SessionStorage::recordOffset(uint16_t dataSegmentIndex,
                                      uint16_t slotInSegment) {
  return segmentOffset(dataSegmentIndex) +
         SESSION_STORAGE_SEGMENT_HEADER_BYTES +
         static_cast<uint32_t>(slotInSegment) * FLASH_RECORD_BYTES;
}

bool SessionStorage::partitionRangeValid(uint32_t offset,
                                         uint32_t length) const {
  if (partition_ == nullptr || length == 0U) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(offset) + length;
  return end <= SESSION_STORAGE_PARTITION_BYTES &&
         end <= partition_->size;
}

bool SessionStorage::metadataSectorRangeValid(uint32_t offset,
                                              uint32_t length) const {
  if (!partitionRangeValid(offset, length)) {
    return false;
  }
  return length == SESSION_METADATA_SECTOR_BYTES &&
         (offset == SESSION_METADATA_COPY_A_OFFSET ||
          offset == SESSION_METADATA_COPY_B_OFFSET) &&
         static_cast<uint64_t>(offset) + length <=
             SESSION_STORAGE_RESERVED_BYTES;
}

bool SessionStorage::dataRangeValid(uint32_t offset,
                                    uint32_t length) const {
  return offset >= SESSION_STORAGE_RESERVED_BYTES &&
         partitionRangeValid(offset, length);
}

bool SessionStorage::recordRangeValid(uint16_t dataSegmentIndex,
                                      uint16_t slotInSegment,
                                      uint32_t offset) const {
  if (dataSegmentIndex >= SESSION_STORAGE_DATA_SEGMENTS ||
      slotInSegment >= SESSION_STORAGE_RECORDS_PER_SEGMENT ||
      offset != recordOffset(dataSegmentIndex, slotInSegment)) {
    return false;
  }
  const uint64_t start = segmentOffset(dataSegmentIndex);
  const uint64_t recordEnd = static_cast<uint64_t>(offset) +
                             FLASH_RECORD_BYTES;
  return offset >= start + SESSION_STORAGE_SEGMENT_HEADER_BYTES &&
         recordEnd <= start + SESSION_STORAGE_SEGMENT_BYTES &&
         dataRangeValid(offset, FLASH_RECORD_BYTES);
}

esp_err_t SessionStorage::erasePartitionRange(uint32_t offset,
                                              uint32_t length) {
  const AcquisitionFlashToken acquisitionToken =
      AcquisitionDiagnostics::beginFlashOperation(
          AcquisitionFlashOperation::Erase);
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  const int64_t startedUs = esp_timer_get_time();
#endif
  const esp_err_t result =
      esp_partition_erase_range(partition_, offset, length);
  AcquisitionDiagnostics::endFlashOperation(acquisitionToken);
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  const uint64_t durationUs = static_cast<uint64_t>(
      esp_timer_get_time() - startedUs);
  portENTER_CRITICAL(&mux_);
  ++ioDiagnostics_.eraseOperationCount;
  ioDiagnostics_.totalEraseDurationUs += durationUs;
  if (durationUs > ioDiagnostics_.maximumEraseDurationUs) {
    ioDiagnostics_.maximumEraseDurationUs = durationUs;
  }
  portEXIT_CRITICAL(&mux_);
#endif
  return result;
}

esp_err_t SessionStorage::writePartitionRange(uint32_t offset,
                                              const void* source,
                                              uint32_t length) {
  const AcquisitionFlashToken acquisitionToken =
      AcquisitionDiagnostics::beginFlashOperation(
          AcquisitionFlashOperation::Write);
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  const int64_t startedUs = esp_timer_get_time();
#endif
  const esp_err_t result =
      esp_partition_write(partition_, offset, source, length);
  AcquisitionDiagnostics::endFlashOperation(acquisitionToken);
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  const uint64_t durationUs = static_cast<uint64_t>(
      esp_timer_get_time() - startedUs);
  portENTER_CRITICAL(&mux_);
  ++ioDiagnostics_.writeOperationCount;
  ioDiagnostics_.totalWriteDurationUs += durationUs;
  if (durationUs > ioDiagnostics_.maximumWriteDurationUs) {
    ioDiagnostics_.maximumWriteDurationUs = durationUs;
  }
  portEXIT_CRITICAL(&mux_);
#endif
  return result;
}

SessionStorageError SessionStorage::loadMetadataCopies(
    MetadataSelectionResult& selection) {
  uint8_t copyA[SESSION_METADATA_ENTRY_BYTES]{};
  uint8_t copyB[SESSION_METADATA_ENTRY_BYTES]{};
  const esp_err_t readA = esp_partition_read(
      partition_,
      SESSION_METADATA_COPY_A_OFFSET,
      copyA,
      sizeof(copyA));
  const esp_err_t readB = esp_partition_read(
      partition_,
      SESSION_METADATA_COPY_B_OFFSET,
      copyB,
      sizeof(copyB));
  if (readA != ESP_OK || readB != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    status_.flashReadFailureCount +=
        static_cast<uint64_t>(readA != ESP_OK) +
        static_cast<uint64_t>(readB != ESP_OK);
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::FlashReadFailed,
             readA != ESP_OK ? readA : readB);
    return SessionStorageError::FlashReadFailed;
  }

  PersistentSessionMetadata selected{};
  bool copyAValid = false;
  bool copyBValid = false;
  selection = SessionStorageFormats::selectMetadataCopies(
      copyA,
      sizeof(copyA),
      copyB,
      sizeof(copyB),
      selected,
      copyAValid,
      copyBValid);

  PersistentSessionMetadata decodedA{};
  PersistentSessionMetadata decodedB{};
  const SessionStorageFormatError validationA =
      SessionStorageFormats::decodeMetadata(
          copyA, sizeof(copyA), decodedA);
  const SessionStorageFormatError validationB =
      SessionStorageFormats::decodeMetadata(
          copyB, sizeof(copyB), decodedB);
  const bool copyAHasMetadataMagic = hasMagic(copyA, 'P', 'Q', 'M', 'D');
  const bool copyBHasMetadataMagic = hasMagic(copyB, 'P', 'Q', 'M', 'D');

  portENTER_CRITICAL(&mux_);
  copyAValid_ = copyAValid;
  copyBValid_ = copyBValid;
  copyAGeneration_ = copyAValid ? decodedA.generation : 0U;
  copyBGeneration_ = copyBValid ? decodedB.generation : 0U;
  status_.metadataAValid = copyAValid;
  status_.metadataBValid = copyBValid;
  if (copyAHasMetadataMagic && !copyAValid) {
    ++status_.metadataValidationFailureCount;
  }
  if (copyBHasMetadataMagic && !copyBValid) {
    ++status_.metadataValidationFailureCount;
  }
  if (selection == MetadataSelectionResult::CopyA) {
    selectedMetadataCopy_ = 1U;
    metadata_ = selected;
  } else if (selection == MetadataSelectionResult::CopyB) {
    selectedMetadataCopy_ = 2U;
    metadata_ = selected;
  } else {
    selectedMetadataCopy_ = 0U;
    metadata_ = PersistentSessionMetadata{};
    metadata_.state = PersistentSessionState::Empty;
    metadata_.synchronizationState =
        SessionSynchronizationState::NotSynced;
    metadata_.nextSessionId = 1U;
    metadata_.nextSegmentSequence = 1U;
    metadata_.oldestPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
    metadata_.oldestPhysicalSlot = SESSION_INVALID_PHYSICAL_INDEX;
    metadata_.nextWritePhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
    metadata_.nextWriteSlot = SESSION_INVALID_PHYSICAL_INDEX;
  }
  status_.selectedMetadataCopy = selectedMetadataCopy_;
  applyMetadataToStatusLocked();
  if (selection == MetadataSelectionResult::Conflict) {
    status_.lastFormatError = SessionStorageFormatError::BadState;
  } else if (copyAHasMetadataMagic && !copyAValid) {
    status_.lastFormatError = validationA;
  } else if (copyBHasMetadataMagic && !copyBValid) {
    status_.lastFormatError = validationB;
  }
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::commitMetadata(
    PersistentSessionMetadata& metadata) {
  const uint64_t highestGeneration = maximum64(
      copyAValid_ ? copyAGeneration_ : 0U,
      copyBValid_ ? copyBGeneration_ : 0U);
  if (highestGeneration == UINT64_MAX) {
    setError(SessionStorageError::MetadataGenerationExhausted);
    return SessionStorageError::MetadataGenerationExhausted;
  }
  metadata.generation = highestGeneration + 1U;

  bool targetCopyA = false;
  if (!copyAValid_) {
    targetCopyA = true;
  } else if (!copyBValid_) {
    targetCopyA = false;
  } else {
    targetCopyA = copyAGeneration_ <= copyBGeneration_;
  }
  const uint32_t targetOffset = targetCopyA
      ? SESSION_METADATA_COPY_A_OFFSET
      : SESSION_METADATA_COPY_B_OFFSET;
  if (!metadataSectorRangeValid(
          targetOffset, SESSION_METADATA_SECTOR_BYTES)) {
    setError(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }

  alignas(4) uint8_t encoded[SESSION_METADATA_ENTRY_BYTES]{};
  alignas(4) uint8_t readback[SESSION_METADATA_ENTRY_BYTES]{};
  const SessionStorageFormatError encodeResult =
      SessionStorageFormats::encodeMetadata(
          encoded, sizeof(encoded), metadata);
  if (encodeResult != SessionStorageFormatError::Ok) {
    portENTER_CRITICAL(&mux_);
    ++status_.metadataValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::MetadataEncodeFailed,
             ESP_OK,
             FlashRecordCodecError::Ok,
             encodeResult);
    return SessionStorageError::MetadataEncodeFailed;
  }

  esp_err_t espResult = erasePartitionRange(
      targetOffset, SESSION_METADATA_SECTOR_BYTES);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashEraseFailureCount;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::FlashEraseFailed, espResult);
    return SessionStorageError::FlashEraseFailed;
  }

  // The target copy is invalid from the moment its sector erase succeeds.
  // The other metadata sector remains untouched and valid throughout.
  portENTER_CRITICAL(&mux_);
  if (targetCopyA) {
    copyAValid_ = false;
    copyAGeneration_ = 0U;
    status_.metadataAValid = false;
  } else {
    copyBValid_ = false;
    copyBGeneration_ = 0U;
    status_.metadataBValid = false;
  }
  portEXIT_CRITICAL(&mux_);

  // Yield before the small body/commit writes so UART reception can run
  // between the metadata erase and subsequent flash-program operations.
  vTaskDelay(ACQUISITION_FLASH_SERVICE_YIELD_TICKS);

  espResult = writePartitionRange(
      targetOffset, encoded, SESSION_METADATA_COMMIT_OFFSET);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashWriteFailureCount;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::FlashWriteFailed, espResult);
    return SessionStorageError::FlashWriteFailed;
  }
  vTaskDelay(ACQUISITION_FLASH_SERVICE_YIELD_TICKS);
  espResult = writePartitionRange(
      targetOffset + SESSION_METADATA_COMMIT_OFFSET,
      encoded + SESSION_METADATA_COMMIT_OFFSET,
      4U);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashWriteFailureCount;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::CommitWriteFailed, espResult);
    return SessionStorageError::CommitWriteFailed;
  }
  espResult = esp_partition_read(
      partition_, targetOffset, readback, sizeof(readback));
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashReadFailureCount;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::FlashReadFailed, espResult);
    return SessionStorageError::FlashReadFailed;
  }

  PersistentSessionMetadata verified{};
  const SessionStorageFormatError validation =
      SessionStorageFormats::decodeMetadata(
          readback, sizeof(readback), verified);
  if (validation != SessionStorageFormatError::Ok ||
      memcmp(encoded, readback, sizeof(encoded)) != 0) {
    portENTER_CRITICAL(&mux_);
    ++status_.metadataValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    setError(SessionStorageError::MetadataValidationFailed,
             ESP_OK,
             FlashRecordCodecError::Ok,
             validation);
    return SessionStorageError::MetadataValidationFailed;
  }

  portENTER_CRITICAL(&mux_);
  if (targetCopyA) {
    copyAValid_ = true;
    copyAGeneration_ = verified.generation;
    status_.metadataAValid = true;
    selectedMetadataCopy_ = 1U;
  } else {
    copyBValid_ = true;
    copyBGeneration_ = verified.generation;
    status_.metadataBValid = true;
    selectedMetadataCopy_ = 2U;
  }
  metadata_ = verified;
  status_.selectedMetadataCopy = selectedMetadataCopy_;
  status_.lastError = SessionStorageError::None;
  status_.lastEspError = ESP_OK;
  status_.lastCodecError = FlashRecordCodecError::Ok;
  status_.lastFormatError = SessionStorageFormatError::Ok;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);
  metadata = verified;
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::scanSegmentHeaders(
    bool filterCurrentSession,
    bool& anyValidStage3Header) {
  anyValidStage3Header = false;
  segmentScanCorruption_ = false;
  segmentSummaryCount_ = 0U;
  memset(segmentSummaries_, 0, sizeof(segmentSummaries_));
  memset(segmentBootIds_, 0, sizeof(segmentBootIds_));

  discoveredNextSessionIdFloor_ = maximum64(metadata_.nextSessionId, 1U);
  discoveredNextLogicalIndexFloor_ = metadata_.nextGlobalLogicalIndex;
  discoveredNextSegmentSequenceFloor_ = maximum64(
      metadata_.nextSegmentSequence, 1U);
  bool duplicateSegmentSequence = false;

  uint8_t headerBytes[SESSION_SEGMENT_HEADER_BYTES]{};
  for (uint16_t physical = 0U;
       physical < SESSION_STORAGE_DATA_SEGMENTS;
       ++physical) {
    const uint32_t offset = segmentOffset(physical);
    if (!dataRangeValid(offset, SESSION_SEGMENT_HEADER_BYTES)) {
      setError(SessionStorageError::GeometryOutOfBounds);
      return SessionStorageError::GeometryOutOfBounds;
    }
    const esp_err_t readResult = esp_partition_read(
        partition_, offset, headerBytes, sizeof(headerBytes));
    if (readResult != ESP_OK) {
      portENTER_CRITICAL(&mux_);
      ++status_.flashReadFailureCount;
      portEXIT_CRITICAL(&mux_);
      setError(SessionStorageError::FlashReadFailed, readResult);
      return SessionStorageError::FlashReadFailed;
    }

    const bool stage3Magic = hasMagic(headerBytes, 'P', 'Q', 'S', 'G');
    SessionSegmentHeader header{};
    const SessionStorageFormatError decodeResult =
        SessionStorageFormats::decodeSegmentHeader(
            headerBytes, sizeof(headerBytes), header, physical);
    if (decodeResult != SessionStorageFormatError::Ok) {
      if (stage3Magic) {
        portENTER_CRITICAL(&mux_);
        ++status_.segmentValidationFailureCount;
        status_.lastFormatError = decodeResult;
        portEXIT_CRITICAL(&mux_);
      }
      if ((physical & 0x0FU) == 0x0FU) {
        vTaskDelay(1);
      }
      continue;
    }

    anyValidStage3Header = true;
    uint64_t successor = 0U;
    if (checkedSuccessor(header.sessionId, successor)) {
      discoveredNextSessionIdFloor_ = maximum64(
          discoveredNextSessionIdFloor_, successor);
    } else {
      discoveredNextSessionIdFloor_ = UINT64_MAX;
    }
    if (checkedSuccessor(header.segmentSequence, successor)) {
      discoveredNextSegmentSequenceFloor_ = maximum64(
          discoveredNextSegmentSequenceFloor_, successor);
    } else {
      discoveredNextSegmentSequenceFloor_ = UINT64_MAX;
    }
    if (checkedAdd(header.firstLogicalRecordIndex,
                   SESSION_STORAGE_RECORDS_PER_SEGMENT,
                   successor)) {
      discoveredNextLogicalIndexFloor_ = maximum64(
          discoveredNextLogicalIndexFloor_, successor);
    } else {
      discoveredNextLogicalIndexFloor_ = UINT64_MAX;
    }

    if (filterCurrentSession && header.sessionId == metadata_.sessionId) {
      StorageSegmentSummary summary{};
      summary.valid = true;
      summary.physicalSegmentIndex = physical;
      summary.segmentSequence = header.segmentSequence;
      summary.sessionId = header.sessionId;
      summary.firstLogicalRecordIndex = header.firstLogicalRecordIndex;
      segmentBootIds_[physical] = header.bootId;
      if (!addSummary(summary)) {
        setError(SessionStorageError::GeometryOutOfBounds);
        return SessionStorageError::GeometryOutOfBounds;
      }
    }
    if ((physical & 0x0FU) == 0x0FU) {
      vTaskDelay(1);
    }
  }

  SessionStorageFormats::sortSegmentSummaries(
      segmentSummaries_, segmentSummaryCount_);
  for (uint16_t index = 1U; index < segmentSummaryCount_; ++index) {
    if (segmentSummaries_[index - 1U].segmentSequence ==
        segmentSummaries_[index].segmentSequence) {
      duplicateSegmentSequence = true;
    }
  }
  portENTER_CRITICAL(&mux_);
  status_.validSegmentCount = segmentSummaryCount_;
  if (duplicateSegmentSequence) {
    segmentScanCorruption_ = true;
    status_.corruptionOrGap = true;
  }
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::reconstructCurrentSession(
    bool commitRecoveryState) {
  if (recoveryLocations_ == nullptr || recoveryRecordBuffer_ == nullptr) {
    setError(SessionStorageError::AllocationFailed);
    return SessionStorageError::AllocationFailed;
  }

  const PersistentSessionMetadata persisted = metadata_;
  uint32_t recordCount = 0U;
  bool gapOrCorruption = segmentScanCorruption_;
  bool pendingHole = false;

  for (uint16_t summaryIndex = 1U;
       summaryIndex < segmentSummaryCount_;
       ++summaryIndex) {
    if (segmentSummaries_[summaryIndex - 1U].segmentSequence ==
        segmentSummaries_[summaryIndex].segmentSequence) {
      gapOrCorruption = true;
    }
  }

  for (uint16_t summaryIndex = 0U;
       summaryIndex < segmentSummaryCount_;
       ++summaryIndex) {
    StorageSegmentSummary& summary = segmentSummaries_[summaryIndex];
    summary.validRecordMask = 0U;
    for (uint16_t slot = 0U;
         slot < SESSION_STORAGE_RECORDS_PER_SEGMENT;
         ++slot) {
      const uint32_t offset = recordOffset(
          summary.physicalSegmentIndex, slot);
      if (!recordRangeValid(summary.physicalSegmentIndex, slot, offset)) {
        setError(SessionStorageError::GeometryOutOfBounds);
        return SessionStorageError::GeometryOutOfBounds;
      }
      const esp_err_t readResult = esp_partition_read(
          partition_,
          offset,
          recoveryRecordBuffer_,
          FLASH_RECORD_BYTES);
      if (readResult != ESP_OK) {
        portENTER_CRITICAL(&mux_);
        ++status_.flashReadFailureCount;
        portEXIT_CRITICAL(&mux_);
        setError(SessionStorageError::FlashReadFailed, readResult);
        return SessionStorageError::FlashReadFailed;
      }

      const bool recordMagic = hasMagic(
          recoveryRecordBuffer_, 'P', 'Q', 'R', '1');
      const FlashRecordCodecError validation =
          FlashRecordCodec::validate(
              recoveryRecordBuffer_, FLASH_RECORD_BYTES);
      if (validation != FlashRecordCodecError::Ok) {
        if (recordMagic) {
          portENTER_CRITICAL(&mux_);
          ++status_.recordValidationFailureCount;
          ++status_.codecValidationFailureCount;
          status_.lastCodecError = validation;
          portEXIT_CRITICAL(&mux_);
          if (validation !=
              FlashRecordCodecError::MissingCommitMarker) {
            gapOrCorruption = true;
          }
        }
        pendingHole = true;
        continue;
      }

      FlashRecordMetadata decoded{};
      const FlashRecordCodecError decodeResult =
          FlashRecordCodec::decodeMetadata(
              recoveryRecordBuffer_, FLASH_RECORD_BYTES, decoded);
      uint64_t expectedLogical = 0U;
      const bool expectedValid = checkedAdd(
          summary.firstLogicalRecordIndex, slot, expectedLogical);
      if (decodeResult != FlashRecordCodecError::Ok ||
          decoded.sessionId != persisted.sessionId || !expectedValid ||
          decoded.logicalRecordIndex != expectedLogical ||
          decoded.bootId !=
              segmentBootIds_[summary.physicalSegmentIndex]) {
        portENTER_CRITICAL(&mux_);
        ++status_.recordValidationFailureCount;
        if (decodeResult != FlashRecordCodecError::Ok) {
          ++status_.codecValidationFailureCount;
          status_.lastCodecError = decodeResult;
        }
        portEXIT_CRITICAL(&mux_);
        pendingHole = true;
        gapOrCorruption = true;
        continue;
      }

      if (pendingHole) {
        gapOrCorruption = true;
      }
      pendingHole = false;
      if (recordCount >= SESSION_STORAGE_MAX_RECORDS) {
        setError(SessionStorageError::GeometryOutOfBounds);
        return SessionStorageError::GeometryOutOfBounds;
      }
      StorageRecordLocation& location = recoveryLocations_[recordCount++];
      location.logicalRecordIndex = decoded.logicalRecordIndex;
      location.stm32Sequence = decoded.stm32Sequence;
      location.physicalSegmentIndex = summary.physicalSegmentIndex;
      location.slotIndex = slot;
    }
    vTaskDelay(1);
  }

  const ContiguousSuffixResult suffix =
      SessionStorageFormats::newestContiguousSuffix(
          recoveryLocations_, recordCount);
  gapOrCorruption = gapOrCorruption || suffix.gapOrDuplicateDetected;
  for (uint16_t index = 0U; index < segmentSummaryCount_; ++index) {
    segmentSummaries_[index].validRecordMask = 0U;
  }
  for (uint32_t index = suffix.startIndex;
       index < suffix.startIndex + suffix.count;
       ++index) {
    const StorageRecordLocation& location = recoveryLocations_[index];
    const int16_t summaryIndex =
        findSummaryByPhysical(location.physicalSegmentIndex);
    if (summaryIndex < 0 ||
        location.slotIndex >= SESSION_STORAGE_RECORDS_PER_SEGMENT) {
      setError(SessionStorageError::RecordGapOrCorruption);
      return SessionStorageError::RecordGapOrCorruption;
    }
    segmentSummaries_[summaryIndex].validRecordMask =
        static_cast<uint16_t>(
            segmentSummaries_[summaryIndex].validRecordMask |
            (1U << location.slotIndex));
  }

  PersistentSessionMetadata rebuilt = persisted;
  rebuilt.flags &= ~(SessionMetadataHasRetainedRecords |
                     SessionMetadataStorageTruncated |
                     SessionMetadataInterruptedRecovered |
                     SessionMetadataCounterPartial |
                     SessionMetadataFinalized |
                     SessionMetadataCorruptionOrGap);
  rebuilt.retainedRecordCount = suffix.count;
  rebuilt.firstRetainedLogicalIndex = 0U;
  rebuilt.lastRetainedLogicalIndex = 0U;
  rebuilt.firstStm32Sequence = 0U;
  rebuilt.lastStm32Sequence = 0U;
  rebuilt.oldestPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX;
  rebuilt.oldestPhysicalSlot = SESSION_INVALID_PHYSICAL_INDEX;

  uint64_t derivedTotalStored = 0U;
  if (suffix.count > 0U) {
    const StorageRecordLocation& oldest =
        recoveryLocations_[suffix.startIndex];
    const StorageRecordLocation& newest =
        recoveryLocations_[suffix.startIndex + suffix.count - 1U];
    rebuilt.flags |= SessionMetadataHasRetainedRecords;
    rebuilt.firstRetainedLogicalIndex = oldest.logicalRecordIndex;
    rebuilt.lastRetainedLogicalIndex = newest.logicalRecordIndex;
    rebuilt.firstStm32Sequence = oldest.stm32Sequence;
    rebuilt.lastStm32Sequence = newest.stm32Sequence;
    rebuilt.oldestPhysicalSegment = oldest.physicalSegmentIndex;
    rebuilt.oldestPhysicalSlot = oldest.slotIndex;
    if (newest.logicalRecordIndex < rebuilt.sessionStartLogicalIndex ||
        newest.logicalRecordIndex - rebuilt.sessionStartLogicalIndex ==
            UINT64_MAX) {
      gapOrCorruption = true;
      derivedTotalStored = suffix.count;
    } else {
      derivedTotalStored = newest.logicalRecordIndex -
                           rebuilt.sessionStartLogicalIndex + 1U;
    }
  }
  rebuilt.totalStoredRecords = derivedTotalStored;
  if (derivedTotalStored >= suffix.count) {
    rebuilt.overwrittenRecordCount = derivedTotalStored - suffix.count;
  } else {
    rebuilt.overwrittenRecordCount = 0U;
    gapOrCorruption = true;
  }
  if (rebuilt.overwrittenRecordCount > 0U) {
    rebuilt.flags |= SessionMetadataStorageTruncated;
  }

  uint64_t nextLogical = rebuilt.sessionStartLogicalIndex;
  if (suffix.count > 0U) {
    const uint64_t newest = rebuilt.lastRetainedLogicalIndex;
    if (!checkedSuccessor(newest, nextLogical)) {
      gapOrCorruption = true;
      nextLogical = UINT64_MAX;
    }
  }
  rebuilt.nextGlobalLogicalIndex = maximum64(
      nextLogical, discoveredNextLogicalIndexFloor_);
  rebuilt.nextSessionId = maximum64(
      persisted.nextSessionId, discoveredNextSessionIdFloor_);
  rebuilt.nextSegmentSequence = maximum64(
      persisted.nextSegmentSequence,
      discoveredNextSegmentSequenceFloor_);

  rebuilt.nextWritePhysicalSegment = 0U;
  rebuilt.nextWriteSlot = 0U;
  if (segmentSummaryCount_ > 0U) {
    const StorageSegmentSummary& newestSegment =
        segmentSummaries_[segmentSummaryCount_ - 1U];
    uint16_t firstUnusedSlot = 0U;
    while (firstUnusedSlot < SESSION_STORAGE_RECORDS_PER_SEGMENT &&
           (newestSegment.validRecordMask &
            (1U << firstUnusedSlot)) != 0U) {
      ++firstUnusedSlot;
    }
    if (firstUnusedSlot < SESSION_STORAGE_RECORDS_PER_SEGMENT) {
      rebuilt.nextWritePhysicalSegment =
          newestSegment.physicalSegmentIndex;
      rebuilt.nextWriteSlot = firstUnusedSlot;
    } else {
      rebuilt.nextWritePhysicalSegment =
          SessionStorageFormats::nextPhysicalSegment(
              newestSegment.physicalSegmentIndex);
      rebuilt.nextWriteSlot = 0U;
    }
  }

  const bool persistedFinalized =
      persisted.state == PersistentSessionState::Finalized &&
      (persisted.flags & SessionMetadataFinalized) != 0U &&
      (persisted.flags & (SessionMetadataInterruptedRecovered |
                          SessionMetadataCounterPartial |
                          SessionMetadataCorruptionOrGap)) == 0U;
  bool finalizedConsistent = persistedFinalized && !gapOrCorruption &&
      persisted.totalStoredRecords == rebuilt.totalStoredRecords &&
      persisted.retainedRecordCount == rebuilt.retainedRecordCount &&
      persisted.overwrittenRecordCount == rebuilt.overwrittenRecordCount &&
      persisted.firstRetainedLogicalIndex ==
          rebuilt.firstRetainedLogicalIndex &&
      persisted.lastRetainedLogicalIndex ==
          rebuilt.lastRetainedLogicalIndex &&
      persisted.oldestPhysicalSegment ==
          rebuilt.oldestPhysicalSegment &&
      persisted.oldestPhysicalSlot == rebuilt.oldestPhysicalSlot &&
      persisted.nextWritePhysicalSegment ==
           rebuilt.nextWritePhysicalSegment &&
      persisted.nextWriteSlot == rebuilt.nextWriteSlot &&
      persisted.firstStm32Sequence == rebuilt.firstStm32Sequence &&
      persisted.lastStm32Sequence == rebuilt.lastStm32Sequence;
  if (suffix.count == 0U && persistedFinalized &&
      persisted.totalStoredRecords == 0U &&
      persisted.retainedRecordCount == 0U) {
    finalizedConsistent = !gapOrCorruption;
  }

  if (finalizedConsistent) {
    portENTER_CRITICAL(&mux_);
    metadata_ = persisted;
    currentSegmentOpen_ = false;
    status_.prepared = false;
    status_.recoveryBlocked = false;
    status_.recoveryPerformed = true;
    applyMetadataToStatusLocked();
    portEXIT_CRITICAL(&mux_);
    return SessionStorageError::None;
  }

  rebuilt.state = PersistentSessionState::RecoveredIncomplete;
  rebuilt.flags |= SessionMetadataInterruptedRecovered |
                   SessionMetadataCounterPartial;
  if (gapOrCorruption || persistedFinalized) {
    rebuilt.flags |= SessionMetadataCorruptionOrGap;
  }
  rebuilt.flags &= ~SessionMetadataFinalized;

  const bool recoveredStateUnchanged =
      persisted.state == PersistentSessionState::RecoveredIncomplete &&
      metadataPayloadEqual(persisted, rebuilt);
  if (commitRecoveryState && !recoveredStateUnchanged) {
    const SessionStorageError commitResult = commitMetadata(rebuilt);
    if (commitResult != SessionStorageError::None) {
      return commitResult;
    }
  } else {
    portENTER_CRITICAL(&mux_);
    metadata_ = recoveredStateUnchanged ? persisted : rebuilt;
    applyMetadataToStatusLocked();
    portEXIT_CRITICAL(&mux_);
  }

  portENTER_CRITICAL(&mux_);
  currentSegmentOpen_ = false;
  status_.prepared = false;
  status_.recoveryBlocked = false;
  status_.recoveryPerformed = true;
  status_.corruptionOrGap =
      (metadata_.flags & SessionMetadataCorruptionOrGap) != 0U;
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::openCurrentSegment(
    uint64_t firstLogicalRecordIndex,
    uint64_t creationUptimeUs,
    uint32_t bootId) {
  portENTER_CRITICAL(&mux_);
  const uint16_t physicalSegment = metadata_.nextWritePhysicalSegment;
  const uint16_t slot = metadata_.nextWriteSlot;
  const uint64_t segmentSequence = metadata_.nextSegmentSequence;
  const uint64_t sessionId = metadata_.sessionId;
  const uint64_t totalStoredRecords = metadata_.totalStoredRecords;
  const bool dataAreaPrepared = dataAreaPrepared_;
  portEXIT_CRITICAL(&mux_);
  if (physicalSegment >= SESSION_STORAGE_DATA_SEGMENTS || slot != 0U ||
      segmentSequence == 0U || segmentSequence == UINT64_MAX) {
    pauseAfterFailure(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }
  if (!dataAreaPrepared) {
    setError(SessionStorageError::StoragePreparationRequired);
    return SessionStorageError::StoragePreparationRequired;
  }
  if (totalStoredRecords >= SESSION_STORAGE_MAX_RECORDS ||
      findSummaryByPhysical(physicalSegment) >= 0) {
    portENTER_CRITICAL(&mux_);
    status_.storageCapacityReached = true;
    status_.lastError = SessionStorageError::StorageCapacityReached;
    portEXIT_CRITICAL(&mux_);
    return SessionStorageError::StorageCapacityReached;
  }

  // Every data sector was erased before packet admission. Opening a segment
  // while Active therefore programs only its header. Reaching a physical
  // segment already owned by this session is an explicit capacity stop; no
  // circular erase is allowed while acquisition or draining is in progress.
  const uint32_t segmentBaseOffset = segmentOffset(physicalSegment);
  if (!dataRangeValid(segmentBaseOffset, SESSION_STORAGE_SEGMENT_BYTES) ||
      segmentBaseOffset % SESSION_STORAGE_SEGMENT_BYTES != 0U) {
    pauseAfterFailure(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }

  SessionSegmentHeader header{};
  header.segmentSequence = segmentSequence;
  header.sessionId = sessionId;
  header.firstLogicalRecordIndex = firstLogicalRecordIndex;
  header.creationUptimeUs = creationUptimeUs;
  header.bootId = bootId;
  header.physicalSegmentIndex = physicalSegment;
  alignas(4) uint8_t encoded[SESSION_SEGMENT_HEADER_BYTES]{};
  alignas(4) uint8_t readback[SESSION_SEGMENT_HEADER_BYTES]{};
  const SessionStorageFormatError encodeResult =
      SessionStorageFormats::encodeSegmentHeader(
          encoded, sizeof(encoded), header);
  if (encodeResult != SessionStorageFormatError::Ok) {
    portENTER_CRITICAL(&mux_);
    ++status_.segmentValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::SegmentHeaderEncodeFailed,
                      ESP_OK,
                      FlashRecordCodecError::Ok,
                      encodeResult);
    return SessionStorageError::SegmentHeaderEncodeFailed;
  }

  esp_err_t espResult = writePartitionRange(
      segmentBaseOffset, encoded, SESSION_SEGMENT_HEADER_COMMIT_OFFSET);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashWriteFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::FlashWriteFailed, espResult);
    return SessionStorageError::FlashWriteFailed;
  }
  vTaskDelay(ACQUISITION_FLASH_SERVICE_YIELD_TICKS);
  espResult = writePartitionRange(
      segmentBaseOffset + SESSION_SEGMENT_HEADER_COMMIT_OFFSET,
      encoded + SESSION_SEGMENT_HEADER_COMMIT_OFFSET,
      4U);
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashWriteFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::CommitWriteFailed, espResult);
    return SessionStorageError::CommitWriteFailed;
  }
  espResult = esp_partition_read(
      partition_, segmentBaseOffset, readback, sizeof(readback));
  if (espResult != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++status_.flashReadFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::FlashReadFailed, espResult);
    return SessionStorageError::FlashReadFailed;
  }
  const SessionStorageFormatError validation =
      SessionStorageFormats::validateSegmentHeader(
          readback, sizeof(readback), physicalSegment);
  if (validation != SessionStorageFormatError::Ok ||
      memcmp(encoded, readback, sizeof(encoded)) != 0) {
    portENTER_CRITICAL(&mux_);
    ++status_.segmentValidationFailureCount;
    portEXIT_CRITICAL(&mux_);
    pauseAfterFailure(SessionStorageError::SegmentHeaderValidationFailed,
                      ESP_OK,
                      FlashRecordCodecError::Ok,
                      validation);
    return SessionStorageError::SegmentHeaderValidationFailed;
  }

  StorageSegmentSummary summary{};
  summary.valid = true;
  summary.physicalSegmentIndex = physicalSegment;
  summary.segmentSequence = segmentSequence;
  summary.sessionId = sessionId;
  summary.firstLogicalRecordIndex = firstLogicalRecordIndex;
  segmentBootIds_[physicalSegment] = bootId;
  if (!addSummary(summary)) {
    pauseAfterFailure(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }

  portENTER_CRITICAL(&mux_);
  currentSegmentOpen_ = true;
  metadata_.nextSegmentSequence = segmentSequence + 1U;
  status_.validSegmentCount = segmentSummaryCount_;
  applyMetadataToStatusLocked();
  portEXIT_CRITICAL(&mux_);
  return SessionStorageError::None;
}

SessionStorageError SessionStorage::readValidatedRecord(
    uint16_t physicalSegment,
    uint16_t slot,
    uint8_t* destination,
    FlashRecordMetadata& metadata,
    bool countFailure) {
  if (destination == nullptr) {
    setError(SessionStorageError::NullArgument);
    return SessionStorageError::NullArgument;
  }
  const uint32_t offset = recordOffset(physicalSegment, slot);
  if (!recordRangeValid(physicalSegment, slot, offset)) {
    setError(SessionStorageError::GeometryOutOfBounds);
    return SessionStorageError::GeometryOutOfBounds;
  }
  const esp_err_t readResult = esp_partition_read(
      partition_, offset, destination, FLASH_RECORD_BYTES);
  if (readResult != ESP_OK) {
    if (countFailure) {
      portENTER_CRITICAL(&mux_);
      ++status_.flashReadFailureCount;
      portEXIT_CRITICAL(&mux_);
    }
    setError(SessionStorageError::FlashReadFailed, readResult);
    return SessionStorageError::FlashReadFailed;
  }
  const FlashRecordCodecError validation =
      FlashRecordCodec::validate(destination, FLASH_RECORD_BYTES);
  if (validation != FlashRecordCodecError::Ok) {
    if (countFailure) {
      portENTER_CRITICAL(&mux_);
      ++status_.codecValidationFailureCount;
      ++status_.recordValidationFailureCount;
      portEXIT_CRITICAL(&mux_);
    }
    setError(SessionStorageError::ReadbackValidationFailed,
             ESP_OK,
             validation);
    return SessionStorageError::ReadbackValidationFailed;
  }
  const FlashRecordCodecError decode = FlashRecordCodec::decodeMetadata(
      destination, FLASH_RECORD_BYTES, metadata);
  if (decode != FlashRecordCodecError::Ok) {
    if (countFailure) {
      portENTER_CRITICAL(&mux_);
      ++status_.codecValidationFailureCount;
      ++status_.recordValidationFailureCount;
      portEXIT_CRITICAL(&mux_);
    }
    setError(SessionStorageError::ReadbackValidationFailed,
             ESP_OK,
             decode);
    return SessionStorageError::ReadbackValidationFailed;
  }
  return SessionStorageError::None;
}

int16_t SessionStorage::findSummaryByPhysical(
    uint16_t physicalSegment) const {
  for (uint16_t index = 0U; index < segmentSummaryCount_; ++index) {
    if (segmentSummaries_[index].valid &&
        segmentSummaries_[index].physicalSegmentIndex == physicalSegment) {
      return static_cast<int16_t>(index);
    }
  }
  return -1;
}

void SessionStorage::removeSummaryAt(uint16_t summaryIndex) {
  if (summaryIndex >= segmentSummaryCount_) {
    return;
  }
  for (uint16_t index = summaryIndex;
       index + 1U < segmentSummaryCount_;
       ++index) {
    segmentSummaries_[index] = segmentSummaries_[index + 1U];
  }
  --segmentSummaryCount_;
  segmentSummaries_[segmentSummaryCount_] = StorageSegmentSummary{};
}

bool SessionStorage::addSummary(const StorageSegmentSummary& summary) {
  if (!summary.valid ||
      summary.physicalSegmentIndex >= SESSION_STORAGE_DATA_SEGMENTS ||
      summary.segmentSequence == 0U || summary.sessionId == 0U ||
      segmentSummaryCount_ >= SESSION_STORAGE_DATA_SEGMENTS ||
      findSummaryByPhysical(summary.physicalSegmentIndex) >= 0) {
    return false;
  }
  segmentSummaries_[segmentSummaryCount_++] = summary;
  SessionStorageFormats::sortSegmentSummaries(
      segmentSummaries_, segmentSummaryCount_);
  return true;
}

void SessionStorage::applyMetadataToStatusLocked() {
  status_.metadataAValid = copyAValid_;
  status_.metadataBValid = copyBValid_;
  status_.selectedMetadataCopy = selectedMetadataCopy_;
  status_.selectedMetadataGeneration = metadata_.generation;
  status_.sourceMetadataGeneration =
      metadata_.sourceMetadataGeneration != 0U
          ? metadata_.sourceMetadataGeneration
          : metadata_.generation;
  status_.persistentSessionState = metadata_.state;
  status_.synchronizationState = metadata_.synchronizationState;
  status_.sessionId = metadata_.sessionId;
  status_.selectedIntervalSeconds =
      metadata_.selectedFlushIntervalSeconds;
  status_.bootId = metadata_.bootId;
  status_.startUptimeUs = metadata_.startUptimeUs;
  status_.endUptimeUs = metadata_.endUptimeUs;
  status_.startWallClockUnixMs = metadata_.startWallClockUnixMs;
  status_.endWallClockUnixMs = metadata_.endWallClockUnixMs;
  status_.startWallClockValid =
      (metadata_.flags & SessionMetadataStartWallTimeValid) != 0U;
  status_.endWallClockValid =
      (metadata_.flags & SessionMetadataEndWallTimeValid) != 0U;
  status_.sessionStartLogicalIndex = metadata_.sessionStartLogicalIndex;
  status_.firstRetainedLogicalIndex =
      metadata_.firstRetainedLogicalIndex;
  status_.lastRetainedLogicalIndex = metadata_.lastRetainedLogicalIndex;
  status_.nextLogicalRecordIndex = metadata_.nextGlobalLogicalIndex;
  status_.firstStm32Sequence = metadata_.firstStm32Sequence;
  status_.lastStm32Sequence = metadata_.lastStm32Sequence;
  status_.storedRecordCount = metadata_.totalStoredRecords;
  status_.totalStoredRecords = metadata_.totalStoredRecords;
  status_.retainedRecordCount = metadata_.retainedRecordCount;
  status_.overwrittenRecordCount = metadata_.overwrittenRecordCount;
  status_.droppedRecordCount = metadata_.droppedRecordCount;
  status_.currentDataSegment = metadata_.nextWritePhysicalSegment;
  status_.currentSlotInSegment = metadata_.nextWriteSlot;
  status_.oldestPhysicalSegment = metadata_.oldestPhysicalSegment;
  status_.oldestPhysicalSlot = metadata_.oldestPhysicalSlot;
  status_.nextSegmentSequence = metadata_.nextSegmentSequence;
  status_.storageTruncated =
      (metadata_.flags & SessionMetadataStorageTruncated) != 0U;
  status_.storageCapacityReached =
      metadata_.retainedRecordCount >= SESSION_STORAGE_MAX_RECORDS &&
      !status_.storageTruncated;
  status_.recoveredInterrupted =
      (metadata_.flags & SessionMetadataInterruptedRecovered) != 0U;
  status_.countersPartial =
      (metadata_.flags & SessionMetadataCounterPartial) != 0U;
  status_.corruptionOrGap =
      (metadata_.flags & SessionMetadataCorruptionOrGap) != 0U;
  status_.finalized =
      metadata_.state == PersistentSessionState::Finalized &&
      (metadata_.flags & SessionMetadataFinalized) != 0U;
  status_.stopDrainComplete = status_.finalized;
  status_.readerOpen = readerOpen_;
  status_.validSegmentCount = segmentSummaryCount_;
  status_.maximumRecords = SESSION_STORAGE_MAX_RECORDS;
  if (metadata_.nextWritePhysicalSegment <
          SESSION_STORAGE_DATA_SEGMENTS &&
      metadata_.nextWriteSlot < SESSION_STORAGE_RECORDS_PER_SEGMENT) {
    status_.nextPartitionRelativeWriteOffset = recordOffset(
        metadata_.nextWritePhysicalSegment, metadata_.nextWriteSlot);
  } else {
    status_.nextPartitionRelativeWriteOffset = 0U;
  }
}

void SessionStorage::setError(
    SessionStorageError error,
    int32_t espError,
    FlashRecordCodecError codecError,
    SessionStorageFormatError formatError) {
  portENTER_CRITICAL(&mux_);
  status_.lastError = error;
  status_.lastEspError = espError;
  status_.lastCodecError = codecError;
  status_.lastFormatError = formatError;
  portEXIT_CRITICAL(&mux_);
  if (error != SessionStorageError::None &&
      error != SessionStorageError::ReaderEnd) {
    Serial.print("Session storage error: ");
    Serial.println(static_cast<uint32_t>(error));
  }
}

void SessionStorage::pauseAfterFailure(
    SessionStorageError error,
    int32_t espError,
    FlashRecordCodecError codecError,
    SessionStorageFormatError formatError) {
  portENTER_CRITICAL(&mux_);
  status_.prepared = false;
  dataAreaPrepared_ = false;
  status_.dataAreaPrepared = false;
  status_.preparationInProgress = false;
  if (metadata_.state == PersistentSessionState::Active ||
      metadata_.state == PersistentSessionState::Stopping) {
    metadata_.state = PersistentSessionState::ErrorIncomplete;
    metadata_.flags |= SessionMetadataCounterPartial;
    metadata_.flags &= ~SessionMetadataFinalized;
    applyMetadataToStatusLocked();
  }
  portEXIT_CRITICAL(&mux_);
  setError(error, espError, codecError, formatError);
}

void SessionStorage::releaseBuffers() {
  if (recoveryLocations_ != nullptr) {
    heap_caps_free(recoveryLocations_);
    recoveryLocations_ = nullptr;
  }
  if (recoveryRecordBuffer_ != nullptr) {
    heap_caps_free(recoveryRecordBuffer_);
    recoveryRecordBuffer_ = nullptr;
  }
  if (operationMutex_ != nullptr) {
    vSemaphoreDelete(operationMutex_);
    operationMutex_ = nullptr;
  }
}
