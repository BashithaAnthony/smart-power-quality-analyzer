#include "SessionSyncUploader.h"

#include "TestConsoleConfig.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FirebaseBridge.h"
#include "FirebaseConfig.h"
#include "SessionLogger.h"

namespace {

constexpr uint32_t kMaximumRetriesPerRequest = 8U;
constexpr uint32_t kResponseBufferBytes = SESSION_SYNC_BASE64_BUFFER_BYTES;
constexpr uint32_t kPathBufferBytes = 160U;
#if SESSION_SYNC_TEST_CONSOLE
constexpr size_t kFailureDiagnosticBodyChars = 512U;
#endif

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

#if SESSION_SYNC_TEST_CONSOLE
const char* syncStateName(SessionSyncState state) {
  switch (state) {
    case SessionSyncState::Disabled: return "Disabled";
    case SessionSyncState::Idle: return "Idle";
    case SessionSyncState::Requested: return "Requested";
    case SessionSyncState::ReadingManifest: return "ReadingManifest";
    case SessionSyncState::CreatingManifest: return "CreatingManifest";
    case SessionSyncState::CreatingIndex: return "CreatingIndex";
    case SessionSyncState::PersistingUploading: return "PersistingUploading";
    case SessionSyncState::OpeningReader: return "OpeningReader";
    case SessionSyncState::SeekingReader: return "SeekingReader";
    case SessionSyncState::BuildingChunk: return "BuildingChunk";
    case SessionSyncState::UploadingChunk: return "UploadingChunk";
    case SessionSyncState::VerifyingChunk: return "VerifyingChunk";
    case SessionSyncState::UpdatingProgress: return "UpdatingProgress";
    case SessionSyncState::ReadingCompletionManifest:
      return "ReadingCompletionManifest";
    case SessionSyncState::CommittingCompletion:
      return "CommittingCompletion";
    case SessionSyncState::VerifyingCompletion:
      return "VerifyingCompletion";
    case SessionSyncState::UpdatingIndex: return "UpdatingIndex";
    case SessionSyncState::VerifyingIndex: return "VerifyingIndex";
    case SessionSyncState::PersistingSynced: return "PersistingSynced";
    case SessionSyncState::RetryWaiting: return "RetryWaiting";
    case SessionSyncState::Cancelling: return "Cancelling";
    case SessionSyncState::Failing: return "Failing";
    case SessionSyncState::Cancelled: return "Cancelled";
    case SessionSyncState::Complete: return "Complete";
    case SessionSyncState::Error: return "Error";
    default: return "Unknown";
  }
}

const char* databaseMethodName(FirebaseDatabaseMethod method) {
  switch (method) {
    case FirebaseDatabaseMethod::Get: return "GET";
    case FirebaseDatabaseMethod::Put: return "PUT";
    case FirebaseDatabaseMethod::Patch: return "PATCH";
    default: return "UNKNOWN";
  }
}

void printFailedHistoricalRequest(
    SessionSyncState state,
    FirebaseDatabaseMethod method,
    const char* path,
    size_t requestLength,
    const FirebaseDatabaseRequestOptions& options,
    const FirebaseDatabaseResponse& response,
    const char* responseBody) {
  Serial.println("Session sync REST failure:");
  Serial.print("  state: ");
  Serial.println(syncStateName(state));
  Serial.print("  method: ");
  Serial.println(databaseMethodName(method));
  Serial.print("  path: ");
  Serial.print(path != nullptr ? path : "<invalid>");
  Serial.println(".json");
  Serial.print("  HTTP status: ");
  Serial.println(response.httpStatus);
  Serial.print("  Content-Length sent: ");
  Serial.println(static_cast<unsigned long>(requestLength));
  Serial.print("  print=silent: ");
  Serial.println(options.printSilent ? "yes" : "no");
  Serial.print("  X-Firebase-ETag: ");
  Serial.println(options.requestEtag ? "yes" : "no");
  Serial.print("  If-Match: ");
  Serial.println(options.ifMatch != nullptr && options.ifMatch[0] != '\0'
                     ? "yes"
                     : "no");
  Serial.print("  response body: ");
  if (responseBody == nullptr || response.bodyLength == 0U) {
    Serial.println("<empty>");
    return;
  }
  const size_t characters = min(
      response.bodyLength, kFailureDiagnosticBodyChars);
  Serial.write(
      reinterpret_cast<const uint8_t*>(responseBody), characters);
  if (response.bodyLength > characters || response.bodyTruncated) {
    Serial.print("...[truncated]");
  }
  Serial.println();
}
#endif

const char* persistentStateName(SyncManifestPersistentState state) {
  switch (state) {
    case SyncManifestPersistentState::Finalized:
      return "Finalized";
    case SyncManifestPersistentState::RecoveredIncomplete:
      return "RecoveredIncomplete";
    default:
      return nullptr;
  }
}

bool loggerStateBusy(SessionLoggerState state) {
  return state == SessionLoggerState::Starting ||
         state == SessionLoggerState::PreparingStorage ||
         state == SessionLoggerState::Active ||
         state == SessionLoggerState::Stopping ||
         state == SessionLoggerState::Finalizing ||
         state == SessionLoggerState::Clearing ||
         state == SessionLoggerState::Rescanning;
}

bool safeDeviceId(const char* deviceId) {
  if (deviceId == nullptr) {
    return false;
  }
  const size_t length = strlen(deviceId);
  if (length == 0U || length >= SESSION_SYNC_DEVICE_ID_BYTES) {
    return false;
  }
  for (size_t index = 0U; index < length; ++index) {
    const char value = deviceId[index];
    if (!((value >= 'A' && value <= 'Z') ||
          (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '-' || value == '_')) {
      return false;
    }
  }
  return true;
}

class JsonBufferWriter {
 public:
  JsonBufferWriter(char* destination, size_t capacity)
      : destination_(destination), capacity_(capacity) {
    if (destination_ != nullptr && capacity_ > 0U) {
      destination_[0] = '\0';
    }
  }

  bool append(const char* text) {
    return text != nullptr && appendBytes(text, strlen(text));
  }

  bool appendBytes(const char* bytes, size_t length) {
    if (!valid_ || bytes == nullptr || destination_ == nullptr ||
        length >= capacity_ - length_) {
      valid_ = false;
      return false;
    }
    memcpy(destination_ + length_, bytes, length);
    length_ += length;
    destination_[length_] = '\0';
    return true;
  }

  bool appendFormat(const char* format, ...) {
    if (!valid_ || format == nullptr || destination_ == nullptr ||
        length_ >= capacity_) {
      valid_ = false;
      return false;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(
        destination_ + length_, capacity_ - length_, format, arguments);
    va_end(arguments);
    if (written < 0 || static_cast<size_t>(written) >= capacity_ - length_) {
      valid_ = false;
      return false;
    }
    length_ += static_cast<size_t>(written);
    return true;
  }

  bool valid() const { return valid_; }
  size_t length() const { return length_; }

 private:
  char* destination_ = nullptr;
  size_t capacity_ = 0U;
  size_t length_ = 0U;
  bool valid_ = true;
};

bool formatDecimal(uint64_t value, char (&destination)[21]) {
  uint32_t written = 0U;
  return SessionSyncProtocol::formatUint64Decimal(
             value, destination, sizeof(destination), written) ==
             SessionSyncProtocolError::Ok &&
         written > 0U;
}

bool parseDecimalVariant(JsonVariantConst value, uint64_t& result) {
  if (!value.is<const char*>()) {
    return false;
  }
  const char* text = value.as<const char*>();
  return text != nullptr &&
         SessionSyncProtocol::parseUint64Decimal(
             text, static_cast<uint32_t>(strlen(text)), result) ==
             SessionSyncProtocolError::Ok;
}

bool parseUint32Variant(JsonVariantConst value, uint32_t& result) {
  if (!value.is<uint32_t>()) {
    return false;
  }
  result = value.as<uint32_t>();
  return true;
}

bool parseBoundedCountVariant(JsonVariantConst value, uint64_t& result) {
  uint32_t parsed = 0U;
  if (!parseUint32Variant(value, parsed)) {
    return false;
  }
  result = parsed;
  return true;
}

bool parseBoolVariant(JsonVariantConst value, bool& result) {
  if (!value.is<bool>()) {
    return false;
  }
  result = value.as<bool>();
  return true;
}

bool stringEquals(JsonVariantConst value, const char* expected) {
  if (!value.is<const char*>() || expected == nullptr) {
    return false;
  }
  const char* actual = value.as<const char*>();
  return actual != nullptr && strcmp(actual, expected) == 0;
}

bool parseTimestamp(JsonVariantConst value, uint64_t& timestampMs) {
  if (!value.is<uint64_t>()) {
    return false;
  }
  timestampMs = value.as<uint64_t>();
  return timestampMs > 0U;
}

}  // namespace

SessionSyncUploader::~SessionSyncUploader() {
  closeReader();
  releaseBuffers();
}

void SessionSyncUploader::attach(
    SessionStorage& storage, SessionLogger& logger, bool enabled) {
  storage_ = &storage;
  logger_ = &logger;
  portENTER_CRITICAL(&mux_);
  status_ = SessionSyncStatus{};
  status_.state = enabled ? SessionSyncState::Idle
                          : SessionSyncState::Disabled;
  status_.lastError = enabled ? SessionSyncError::None
                              : SessionSyncError::Disabled;
  status_.localSynchronizationState =
      SessionSynchronizationState::NotSynced;
  portEXIT_CRITICAL(&mux_);
  refreshLocalSynchronizationState();
}

bool SessionSyncUploader::requestUpload() {
  portENTER_CRITICAL(&mux_);
  const bool disabled = status_.state == SessionSyncState::Disabled;
  const bool idle = status_.state == SessionSyncState::Idle ||
                    status_.state == SessionSyncState::Cancelled ||
                    status_.state == SessionSyncState::Complete ||
                    status_.state == SessionSyncState::Error;
  if (!idle || storage_ == nullptr || logger_ == nullptr) {
    status_.lastError = disabled ? SessionSyncError::Disabled
                        : idle ? SessionSyncError::FirebaseUnavailable
                               : SessionSyncError::AlreadyInProgress;
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  cancellationRequested_ = false;
  status_.cancellationRequested = false;
  status_.lastError = SessionSyncError::None;
  status_.lastHttpStatus = 0;
  status_.retryCount = 0U;
  status_.state = SessionSyncState::Requested;
  portEXIT_CRITICAL(&mux_);
  return true;
}

void SessionSyncUploader::requestCancellation() {
  portENTER_CRITICAL(&mux_);
  if (status_.state != SessionSyncState::Idle &&
      status_.state != SessionSyncState::Complete &&
      status_.state != SessionSyncState::Cancelled &&
      status_.state != SessionSyncState::Error) {
    cancellationRequested_ = true;
    status_.cancellationRequested = true;
  }
  portEXIT_CRITICAL(&mux_);
}

SessionSyncStatus SessionSyncUploader::getStatus() const {
  portENTER_CRITICAL(&mux_);
  const SessionSyncStatus snapshot = status_;
  portEXIT_CRITICAL(&mux_);
  return snapshot;
}

void SessionSyncUploader::recordRequestRejection(SessionSyncError error) {
  portENTER_CRITICAL(&mux_);
  status_.lastError = error;
  portEXIT_CRITICAL(&mux_);
}

bool SessionSyncUploader::hasPendingWork() const {
  portENTER_CRITICAL(&mux_);
  const SessionSyncState state = status_.state;
  const bool pending = state != SessionSyncState::Disabled &&
                       state != SessionSyncState::Idle &&
                       state != SessionSyncState::Cancelled &&
                       state != SessionSyncState::Complete &&
                       state != SessionSyncState::Error;
  portEXIT_CRITICAL(&mux_);
  return pending;
}

uint32_t SessionSyncUploader::nextServiceDelayMs(uint32_t nowMs) const {
  portENTER_CRITICAL(&mux_);
  const SessionSyncState state = status_.state;
  const bool cancellation = cancellationRequested_;
  const uint32_t deadlineMs = retryDeadlineMs_;
  portEXIT_CRITICAL(&mux_);
  if (cancellation || state != SessionSyncState::RetryWaiting) {
    return 0U;
  }
  return deadlineReached(nowMs, deadlineMs) ? 0U : deadlineMs - nowMs;
}

void SessionSyncUploader::setState(SessionSyncState state) {
  portENTER_CRITICAL(&mux_);
  status_.state = state;
  portEXIT_CRITICAL(&mux_);
}

void SessionSyncUploader::refreshLocalSynchronizationState() {
  if (storage_ == nullptr) {
    return;
  }
  const RetainedSessionInfo current = storage_->getRetainedSessionInfo();
  portENTER_CRITICAL(&mux_);
  status_.localSynchronizationState = current.synchronizationState;
  portEXIT_CRITICAL(&mux_);
}

bool SessionSyncUploader::allocateBuffers() {
  if (rawChunkBuffer_ != nullptr && base64Buffer_ != nullptr &&
      jsonBuffer_ != nullptr) {
    return true;
  }
  if (!psramFound()) {
    return false;
  }
  releaseBuffers();
  rawChunkBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(
      SESSION_SYNC_MAX_RAW_CHUNK_BYTES,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  base64Buffer_ = static_cast<char*>(heap_caps_malloc(
      SESSION_SYNC_BASE64_BUFFER_BYTES,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  jsonBuffer_ = static_cast<char*>(heap_caps_malloc(
      SESSION_SYNC_JSON_BUFFER_BYTES,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (rawChunkBuffer_ == nullptr || base64Buffer_ == nullptr ||
      jsonBuffer_ == nullptr) {
    releaseBuffers();
    return false;
  }
  rawChunkBuffer_[0] = 0U;
  base64Buffer_[0] = '\0';
  jsonBuffer_[0] = '\0';
  portENTER_CRITICAL(&mux_);
  status_.psramBuffersAllocated = true;
  portEXIT_CRITICAL(&mux_);
  return true;
}

void SessionSyncUploader::releaseBuffers() {
  if (rawChunkBuffer_ != nullptr) {
    heap_caps_free(rawChunkBuffer_);
    rawChunkBuffer_ = nullptr;
  }
  if (base64Buffer_ != nullptr) {
    heap_caps_free(base64Buffer_);
    base64Buffer_ = nullptr;
  }
  if (jsonBuffer_ != nullptr) {
    heap_caps_free(jsonBuffer_);
    jsonBuffer_ = nullptr;
  }
  portENTER_CRITICAL(&mux_);
  status_.psramBuffersAllocated = false;
  portEXIT_CRITICAL(&mux_);
}

void SessionSyncUploader::closeReader() {
  if (storage_ != nullptr && reader_.open) {
    storage_->closeReader(reader_);
  }
  reader_ = SessionStorageReader{};
  portENTER_CRITICAL(&mux_);
  status_.readerOpen = false;
  portEXIT_CRITICAL(&mux_);
}

bool SessionSyncUploader::initializeRequest() {
  if (storage_ == nullptr || logger_ == nullptr) {
    beginFailure(SessionSyncError::FirebaseUnavailable);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    beginFailure(SessionSyncError::WifiUnavailable);
    return false;
  }
  const SessionLoggerStatus loggerStatus = logger_->getStatus();
  if (loggerStateBusy(loggerStatus.state) ||
      (loggerStatus.state != SessionLoggerState::Finalized &&
       loggerStatus.state != SessionLoggerState::RecoveredIncomplete)) {
    beginFailure(SessionSyncError::LoggerBusy);
    return false;
  }
  const SessionStorageStatus storageStatus = storage_->getStatus();
  if (!storageStatus.available) {
    beginFailure(SessionSyncError::StorageUnavailable);
    return false;
  }
  if (storageStatus.recoveryBlocked) {
    beginFailure(SessionSyncError::RecoveryBlocked);
    return false;
  }
  if (storageStatus.readerOpen) {
    beginFailure(SessionSyncError::ReaderAlreadyOpen);
    return false;
  }

  // Acquire the reader lease before taking the retained-session snapshot.
  // This prevents clear/re-scan/start operations from changing the session
  // between eligibility validation and the first cloud request.
  const SessionStorageError readerResult =
      storage_->openChronologicalReader(reader_);
  if (readerResult != SessionStorageError::None) {
    beginFailure(readerResult == SessionStorageError::ReaderAlreadyOpen
                     ? SessionSyncError::ReaderAlreadyOpen
                     : SessionSyncError::LocalStorageError);
    return false;
  }
  portENTER_CRITICAL(&mux_);
  status_.readerOpen = true;
  portEXIT_CRITICAL(&mux_);

  retained_ = storage_->getRetainedSessionInfo();
  if (!retained_.available ||
      (!retained_.finalized && !retained_.recoveredIncomplete)) {
    beginFailure(SessionSyncError::NoRetainedSession);
    return false;
  }
  if (retained_.retainedRecordCount == 0U) {
    beginFailure(SessionSyncError::EmptySession);
    return false;
  }
  if (!allocateBuffers()) {
    beginFailure(psramFound() ? SessionSyncError::AllocationFailed
                              : SessionSyncError::PsramUnavailable);
    return false;
  }
  if (!buildImmutableManifest() ||
      SessionSyncProtocol::formatSessionKey(
          retained_.sessionId,
          sessionKey_,
          sizeof(sessionKey_)) != SessionSyncProtocolError::Ok) {
    beginFailure(SessionSyncError::BufferOverflow);
    return false;
  }
  nextChunk_ = 0U;
  uploadedRecords_ = 0U;
  portENTER_CRITICAL(&mux_);
  memcpy(status_.sessionKey, sessionKey_, sizeof(status_.sessionKey));
  status_.chunkCount = immutable_.chunkCount;
  status_.nextChunk = nextChunk_;
  status_.uploadedRecords = uploadedRecords_;
  status_.retainedRecords = retained_.retainedRecordCount;
  status_.localSynchronizationState = retained_.synchronizationState;
  portEXIT_CRITICAL(&mux_);
  retryIndex_ = 0U;
  authRetryCount_ = 0U;
  resumeRecordsToSkip_ = 0U;
  skipRecordsRemaining_ = 0U;
  uploadStartedAtMs_ = 0U;
  uploadStartedAtValid_ = false;
  completionPreconditionFailed_ = false;
  cloudAlreadyComplete_ = false;
  manifestEtag_[0] = '\0';
  return true;
}

bool SessionSyncUploader::buildImmutableManifest() {
  if (!safeDeviceId(FIREBASE_DEVICE_ID)) {
    return false;
  }
  immutable_ = SyncManifestImmutable{};
  immutable_.schemaVersion = SESSION_SYNC_SCHEMA_VERSION;
  strncpy(
      immutable_.deviceId, FIREBASE_DEVICE_ID, sizeof(immutable_.deviceId) - 1U);
  memcpy(immutable_.recordFormat, "PQR1", sizeof(immutable_.recordFormat));
  immutable_.sessionId = retained_.sessionId;
  immutable_.persistentState = retained_.recoveredIncomplete
      ? SyncManifestPersistentState::RecoveredIncomplete
      : SyncManifestPersistentState::Finalized;
  immutable_.truncated = retained_.storageTruncated;
  immutable_.recoveredIncomplete = retained_.recoveredIncomplete;
  immutable_.countersPartial = retained_.countersPartial;
  immutable_.recordSize = FLASH_RECORD_BYTES;
  immutable_.recordsPerChunk = SESSION_SYNC_RECORDS_PER_CHUNK;
  immutable_.retainedCount = retained_.retainedRecordCount;
  immutable_.totalStored = retained_.totalStoredRecords;
  immutable_.overwrittenCount = retained_.overwrittenRecordCount;
  immutable_.firstLogicalIndex = retained_.firstRetainedLogicalIndex;
  immutable_.lastLogicalIndex = retained_.lastRetainedLogicalIndex;
  immutable_.firstStm32Sequence = retained_.firstStm32Sequence;
  immutable_.lastStm32Sequence = retained_.lastStm32Sequence;
  immutable_.sourceMetadataGeneration =
      retained_.sourceMetadataGeneration;
  immutable_.sessionTimeValid = retained_.startWallClockValid;
  immutable_.sessionEndTimeValid = retained_.endWallClockValid;
  immutable_.timeSource = SyncManifestTimeSource::Ntp;
  immutable_.sessionBootId = retained_.bootId;
  immutable_.sessionStartEpochMs = retained_.startWallClockValid
      ? retained_.startWallClockUnixMs
      : 0U;
  immutable_.sessionStartCaptureTimestampUs = retained_.startUptimeUs;
  immutable_.sessionEndEpochMs = retained_.endWallClockValid
      ? retained_.endWallClockUnixMs
      : 0U;
  if (SessionSyncProtocol::calculateChunkCount(
          immutable_.retainedCount, immutable_.chunkCount) !=
      SessionSyncProtocolError::Ok) {
    return false;
  }
  return SessionSyncProtocol::calculateManifestCrc32c(
             immutable_, manifestCrc32c_) == SessionSyncProtocolError::Ok;
}

bool SessionSyncUploader::parseCloudManifest(
    const char* json,
    size_t jsonLength,
    SyncParsedCloudManifest& manifest,
    bool& absent) const {
  manifest = SyncParsedCloudManifest{};
  absent = false;
  if (json == nullptr || jsonLength == 0U) {
    return false;
  }
  JsonDocument document;
  const DeserializationError jsonError =
      deserializeJson(document, json, jsonLength);
  if (jsonError) {
    return false;
  }
  JsonVariantConst root = document.as<JsonVariantConst>();
  if (root.isNull()) {
    manifest.state = SyncCloudManifestState::Absent;
    absent = true;
    return true;
  }
  if (!root.is<JsonObjectConst>()) {
    return false;
  }
  const JsonObjectConst object = root.as<JsonObjectConst>();
  uint32_t schemaVersion = 0U;
  if (!parseUint32Variant(object["schemaVersion"], schemaVersion)) {
    return false;
  }
  if (schemaVersion != SESSION_SYNC_SCHEMA_VERSION &&
      schemaVersion != SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION) {
    manifest.state = SyncCloudManifestState::Unknown;
    return true;
  }
  if (!object["state"].is<const char*>()) {
    return false;
  }
  const char* state = object["state"].as<const char*>();
  if (state == nullptr) {
    return false;
  }
  if (strcmp(state, "uploading") == 0) {
    manifest.state = SyncCloudManifestState::Uploading;
  } else if (strcmp(state, "complete") == 0) {
    manifest.state = SyncCloudManifestState::Complete;
  } else {
    manifest.state = SyncCloudManifestState::Unknown;
    return true;
  }

  SyncManifestImmutable& immutable = manifest.immutable;
  immutable.schemaVersion = schemaVersion;
  if (!object["deviceId"].is<const char*>()) {
    return false;
  }
  const char* deviceId = object["deviceId"].as<const char*>();
  if (deviceId == nullptr || strlen(deviceId) >= sizeof(immutable.deviceId)) {
    return false;
  }
  strncpy(immutable.deviceId, deviceId, sizeof(immutable.deviceId) - 1U);
  memcpy(immutable.recordFormat, "PQR1", sizeof(immutable.recordFormat));
  if (!parseDecimalVariant(object["sessionId"], immutable.sessionId) ||
      !stringEquals(object["recordFormat"], "PQR1") ||
      !parseUint32Variant(object["recordSize"], immutable.recordSize) ||
      !parseUint32Variant(
          object["recordsPerChunk"], immutable.recordsPerChunk) ||
      !parseUint32Variant(object["chunkCount"], immutable.chunkCount) ||
      !parseUint32Variant(object["nextChunk"], manifest.nextChunk) ||
      !parseBoundedCountVariant(
          object["uploadedRecords"], manifest.uploadedRecords) ||
      !parseBoundedCountVariant(
          object["retainedCount"], immutable.retainedCount) ||
      !parseDecimalVariant(object["totalStored"], immutable.totalStored) ||
      !parseDecimalVariant(
          object["overwrittenCount"], immutable.overwrittenCount) ||
      !parseDecimalVariant(
          object["firstLogicalIndex"], immutable.firstLogicalIndex) ||
      !parseDecimalVariant(
          object["lastLogicalIndex"], immutable.lastLogicalIndex) ||
      !parseUint32Variant(
          object["firstStm32Sequence"], immutable.firstStm32Sequence) ||
      !parseUint32Variant(
          object["lastStm32Sequence"], immutable.lastStm32Sequence) ||
      !parseBoolVariant(object["truncated"], immutable.truncated) ||
      !parseBoolVariant(
          object["recoveredIncomplete"], immutable.recoveredIncomplete) ||
      !parseBoolVariant(
          object["countersPartial"], immutable.countersPartial) ||
      !parseDecimalVariant(
          object["sourceMetadataGeneration"],
          immutable.sourceMetadataGeneration)) {
    return false;
  }
  if (stringEquals(object["persistentState"], "Finalized")) {
    immutable.persistentState = SyncManifestPersistentState::Finalized;
  } else if (stringEquals(
                 object["persistentState"], "RecoveredIncomplete")) {
    immutable.persistentState =
        SyncManifestPersistentState::RecoveredIncomplete;
  } else {
    return false;
  }
  if (schemaVersion == SESSION_SYNC_SCHEMA_VERSION) {
    if (!parseBoolVariant(
            object["sessionTimeValid"], immutable.sessionTimeValid) ||
        !parseBoolVariant(
            object["sessionEndTimeValid"], immutable.sessionEndTimeValid) ||
        !stringEquals(object["timeSource"], "ntp") ||
        !parseUint32Variant(
            object["sessionBootId"], immutable.sessionBootId) ||
        !parseDecimalVariant(
            object["sessionStartEpochMs"],
            immutable.sessionStartEpochMs) ||
        !parseDecimalVariant(
            object["sessionStartCaptureTimestampUs"],
            immutable.sessionStartCaptureTimestampUs)) {
      return false;
    }
    immutable.timeSource = SyncManifestTimeSource::Ntp;
    if (immutable.sessionEndTimeValid) {
      if (!parseDecimalVariant(
              object["sessionEndEpochMs"], immutable.sessionEndEpochMs)) {
        return false;
      }
    } else if (!object["sessionEndEpochMs"].isNull()) {
      return false;
    }
  } else {
    immutable.timeSource = SyncManifestTimeSource::None;
  }
  if (!object["manifestCrc32c"].is<const char*>()) {
    return false;
  }
  const char* crcText = object["manifestCrc32c"].as<const char*>();
  if (crcText == nullptr ||
      SessionSyncProtocol::parseCrc32cHex(
          crcText,
          static_cast<uint32_t>(strlen(crcText)),
          manifest.manifestCrc32c) != SessionSyncProtocolError::Ok) {
    return false;
  }

  uint64_t startedAt = 0U;
  uint64_t completedAt = 0U;
  manifest.uploadStartedAtValid =
      parseTimestamp(object["uploadStartedAt"], startedAt);
  manifest.uploadCompletedAtValid =
      parseTimestamp(object["uploadCompletedAt"], completedAt);
  // Protocol revisions expose timestamp values as well as validity. Keeping
  // this assignment conditional preserves compatibility while that API lands.
  manifest.uploadStartedAtMs = startedAt;
  manifest.uploadCompletedAtMs = completedAt;
  return true;
}

bool SessionSyncUploader::buildManifestJson(
    bool complete,
    uint32_t nextChunk,
    uint64_t uploadedRecords,
    uint64_t uploadStartedAtMs,
    size_t& outputLength) {
  outputLength = 0U;
  if (jsonBuffer_ == nullptr) {
    return false;
  }
  const char* persistentState =
      persistentStateName(immutable_.persistentState);
  if (persistentState == nullptr ||
      uploadedRecords > UINT32_MAX ||
      immutable_.retainedCount > UINT32_MAX ||
      !SessionSyncProtocol::progressIsValid(
          immutable_.retainedCount, nextChunk, uploadedRecords)) {
    return false;
  }
  char sessionId[21]{};
  char totalStored[21]{};
  char overwritten[21]{};
  char firstLogical[21]{};
  char lastLogical[21]{};
  char sourceGeneration[21]{};
  char sessionStartEpoch[21]{};
  char sessionStartCapture[21]{};
  char sessionEndEpoch[21]{};
  char manifestCrc[9]{};
  if (!formatDecimal(immutable_.sessionId, sessionId) ||
      !formatDecimal(immutable_.totalStored, totalStored) ||
      !formatDecimal(immutable_.overwrittenCount, overwritten) ||
      !formatDecimal(immutable_.firstLogicalIndex, firstLogical) ||
      !formatDecimal(immutable_.lastLogicalIndex, lastLogical) ||
      !formatDecimal(
          immutable_.sourceMetadataGeneration, sourceGeneration) ||
      !formatDecimal(immutable_.sessionStartEpochMs, sessionStartEpoch) ||
      !formatDecimal(immutable_.sessionStartCaptureTimestampUs,
                     sessionStartCapture) ||
      (immutable_.sessionEndTimeValid &&
       !formatDecimal(immutable_.sessionEndEpochMs, sessionEndEpoch)) ||
      SessionSyncProtocol::formatCrc32cHex(
          manifestCrc32c_, manifestCrc, sizeof(manifestCrc)) !=
          SessionSyncProtocolError::Ok) {
    return false;
  }

  JsonBufferWriter writer(jsonBuffer_, SESSION_SYNC_JSON_BUFFER_BYTES);
  writer.appendFormat(
      "{\"schemaVersion\":%u,\"state\":\"%s\","
      "\"deviceId\":\"%s\",\"sessionId\":\"%s\","
      "\"persistentState\":\"%s\",\"recordFormat\":\"PQR1\","
      "\"recordSize\":%u,\"recordsPerChunk\":%u,"
      "\"chunkCount\":%u,\"nextChunk\":%u,"
      "\"uploadedRecords\":%u,\"retainedCount\":%u,"
      "\"totalStored\":\"%s\",\"overwrittenCount\":\"%s\","
      "\"firstLogicalIndex\":\"%s\",\"lastLogicalIndex\":\"%s\","
      "\"firstStm32Sequence\":%u,\"lastStm32Sequence\":%u,"
      "\"truncated\":%s,\"recoveredIncomplete\":%s,"
      "\"countersPartial\":%s,"
      "\"sourceMetadataGeneration\":\"%s\","
      "\"sessionTimeValid\":%s,"
      "\"sessionStartEpochMs\":\"%s\","
      "\"sessionStartCaptureTimestampUs\":\"%s\","
      "\"sessionEndTimeValid\":%s,"
      "\"sessionBootId\":%u,\"timeSource\":\"ntp\"",
      static_cast<unsigned>(immutable_.schemaVersion),
      complete ? "complete" : "uploading",
      immutable_.deviceId,
      sessionId,
      persistentState,
      static_cast<unsigned>(immutable_.recordSize),
      static_cast<unsigned>(immutable_.recordsPerChunk),
      static_cast<unsigned>(immutable_.chunkCount),
      static_cast<unsigned>(nextChunk),
      static_cast<unsigned>(uploadedRecords),
      static_cast<unsigned>(immutable_.retainedCount),
      totalStored,
      overwritten,
      firstLogical,
      lastLogical,
      static_cast<unsigned>(immutable_.firstStm32Sequence),
      static_cast<unsigned>(immutable_.lastStm32Sequence),
      immutable_.truncated ? "true" : "false",
      immutable_.recoveredIncomplete ? "true" : "false",
      immutable_.countersPartial ? "true" : "false",
      sourceGeneration,
      immutable_.sessionTimeValid ? "true" : "false",
      sessionStartEpoch,
      sessionStartCapture,
      immutable_.sessionEndTimeValid ? "true" : "false",
      static_cast<unsigned>(immutable_.sessionBootId));
  if (immutable_.sessionEndTimeValid) {
    writer.appendFormat(",\"sessionEndEpochMs\":\"%s\"", sessionEndEpoch);
  }
  writer.append(",\"uploadStartedAt\":");
  if (uploadStartedAtMs > 0U) {
    char startedAt[21]{};
    if (!formatDecimal(uploadStartedAtMs, startedAt)) {
      return false;
    }
    writer.append(startedAt);
  } else {
    writer.append("{\".sv\":\"timestamp\"}");
  }
  writer.append(",\"uploadCompletedAt\":");
  writer.append(complete ? "{\".sv\":\"timestamp\"}" : "null");
  writer.appendFormat(
      ",\"manifestCrc32c\":\"%s\"}", manifestCrc);
  if (!writer.valid()) {
    return false;
  }
  outputLength = writer.length();
  return true;
}

bool SessionSyncUploader::buildIndexJson(
    bool complete,
    uint64_t uploadStartedAtMs,
    size_t& outputLength) {
  outputLength = 0U;
  if (jsonBuffer_ == nullptr) {
    return false;
  }
  const char* persistentState =
      persistentStateName(immutable_.persistentState);
  char sessionId[21]{};
  char totalStored[21]{};
  char overwritten[21]{};
  char firstLogical[21]{};
  char lastLogical[21]{};
  char sessionStartEpoch[21]{};
  if (persistentState == nullptr ||
      immutable_.retainedCount > UINT32_MAX ||
      !formatDecimal(immutable_.sessionId, sessionId) ||
      !formatDecimal(immutable_.totalStored, totalStored) ||
      !formatDecimal(immutable_.overwrittenCount, overwritten) ||
      !formatDecimal(immutable_.firstLogicalIndex, firstLogical) ||
      !formatDecimal(immutable_.lastLogicalIndex, lastLogical) ||
      (immutable_.sessionTimeValid &&
       !formatDecimal(immutable_.sessionStartEpochMs, sessionStartEpoch))) {
    return false;
  }
  JsonBufferWriter writer(jsonBuffer_, SESSION_SYNC_JSON_BUFFER_BYTES);
  writer.appendFormat(
      "{\"schemaVersion\":%u,\"state\":\"%s\","
      "\"deviceId\":\"%s\",\"sessionId\":\"%s\","
      "\"persistentState\":\"%s\",\"retainedCount\":%u,"
      "\"totalStored\":\"%s\",\"overwrittenCount\":\"%s\","
      "\"firstLogicalIndex\":\"%s\",\"lastLogicalIndex\":\"%s\","
      "\"truncated\":%s,\"recoveredIncomplete\":%s,"
      "\"countersPartial\":%s,\"chunkCount\":%u,"
      "\"sessionTimeValid\":%s",
      static_cast<unsigned>(immutable_.schemaVersion),
      complete ? "complete" : "uploading",
      immutable_.deviceId,
      sessionId,
      persistentState,
      static_cast<unsigned>(immutable_.retainedCount),
      totalStored,
      overwritten,
      firstLogical,
      lastLogical,
      immutable_.truncated ? "true" : "false",
      immutable_.recoveredIncomplete ? "true" : "false",
      immutable_.countersPartial ? "true" : "false",
      static_cast<unsigned>(immutable_.chunkCount),
      immutable_.sessionTimeValid ? "true" : "false");
  if (immutable_.sessionTimeValid) {
    writer.appendFormat(",\"sessionStartEpochMs\":\"%s\"",
                        sessionStartEpoch);
  }
  writer.append(",\"uploadStartedAt\":");
  if (uploadStartedAtMs > 0U) {
    char startedAt[21]{};
    if (!formatDecimal(uploadStartedAtMs, startedAt)) {
      return false;
    }
    writer.append(startedAt);
  } else {
    writer.append("{\".sv\":\"timestamp\"}");
  }
  writer.append(",\"uploadCompletedAt\":");
  writer.append(complete ? "{\".sv\":\"timestamp\"}" : "null");
  writer.append("}");
  if (!writer.valid()) {
    return false;
  }
  outputLength = writer.length();
  return true;
}

bool SessionSyncUploader::buildChunk() {
  SyncChunkBounds bounds{};
  if (rawChunkBuffer_ == nullptr || base64Buffer_ == nullptr ||
      SessionSyncProtocol::calculateChunkBounds(
          immutable_.retainedCount, nextChunk_, bounds) !=
          SessionSyncProtocolError::Ok ||
      bounds.rawBytes > SESSION_SYNC_MAX_RAW_CHUNK_BYTES) {
    beginFailure(SessionSyncError::BufferOverflow);
    return false;
  }
  uint64_t expectedLogicalIndex = immutable_.firstLogicalIndex;
  if (bounds.firstRecordOrdinal > UINT64_MAX - expectedLogicalIndex) {
    beginFailure(SessionSyncError::LocalRecordValidation);
    return false;
  }
  expectedLogicalIndex += bounds.firstRecordOrdinal;
  if (bounds.recordCount == 0U ||
      expectedLogicalIndex > UINT64_MAX - (bounds.recordCount - 1U)) {
    beginFailure(SessionSyncError::LocalRecordValidation);
    return false;
  }
  for (uint32_t recordIndex = 0U;
       recordIndex < bounds.recordCount;
       ++recordIndex) {
    FlashRecordMetadata recordMetadata{};
    const SessionStorageError readResult = storage_->readNextEncodedRecord(
        reader_,
        rawChunkBuffer_ + recordIndex * FLASH_RECORD_BYTES,
        FLASH_RECORD_BYTES,
        recordMetadata);
    if (readResult != SessionStorageError::None ||
        recordMetadata.sessionId != immutable_.sessionId ||
        recordMetadata.logicalRecordIndex !=
            expectedLogicalIndex + recordIndex) {
      beginFailure(SessionSyncError::LocalRecordValidation);
      return false;
    }
  }
  chunkRecordCount_ = bounds.recordCount;
  chunkRawBytes_ = bounds.rawBytes;
  chunkFirstLogicalIndex_ = expectedLogicalIndex;
  chunkLastLogicalIndex_ =
      expectedLogicalIndex + bounds.recordCount - 1U;
  chunkCrc32c_ =
      SessionSyncProtocol::crc32c(rawChunkBuffer_, chunkRawBytes_);
  uint32_t base64Length = 0U;
  if (SessionSyncProtocol::base64Encode(
          rawChunkBuffer_,
          chunkRawBytes_,
          base64Buffer_,
          SESSION_SYNC_BASE64_BUFFER_BYTES,
          base64Length) != SessionSyncProtocolError::Ok ||
      base64Length >= SESSION_SYNC_BASE64_BUFFER_BYTES) {
    beginFailure(SessionSyncError::BufferOverflow);
    return false;
  }
  base64Buffer_[base64Length] = '\0';
  return buildChunkJson(jsonLength_);
}

bool SessionSyncUploader::buildChunkJson(size_t& outputLength) {
  outputLength = 0U;
  if (jsonBuffer_ == nullptr || base64Buffer_ == nullptr) {
    return false;
  }
  char firstLogical[21]{};
  char lastLogical[21]{};
  char crcText[9]{};
  uint32_t base64Length = 0U;
  if (!formatDecimal(chunkFirstLogicalIndex_, firstLogical) ||
      !formatDecimal(chunkLastLogicalIndex_, lastLogical) ||
      SessionSyncProtocol::formatCrc32cHex(
          chunkCrc32c_, crcText, sizeof(crcText)) !=
          SessionSyncProtocolError::Ok ||
      SessionSyncProtocol::base64EncodedLength(
          chunkRawBytes_, base64Length) != SessionSyncProtocolError::Ok ||
      base64Length >= SESSION_SYNC_BASE64_BUFFER_BYTES) {
    return false;
  }
  JsonBufferWriter writer(jsonBuffer_, SESSION_SYNC_JSON_BUFFER_BYTES);
  writer.appendFormat(
      "{\"meta\":{\"schemaVersion\":%u,\"chunkIndex\":%u,"
      "\"firstLogicalIndex\":\"%s\",\"lastLogicalIndex\":\"%s\","
      "\"recordCount\":%u,\"recordSize\":%u,\"rawBytes\":%u,"
      "\"crc32c\":\"%s\",\"encoding\":\"base64\","
      "\"recordFormat\":\"PQR1\"},\"payload\":\"",
      static_cast<unsigned>(SESSION_SYNC_CHUNK_SCHEMA_VERSION),
      static_cast<unsigned>(nextChunk_),
      firstLogical,
      lastLogical,
      static_cast<unsigned>(chunkRecordCount_),
      static_cast<unsigned>(FLASH_RECORD_BYTES),
      static_cast<unsigned>(chunkRawBytes_),
      crcText);
  writer.appendBytes(base64Buffer_, base64Length);
  writer.append("\"}");
  if (!writer.valid()) {
    return false;
  }
  outputLength = writer.length();
  return true;
}

bool SessionSyncUploader::parseAndVerifyChunkMeta(
    const char* json, size_t jsonLength) const {
  if (json == nullptr || jsonLength == 0U) {
    return false;
  }
  JsonDocument document;
  if (deserializeJson(document, json, jsonLength)) {
    return false;
  }
  const JsonObjectConst object = document.as<JsonObjectConst>();
  if (object.isNull()) {
    return false;
  }
  uint32_t schemaVersion = 0U;
  uint32_t chunkIndex = 0U;
  uint32_t recordCount = 0U;
  uint32_t recordSize = 0U;
  uint32_t rawBytes = 0U;
  uint64_t firstLogical = 0U;
  uint64_t lastLogical = 0U;
  uint32_t crc = 0U;
  if (!parseUint32Variant(object["schemaVersion"], schemaVersion) ||
      !parseUint32Variant(object["chunkIndex"], chunkIndex) ||
      !parseDecimalVariant(object["firstLogicalIndex"], firstLogical) ||
      !parseDecimalVariant(object["lastLogicalIndex"], lastLogical) ||
      !parseUint32Variant(object["recordCount"], recordCount) ||
      !parseUint32Variant(object["recordSize"], recordSize) ||
      !parseUint32Variant(object["rawBytes"], rawBytes) ||
      !stringEquals(object["encoding"], "base64") ||
      !stringEquals(object["recordFormat"], "PQR1") ||
      !object["crc32c"].is<const char*>()) {
    return false;
  }
  const char* crcText = object["crc32c"].as<const char*>();
  if (crcText == nullptr ||
      SessionSyncProtocol::parseCrc32cHex(
          crcText, static_cast<uint32_t>(strlen(crcText)), crc) !=
          SessionSyncProtocolError::Ok) {
    return false;
  }
  return schemaVersion == SESSION_SYNC_CHUNK_SCHEMA_VERSION &&
         chunkIndex == nextChunk_ &&
         firstLogical == chunkFirstLogicalIndex_ &&
         lastLogical == chunkLastLogicalIndex_ &&
         recordCount == chunkRecordCount_ &&
         recordSize == FLASH_RECORD_BYTES && rawBytes == chunkRawBytes_ &&
         crc == chunkCrc32c_;
}

bool SessionSyncUploader::parseAndVerifyIndex(
    const char* json, size_t jsonLength) const {
  if (json == nullptr || jsonLength == 0U) {
    return false;
  }
  JsonDocument document;
  if (deserializeJson(document, json, jsonLength)) {
    return false;
  }
  const JsonObjectConst object = document.as<JsonObjectConst>();
  if (object.isNull()) {
    return false;
  }
  uint32_t schemaVersion = 0U;
  uint32_t chunkCount = 0U;
  uint64_t sessionId = 0U;
  uint64_t retained = 0U;
  uint64_t totalStored = 0U;
  uint64_t overwritten = 0U;
  uint64_t firstLogical = 0U;
  uint64_t lastLogical = 0U;
  bool truncated = false;
  bool recovered = false;
  bool countersPartial = false;
  bool sessionTimeValid = false;
  uint64_t sessionStartEpochMs = 0U;
  uint64_t startedAt = 0U;
  uint64_t completedAt = 0U;
  const bool baseMatches =
         parseUint32Variant(object["schemaVersion"], schemaVersion) &&
         schemaVersion == immutable_.schemaVersion &&
         stringEquals(object["state"], "complete") &&
         stringEquals(object["deviceId"], immutable_.deviceId) &&
         parseDecimalVariant(object["sessionId"], sessionId) &&
         sessionId == immutable_.sessionId &&
         stringEquals(
             object["persistentState"],
             persistentStateName(immutable_.persistentState)) &&
         parseBoundedCountVariant(object["retainedCount"], retained) &&
         retained == immutable_.retainedCount &&
         parseDecimalVariant(object["totalStored"], totalStored) &&
         totalStored == immutable_.totalStored &&
         parseDecimalVariant(object["overwrittenCount"], overwritten) &&
         overwritten == immutable_.overwrittenCount &&
         parseDecimalVariant(object["firstLogicalIndex"], firstLogical) &&
         firstLogical == immutable_.firstLogicalIndex &&
         parseDecimalVariant(object["lastLogicalIndex"], lastLogical) &&
         lastLogical == immutable_.lastLogicalIndex &&
         parseBoolVariant(object["truncated"], truncated) &&
         truncated == immutable_.truncated &&
         parseBoolVariant(object["recoveredIncomplete"], recovered) &&
         recovered == immutable_.recoveredIncomplete &&
         parseBoolVariant(object["countersPartial"], countersPartial) &&
         countersPartial == immutable_.countersPartial &&
         parseUint32Variant(object["chunkCount"], chunkCount) &&
         chunkCount == immutable_.chunkCount &&
         parseBoolVariant(object["sessionTimeValid"], sessionTimeValid) &&
         sessionTimeValid == immutable_.sessionTimeValid &&
         parseTimestamp(object["uploadStartedAt"], startedAt) &&
         startedAt == uploadStartedAtMs_ &&
         parseTimestamp(object["uploadCompletedAt"], completedAt);
  if (!baseMatches) {
    return false;
  }
  if (immutable_.sessionTimeValid) {
    return parseDecimalVariant(
               object["sessionStartEpochMs"], sessionStartEpochMs) &&
           sessionStartEpochMs == immutable_.sessionStartEpochMs;
  }
  return object["sessionStartEpochMs"].isNull();
}

bool SessionSyncUploader::buildProgressPatch(size_t& outputLength) {
  outputLength = 0U;
  const uint32_t completedChunk = nextChunk_ + 1U;
  uint64_t uploadedRecords = uploadedRecords_;
  if (chunkRecordCount_ > UINT64_MAX - uploadedRecords) {
    return false;
  }
  uploadedRecords += chunkRecordCount_;
  if (!SessionSyncProtocol::progressIsValid(
          immutable_.retainedCount, completedChunk, uploadedRecords)) {
    return false;
  }
  if (uploadedRecords > UINT32_MAX) {
    return false;
  }
  const int written = snprintf(
      jsonBuffer_,
      SESSION_SYNC_JSON_BUFFER_BYTES,
      "{\"nextChunk\":%u,\"uploadedRecords\":%u}",
      static_cast<unsigned>(completedChunk),
      static_cast<unsigned>(uploadedRecords));
  if (written < 0 ||
      static_cast<size_t>(written) >= SESSION_SYNC_JSON_BUFFER_BYTES) {
    return false;
  }
  outputLength = static_cast<size_t>(written);
  return true;
}

bool SessionSyncUploader::makePath(
    char* destination,
    size_t destinationLength,
    const char* suffix) const {
  if (destination == nullptr || destinationLength == 0U || suffix == nullptr ||
      !safeDeviceId(FIREBASE_DEVICE_ID) || sessionKey_[0] == '\0') {
    return false;
  }
  int written = 0;
  if (strcmp(suffix, "@index") == 0) {
    written = snprintf(
        destination,
        destinationLength,
        "devices/%s/sessionIndex/%s",
        FIREBASE_DEVICE_ID,
        sessionKey_);
  } else {
    written = snprintf(
        destination,
        destinationLength,
        "devices/%s/sessionData/%s/%s",
        FIREBASE_DEVICE_ID,
        sessionKey_,
        suffix);
  }
  return written > 0 && static_cast<size_t>(written) < destinationLength;
}

bool SessionSyncUploader::performRequest(
    FirebaseBridge& bridge,
    FirebaseDatabaseMethod method,
    const char* suffix,
    const uint8_t* body,
    size_t bodyLength,
    const FirebaseDatabaseRequestOptions& options,
    FirebaseDatabaseResponse& response) {
  const bool writeMethod = method == FirebaseDatabaseMethod::Put ||
                           method == FirebaseDatabaseMethod::Patch;
  const bool hasIfMatch =
      options.ifMatch != nullptr && options.ifMatch[0] != '\0';
  if ((options.printSilent && !writeMethod) ||
      (options.requestEtag && method != FirebaseDatabaseMethod::Get) ||
      (hasIfMatch && method != FirebaseDatabaseMethod::Put) ||
      !SessionSyncProtocol::restRequestOptionsAreCompatible(
          options.printSilent,
          hasIfMatch,
          false,
          false,
          false) ||
      (bodyLength > 0U &&
       (body == nullptr || bodyLength >= SESSION_SYNC_JSON_BUFFER_BYTES ||
        body[bodyLength] != '\0'))) {
    beginFailure(SessionSyncError::MalformedRequest);
    return false;
  }
  char path[kPathBufferBytes]{};
  if (!makePath(path, sizeof(path), suffix)) {
    beginFailure(SessionSyncError::BufferOverflow);
    return false;
  }
  const bool requestResult = bridge.performDatabaseRequest(
      method,
      path,
      body,
      bodyLength,
      options,
      base64Buffer_,
      kResponseBufferBytes,
      response);
#if SESSION_SYNC_TEST_CONSOLE
  if (response.httpStatus < 200 || response.httpStatus >= 300) {
    SessionSyncState requestState = SessionSyncState::Error;
    portENTER_CRITICAL(&mux_);
    requestState = status_.state;
    portEXIT_CRITICAL(&mux_);
    printFailedHistoricalRequest(
        requestState,
        method,
        path,
        bodyLength,
        options,
        response,
        base64Buffer_);
  }
#endif
  portENTER_CRITICAL(&mux_);
  status_.lastHttpStatus = response.httpStatus;
  portEXIT_CRITICAL(&mux_);
  return requestResult;
}

void SessionSyncUploader::resetRetry() {
  retryIndex_ = 0U;
  authRetryCount_ = 0U;
  portENTER_CRITICAL(&mux_);
  status_.retryCount = 0U;
  portEXIT_CRITICAL(&mux_);
}

void SessionSyncUploader::scheduleRetry(
    SessionSyncState retryState,
    int httpStatus,
    uint32_t nowMs) {
  if (retryIndex_ >= kMaximumRetriesPerRequest) {
    beginFailure(SessionSyncError::RetryExhausted, httpStatus);
    return;
  }
  const uint32_t delayMs = SessionSyncProtocol::retryDelayMs(retryIndex_);
  ++retryIndex_;
  retryState_ = retryState;
  retryDeadlineMs_ = nowMs + delayMs;
  portENTER_CRITICAL(&mux_);
  status_.retryCount = retryIndex_;
  status_.lastHttpStatus = httpStatus;
  status_.state = SessionSyncState::RetryWaiting;
  portEXIT_CRITICAL(&mux_);
#if SESSION_SYNC_TEST_CONSOLE
  Serial.printf("Session sync retry %u in %u ms (HTTP %d)\n",
                static_cast<unsigned>(retryIndex_),
                static_cast<unsigned>(delayMs),
                httpStatus);
#endif
}

bool SessionSyncUploader::acceptSuccessfulStatus(
    const FirebaseDatabaseResponse& response,
    SessionSyncState retryState,
    uint32_t nowMs,
    bool allowPreconditionFailure) {
  if (response.authenticationFailed) {
    if (SessionSyncProtocol::isRetryableHttpStatus(response.httpStatus)) {
      scheduleRetry(retryState, response.httpStatus, nowMs);
    } else {
      beginFailure(SessionSyncError::AuthenticationFailed,
                   response.httpStatus);
    }
    return false;
  }
  if (response.httpStatus == 200 || response.httpStatus == 204) {
    resetRetry();
    return true;
  }
  if (allowPreconditionFailure && response.httpStatus == 412) {
    resetRetry();
    return false;
  }
  if (response.httpStatus == 401) {
    if (authRetryCount_ == 0U) {
      ++authRetryCount_;
      scheduleRetry(retryState, response.httpStatus, nowMs);
    } else {
      beginFailure(SessionSyncError::AuthenticationFailed,
                   response.httpStatus);
    }
    return false;
  }
  if (response.httpStatus == 403) {
    beginFailure(SessionSyncError::SecurityRulesDenied, response.httpStatus);
    return false;
  }
  if (response.httpStatus == 400) {
    beginFailure(SessionSyncError::MalformedRequest, response.httpStatus);
    return false;
  }
  if (SessionSyncProtocol::isRetryableHttpStatus(response.httpStatus)) {
    scheduleRetry(retryState, response.httpStatus, nowMs);
    return false;
  }
  beginFailure(SessionSyncError::UnexpectedHttpStatus, response.httpStatus);
  return false;
}

void SessionSyncUploader::beginFailure(
    SessionSyncError error, int httpStatus) {
  pendingFailure_ = error;
  portENTER_CRITICAL(&mux_);
  status_.lastError = error;
  if (httpStatus != 0) {
    status_.lastHttpStatus = httpStatus;
  }
  status_.state = SessionSyncState::Failing;
  portEXIT_CRITICAL(&mux_);
}

void SessionSyncUploader::finishFailure() {
  const SessionSyncError error = pendingFailure_;
  closeReader();
  releaseBuffers();
  refreshLocalSynchronizationState();
  portENTER_CRITICAL(&mux_);
  status_.lastError = error;
  status_.state = SessionSyncState::Error;
  status_.cancellationRequested = false;
  cancellationRequested_ = false;
  portEXIT_CRITICAL(&mux_);
  switch (error) {
    case SessionSyncError::CloudConflict:
      Serial.println("Session sync cloud conflict");
      break;
    case SessionSyncError::UnsupportedCloudManifest:
      Serial.println("Session sync unsupported cloud manifest");
      break;
    case SessionSyncError::SecurityRulesDenied:
      Serial.println("Session sync security rules denied");
      break;
    case SessionSyncError::LocalRecordValidation:
      Serial.println("Session sync local record validation error");
      break;
    case SessionSyncError::MalformedRequest:
      Serial.println("Session sync malformed request");
      break;
    default:
      Serial.print("Session sync error: ");
      Serial.println(static_cast<unsigned>(error));
      break;
  }
}

void SessionSyncUploader::cancelNow() {
  closeReader();
  releaseBuffers();
  refreshLocalSynchronizationState();
  portENTER_CRITICAL(&mux_);
  cancellationRequested_ = false;
  status_.cancellationRequested = false;
  status_.lastError = SessionSyncError::Cancelled;
  status_.state = SessionSyncState::Cancelled;
  portEXIT_CRITICAL(&mux_);
#if SESSION_SYNC_TEST_CONSOLE
  Serial.println("Session upload cancelled");
#endif
}

void SessionSyncUploader::printChunkProgressIfEnabled() const {
#if SESSION_SYNC_TEST_CONSOLE
  const uint32_t completedChunk = nextChunk_;
  if (completedChunk == 1U || completedChunk == immutable_.chunkCount ||
      (completedChunk % 10U) == 0U) {
    Serial.printf("Session upload chunk %u verified\n",
                  static_cast<unsigned>(completedChunk - 1U));
  }
#endif
}

bool SessionSyncUploader::service(
    FirebaseBridge& bridge, uint32_t nowMs) {
  portENTER_CRITICAL(&mux_);
  const bool cancellation = cancellationRequested_;
  SessionSyncState state = status_.state;
  portEXIT_CRITICAL(&mux_);

  if (SessionSyncProtocol::evaluateCancellation(cancellation, false) ==
      SyncCancellationDecision::CancelNow) {
    setState(SessionSyncState::Cancelling);
    cancelNow();
    return false;
  }
  if (state == SessionSyncState::RetryWaiting) {
    if (!deadlineReached(nowMs, retryDeadlineMs_)) {
      return false;
    }
    setState(retryState_);
    state = retryState_;
  }

  FirebaseDatabaseResponse response{};
  FirebaseDatabaseRequestOptions options{};
  bool parsedAbsent = false;

  switch (state) {
    case SessionSyncState::Requested:
      if (initializeRequest()) {
        setState(SessionSyncState::ReadingManifest);
      }
      return false;

    case SessionSyncState::ReadingManifest: {
      options.requestEtag = true;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Get,
          "manifest",
          nullptr,
          0U,
          options,
          response);
      if (!acceptSuccessfulStatus(
              response, SessionSyncState::ReadingManifest, nowMs)) {
        return true;
      }
      if (response.bodyTruncated ||
          !parseCloudManifest(
              base64Buffer_, response.bodyLength, cloudManifest_, parsedAbsent)) {
        beginFailure(response.bodyTruncated
                         ? SessionSyncError::ResponseTooLarge
                         : SessionSyncError::InvalidCloudResponse);
        return true;
      }
      const SyncManifestDecision decision =
          SessionSyncProtocol::evaluateCloudManifest(
              immutable_, cloudManifest_);
      if (decision == SyncManifestDecision::Create) {
        if (response.etag[0] == '\0') {
          beginFailure(SessionSyncError::InvalidCloudResponse);
          return true;
        }
        strncpy(manifestEtag_, response.etag, sizeof(manifestEtag_) - 1U);
        setState(SessionSyncState::CreatingManifest);
      } else if (decision == SyncManifestDecision::Resume) {
        uploadStartedAtMs_ = cloudManifest_.uploadStartedAtMs;
        uploadStartedAtValid_ = cloudManifest_.uploadStartedAtValid;
        const uint64_t skip = cloudManifest_.uploadedRecords;
        if (skip > UINT32_MAX || skip > immutable_.retainedCount) {
          beginFailure(SessionSyncError::CloudConflict);
          return true;
        }
        resumeRecordsToSkip_ = static_cast<uint32_t>(skip);
        skipRecordsRemaining_ = resumeRecordsToSkip_;
        nextChunk_ = cloudManifest_.nextChunk;
        uploadedRecords_ = cloudManifest_.uploadedRecords;
        portENTER_CRITICAL(&mux_);
        status_.nextChunk = nextChunk_;
        status_.uploadedRecords = uploadedRecords_;
        portEXIT_CRITICAL(&mux_);
#if SESSION_SYNC_TEST_CONSOLE
        Serial.printf("Session upload resumed at chunk %u\n",
                      static_cast<unsigned>(cloudManifest_.nextChunk));
#endif
        setState(SessionSyncState::PersistingUploading);
      } else if (decision == SyncManifestDecision::AlreadyComplete) {
        uploadStartedAtMs_ = cloudManifest_.uploadStartedAtMs;
        uploadStartedAtValid_ = cloudManifest_.uploadStartedAtValid;
        cloudAlreadyComplete_ = true;
        nextChunk_ = immutable_.chunkCount;
        uploadedRecords_ = immutable_.retainedCount;
        portENTER_CRITICAL(&mux_);
        status_.nextChunk = nextChunk_;
        status_.uploadedRecords = uploadedRecords_;
        portEXIT_CRITICAL(&mux_);
        setState(cloudManifest_.immutable.schemaVersion ==
                         SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION
                     ? SessionSyncState::PersistingSynced
                     : SessionSyncState::UpdatingIndex);
      } else if (decision ==
                 SyncManifestDecision::UnsupportedCloudManifest) {
        beginFailure(SessionSyncError::UnsupportedCloudManifest);
      } else {
        beginFailure(SessionSyncError::CloudConflict);
      }
      return true;
    }

    case SessionSyncState::CreatingManifest:
      if (!buildManifestJson(false, 0U, 0U, 0U, jsonLength_)) {
        beginFailure(SessionSyncError::BufferOverflow);
        return false;
      }
      options.printSilent =
          SessionSyncProtocol::printSilentForRequestSite(
              SyncRestRequestSite::InitialConditionalManifestPut);
      options.ifMatch = manifestEtag_;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Put,
          "manifest",
          reinterpret_cast<const uint8_t*>(jsonBuffer_),
          jsonLength_,
          options,
          response);
      if (response.httpStatus == 412) {
        resetRetry();
        setState(SessionSyncState::ReadingManifest);
        return true;
      }
      if (!acceptSuccessfulStatus(
              response, SessionSyncState::CreatingManifest, nowMs)) {
        return true;
      }
      if (response.bodyTruncated ||
          !parseCloudManifest(
              base64Buffer_, response.bodyLength, cloudManifest_, parsedAbsent)) {
        beginFailure(response.bodyTruncated
                         ? SessionSyncError::ResponseTooLarge
                         : SessionSyncError::InvalidCloudResponse);
        return true;
      }
      if (parsedAbsent ||
          SessionSyncProtocol::evaluateCloudManifest(
              immutable_, cloudManifest_) != SyncManifestDecision::Resume ||
          cloudManifest_.nextChunk != 0U ||
          cloudManifest_.uploadedRecords != 0U) {
        beginFailure(SessionSyncError::CloudConflict);
        return true;
      }
      uploadStartedAtMs_ = cloudManifest_.uploadStartedAtMs;
      uploadStartedAtValid_ = cloudManifest_.uploadStartedAtValid;
#if SESSION_SYNC_TEST_CONSOLE
      Serial.println("Session upload manifest created");
#endif
      setState(SessionSyncState::CreatingIndex);
      return true;

    case SessionSyncState::CreatingIndex:
      if (!buildIndexJson(false, 0U, jsonLength_)) {
        beginFailure(SessionSyncError::BufferOverflow);
        return false;
      }
      options.printSilent = true;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Put,
          "@index",
          reinterpret_cast<const uint8_t*>(jsonBuffer_),
          jsonLength_,
          options,
          response);
      if (acceptSuccessfulStatus(
              response, SessionSyncState::CreatingIndex, nowMs)) {
        skipRecordsRemaining_ = 0U;
        setState(SessionSyncState::PersistingUploading);
      }
      return true;

    case SessionSyncState::PersistingUploading: {
      const SessionStorageError transition =
          storage_->transitionSynchronizationState(
              reader_, SessionSynchronizationState::Uploading);
      if (transition != SessionStorageError::None) {
        beginFailure(SessionSyncError::LocalStorageError);
        return false;
      }
      retained_ = storage_->getRetainedSessionInfo();
      refreshLocalSynchronizationState();
      setState(skipRecordsRemaining_ > 0U
                   ? SessionSyncState::SeekingReader
                   : (nextChunk_ >= immutable_.chunkCount
                          ? SessionSyncState::ReadingCompletionManifest
                          : SessionSyncState::BuildingChunk));
      return false;
    }

    case SessionSyncState::SeekingReader: {
      if (skipRecordsRemaining_ > resumeRecordsToSkip_) {
        beginFailure(SessionSyncError::CloudConflict);
        return false;
      }
      const uint32_t alreadySkipped =
          resumeRecordsToSkip_ - skipRecordsRemaining_;
      const uint32_t batch = min(skipRecordsRemaining_, 8U);
      if (immutable_.firstLogicalIndex >
          UINT64_MAX - static_cast<uint64_t>(alreadySkipped) -
              static_cast<uint64_t>(batch > 0U ? batch - 1U : 0U)) {
        beginFailure(SessionSyncError::LocalRecordValidation);
        return false;
      }
      for (uint32_t index = 0U; index < batch; ++index) {
        FlashRecordMetadata metadata{};
        const SessionStorageError readResult =
            storage_->readNextEncodedRecord(
                reader_, rawChunkBuffer_, FLASH_RECORD_BYTES, metadata);
        const uint64_t expected = immutable_.firstLogicalIndex +
                                  alreadySkipped + index;
        if (readResult != SessionStorageError::None ||
            metadata.sessionId != immutable_.sessionId ||
            metadata.logicalRecordIndex != expected) {
          beginFailure(SessionSyncError::LocalRecordValidation);
          return false;
        }
      }
      skipRecordsRemaining_ -= batch;
      if (skipRecordsRemaining_ == 0U) {
        setState(nextChunk_ >= immutable_.chunkCount
                     ? SessionSyncState::ReadingCompletionManifest
                     : SessionSyncState::BuildingChunk);
      }
      return false;
    }

    case SessionSyncState::BuildingChunk:
      if (nextChunk_ >= immutable_.chunkCount) {
        setState(SessionSyncState::ReadingCompletionManifest);
      } else if (buildChunk()) {
        setState(SessionSyncState::UploadingChunk);
      }
      return false;

    case SessionSyncState::UploadingChunk: {
      char chunkKey[SESSION_SYNC_CHUNK_KEY_BYTES]{};
      char suffix[32]{};
      if (SessionSyncProtocol::formatChunkKey(
              nextChunk_, chunkKey, sizeof(chunkKey)) !=
              SessionSyncProtocolError::Ok ||
          snprintf(suffix, sizeof(suffix), "chunks/%s", chunkKey) <= 0) {
        beginFailure(SessionSyncError::BufferOverflow);
        return false;
      }
      options.printSilent =
          SessionSyncProtocol::printSilentForRequestSite(
              SyncRestRequestSite::ChunkPut);
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Put,
          suffix,
          reinterpret_cast<const uint8_t*>(jsonBuffer_),
          jsonLength_,
          options,
          response);
      if (acceptSuccessfulStatus(
              response, SessionSyncState::UploadingChunk, nowMs)) {
        setState(SessionSyncState::VerifyingChunk);
      }
      return true;
    }

    case SessionSyncState::VerifyingChunk: {
      char chunkKey[SESSION_SYNC_CHUNK_KEY_BYTES]{};
      char suffix[40]{};
      if (SessionSyncProtocol::formatChunkKey(
              nextChunk_, chunkKey, sizeof(chunkKey)) !=
              SessionSyncProtocolError::Ok ||
          snprintf(suffix, sizeof(suffix), "chunks/%s/meta", chunkKey) <= 0) {
        beginFailure(SessionSyncError::BufferOverflow);
        return false;
      }
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Get,
          suffix,
          nullptr,
          0U,
          options,
          response);
      if (!acceptSuccessfulStatus(
              response, SessionSyncState::VerifyingChunk, nowMs)) {
        return true;
      }
      if (response.bodyTruncated ||
          !parseAndVerifyChunkMeta(base64Buffer_, response.bodyLength)) {
        beginFailure(response.bodyTruncated
                         ? SessionSyncError::ResponseTooLarge
                         : SessionSyncError::CloudConflict);
      } else {
        setState(SessionSyncState::UpdatingProgress);
      }
      return true;
    }

    case SessionSyncState::UpdatingProgress:
      if (!buildProgressPatch(jsonLength_)) {
        beginFailure(SessionSyncError::BufferOverflow);
        return false;
      }
      options.printSilent = true;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Patch,
          "manifest",
          reinterpret_cast<const uint8_t*>(jsonBuffer_),
          jsonLength_,
          options,
          response);
      if (acceptSuccessfulStatus(
              response, SessionSyncState::UpdatingProgress, nowMs)) {
        ++nextChunk_;
        uploadedRecords_ += chunkRecordCount_;
        portENTER_CRITICAL(&mux_);
        status_.nextChunk = nextChunk_;
        status_.uploadedRecords = uploadedRecords_;
        portEXIT_CRITICAL(&mux_);
        printChunkProgressIfEnabled();
        setState(nextChunk_ >= immutable_.chunkCount
                     ? SessionSyncState::ReadingCompletionManifest
                     : SessionSyncState::BuildingChunk);
      }
      return true;

    case SessionSyncState::ReadingCompletionManifest: {
      options.requestEtag = true;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Get,
          "manifest",
          nullptr,
          0U,
          options,
          response);
      if (!acceptSuccessfulStatus(
              response,
              SessionSyncState::ReadingCompletionManifest,
              nowMs)) {
        return true;
      }
      if (response.bodyTruncated || response.etag[0] == '\0' ||
          !parseCloudManifest(
              base64Buffer_, response.bodyLength, cloudManifest_, parsedAbsent)) {
        beginFailure(response.bodyTruncated
                         ? SessionSyncError::ResponseTooLarge
                         : SessionSyncError::InvalidCloudResponse);
        return true;
      }
      const SyncManifestDecision decision =
          SessionSyncProtocol::evaluateCloudManifest(
              immutable_, cloudManifest_);
      uploadStartedAtMs_ = cloudManifest_.uploadStartedAtMs;
      uploadStartedAtValid_ = cloudManifest_.uploadStartedAtValid;
      if (decision == SyncManifestDecision::AlreadyComplete) {
        cloudAlreadyComplete_ = true;
        setState(cloudManifest_.immutable.schemaVersion ==
                         SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION
                     ? SessionSyncState::PersistingSynced
                     : SessionSyncState::UpdatingIndex);
      } else if (decision == SyncManifestDecision::Resume &&
                 cloudManifest_.nextChunk == immutable_.chunkCount &&
                 cloudManifest_.uploadedRecords == immutable_.retainedCount) {
        strncpy(manifestEtag_, response.etag, sizeof(manifestEtag_) - 1U);
        setState(SessionSyncState::CommittingCompletion);
      } else if (decision ==
                 SyncManifestDecision::UnsupportedCloudManifest) {
        beginFailure(SessionSyncError::UnsupportedCloudManifest);
      } else {
        beginFailure(SessionSyncError::CloudConflict);
      }
      return true;
    }

    case SessionSyncState::CommittingCompletion:
      if (!uploadStartedAtValid_ ||
          !buildManifestJson(
              true,
              immutable_.chunkCount,
              immutable_.retainedCount,
              uploadStartedAtMs_,
              jsonLength_)) {
        beginFailure(SessionSyncError::InvalidCloudResponse);
        return false;
      }
      options.printSilent =
          SessionSyncProtocol::printSilentForRequestSite(
              SyncRestRequestSite::CompletionConditionalManifestPut);
      options.ifMatch = manifestEtag_;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Put,
          "manifest",
          reinterpret_cast<const uint8_t*>(jsonBuffer_),
          jsonLength_,
          options,
          response);
      if (response.httpStatus == 412) {
        resetRetry();
        completionPreconditionFailed_ = true;
        setState(SessionSyncState::VerifyingCompletion);
      } else if (acceptSuccessfulStatus(
                     response,
                     SessionSyncState::CommittingCompletion,
                     nowMs)) {
        completionPreconditionFailed_ = false;
        setState(SessionSyncState::VerifyingCompletion);
      }
      return true;

    case SessionSyncState::VerifyingCompletion:
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Get,
          "manifest",
          nullptr,
          0U,
          options,
          response);
      if (!acceptSuccessfulStatus(
              response, SessionSyncState::VerifyingCompletion, nowMs)) {
        return true;
      }
      if (response.bodyTruncated ||
          !parseCloudManifest(
              base64Buffer_, response.bodyLength, cloudManifest_, parsedAbsent)) {
        beginFailure(response.bodyTruncated
                         ? SessionSyncError::ResponseTooLarge
                         : SessionSyncError::InvalidCloudResponse);
        return true;
      }
      if ((completionPreconditionFailed_ &&
           SessionSyncProtocol::evaluateCompletionAfter412(
               immutable_, cloudManifest_) !=
               SyncCompletion412Decision::AcceptMatchingComplete) ||
          (!completionPreconditionFailed_ &&
           SessionSyncProtocol::evaluateCloudManifest(
               immutable_, cloudManifest_) !=
               SyncManifestDecision::AlreadyComplete)) {
        beginFailure(SessionSyncError::CloudConflict);
      } else {
        uploadStartedAtMs_ = cloudManifest_.uploadStartedAtMs;
        uploadStartedAtValid_ = cloudManifest_.uploadStartedAtValid;
        setState(SessionSyncState::UpdatingIndex);
      }
      return true;

    case SessionSyncState::UpdatingIndex:
      if (!uploadStartedAtValid_ ||
          !buildIndexJson(true, uploadStartedAtMs_, jsonLength_)) {
        beginFailure(SessionSyncError::InvalidCloudResponse);
        return false;
      }
      options.printSilent = true;
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Put,
          "@index",
          reinterpret_cast<const uint8_t*>(jsonBuffer_),
          jsonLength_,
          options,
          response);
      if (acceptSuccessfulStatus(
              response, SessionSyncState::UpdatingIndex, nowMs)) {
        setState(SessionSyncState::VerifyingIndex);
      }
      return true;

    case SessionSyncState::VerifyingIndex:
      performRequest(
          bridge,
          FirebaseDatabaseMethod::Get,
          "@index",
          nullptr,
          0U,
          options,
          response);
      if (!acceptSuccessfulStatus(
              response, SessionSyncState::VerifyingIndex, nowMs)) {
        return true;
      }
      if (response.bodyTruncated ||
          !parseAndVerifyIndex(base64Buffer_, response.bodyLength)) {
        beginFailure(response.bodyTruncated
                         ? SessionSyncError::ResponseTooLarge
                         : SessionSyncError::CloudConflict);
      } else {
        setState(SessionSyncState::PersistingSynced);
      }
      return true;

    case SessionSyncState::PersistingSynced: {
      const SessionStorageError transition =
          storage_->transitionSynchronizationState(
              reader_, SessionSynchronizationState::Synced);
      if (transition != SessionStorageError::None) {
        beginFailure(SessionSyncError::LocalStorageError);
        return false;
      }
      refreshLocalSynchronizationState();
      closeReader();
      releaseBuffers();
      portENTER_CRITICAL(&mux_);
      status_.lastError = SessionSyncError::None;
      status_.state = SessionSyncState::Complete;
      status_.cancellationRequested = false;
      portEXIT_CRITICAL(&mux_);
#if SESSION_SYNC_TEST_CONSOLE
      Serial.println("Session upload completed and verified");
#endif
      return false;
    }

    case SessionSyncState::Failing:
      if (reader_.open && pendingFailure_ != SessionSyncError::LocalStorageError) {
        const SessionStorageError transition =
            storage_->transitionSynchronizationState(
                reader_, SessionSynchronizationState::SyncError);
        if (transition != SessionStorageError::None) {
          pendingFailure_ = SessionSyncError::LocalStorageError;
          portENTER_CRITICAL(&mux_);
          status_.lastError = pendingFailure_;
          portEXIT_CRITICAL(&mux_);
        }
      }
      finishFailure();
      return false;

    case SessionSyncState::Cancelling:
      cancelNow();
      return false;

    case SessionSyncState::Disabled:
    case SessionSyncState::Idle:
    case SessionSyncState::Cancelled:
    case SessionSyncState::Complete:
    case SessionSyncState::Error:
      return false;

    case SessionSyncState::OpeningReader:
      // The reader lease is acquired during request initialization before the
      // first cloud transaction, so this transient state is not used.
      beginFailure(SessionSyncError::LocalStorageError);
      return false;

    case SessionSyncState::RetryWaiting:
      return false;
  }

  beginFailure(SessionSyncError::UnexpectedHttpStatus);
  return false;
}
