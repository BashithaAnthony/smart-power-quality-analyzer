#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "SessionStorage.h"
#include "SessionSyncProtocol.h"

class FirebaseBridge;
class SessionLogger;

static_assert(SESSION_SYNC_RECORD_BYTES == FLASH_RECORD_BYTES,
              "Session upload must preserve the complete PQR1 record");

enum class FirebaseDatabaseMethod : uint8_t {
  Get = 0,
  Put,
  Patch
};

struct FirebaseDatabaseRequestOptions {
  bool printSilent;
  bool requestEtag;
  const char* ifMatch;
};

struct FirebaseDatabaseResponse {
  int httpStatus;
  size_t bodyLength;
  bool bodyTruncated;
  bool authenticationFailed;
  char etag[80];
};

enum class SessionSyncState : uint8_t {
  Disabled = 0,
  Idle,
  Requested,
  ReadingManifest,
  CreatingManifest,
  CreatingIndex,
  PersistingUploading,
  OpeningReader,
  SeekingReader,
  BuildingChunk,
  UploadingChunk,
  VerifyingChunk,
  UpdatingProgress,
  ReadingCompletionManifest,
  CommittingCompletion,
  VerifyingCompletion,
  UpdatingIndex,
  VerifyingIndex,
  PersistingSynced,
  RetryWaiting,
  Cancelling,
  Failing,
  Cancelled,
  Complete,
  Error
};

enum class SessionSyncError : uint8_t {
  None = 0,
  Disabled,
  AlreadyInProgress,
  WifiUnavailable,
  FirebaseUnavailable,
  LoggerBusy,
  StorageUnavailable,
  RecoveryBlocked,
  NoRetainedSession,
  EmptySession,
  ReaderAlreadyOpen,
  PsramUnavailable,
  AllocationFailed,
  LocalStorageError,
  LocalRecordValidation,
  ResponseTooLarge,
  InvalidCloudResponse,
  UnsupportedCloudManifest,
  CloudConflict,
  SecurityRulesDenied,
  AuthenticationFailed,
  UnexpectedHttpStatus,
  RetryExhausted,
  BufferOverflow,
  Cancelled,
  MalformedRequest
};

struct SessionSyncStatus {
  SessionSyncState state;
  SessionSyncError lastError;
  char sessionKey[SESSION_SYNC_SESSION_KEY_BYTES];
  uint32_t chunkCount;
  uint32_t nextChunk;
  uint64_t uploadedRecords;
  uint64_t retainedRecords;
  uint32_t retryCount;
  int lastHttpStatus;
  SessionSynchronizationState localSynchronizationState;
  bool wifiConnected;
  bool firebaseAuthenticated;
  bool readerOpen;
  bool cancellationRequested;
  bool psramBuffersAllocated;
};

class SessionSyncUploader {
 public:
  SessionSyncUploader() = default;
  ~SessionSyncUploader();

  SessionSyncUploader(const SessionSyncUploader&) = delete;
  SessionSyncUploader& operator=(const SessionSyncUploader&) = delete;

  void attach(SessionStorage& storage, SessionLogger& logger, bool enabled);
  bool requestUpload();
  void requestCancellation();
  SessionSyncStatus getStatus() const;

 private:
  friend class FirebaseBridge;

  bool service(FirebaseBridge& bridge, uint32_t nowMs);
  bool hasPendingWork() const;
  uint32_t nextServiceDelayMs(uint32_t nowMs) const;
  void recordRequestRejection(SessionSyncError error);

  bool initializeRequest();
  bool allocateBuffers();
  void releaseBuffers();
  void closeReader();
  void cancelNow();
  void beginFailure(SessionSyncError error, int httpStatus = 0);
  void finishFailure();
  void setState(SessionSyncState state);
  void refreshLocalSynchronizationState();

  bool buildImmutableManifest();
  bool parseCloudManifest(const char* json,
                          size_t jsonLength,
                          SyncParsedCloudManifest& manifest,
                          bool& absent) const;
  bool buildManifestJson(bool complete,
                         uint32_t nextChunk,
                         uint64_t uploadedRecords,
                         uint64_t uploadStartedAtMs,
                         size_t& outputLength);
  bool buildIndexJson(bool complete,
                      uint64_t uploadStartedAtMs,
                      size_t& outputLength);
  bool buildChunk();
  bool buildChunkJson(size_t& outputLength);
  bool parseAndVerifyChunkMeta(const char* json, size_t jsonLength) const;
  bool parseAndVerifyIndex(const char* json, size_t jsonLength) const;
  bool buildProgressPatch(size_t& outputLength);
  bool makePath(char* destination,
                size_t destinationLength,
                const char* suffix) const;
  bool performRequest(FirebaseBridge& bridge,
                      FirebaseDatabaseMethod method,
                      const char* suffix,
                      const uint8_t* body,
                      size_t bodyLength,
                      const FirebaseDatabaseRequestOptions& options,
                      FirebaseDatabaseResponse& response);
  bool acceptSuccessfulStatus(const FirebaseDatabaseResponse& response,
                              SessionSyncState retryState,
                              uint32_t nowMs,
                              bool allowPreconditionFailure = false);
  void scheduleRetry(SessionSyncState retryState,
                     int httpStatus,
                     uint32_t nowMs);
  void resetRetry();
  void printChunkProgressIfEnabled() const;

  SessionStorage* storage_ = nullptr;
  SessionLogger* logger_ = nullptr;
  SessionStorageReader reader_{};
  RetainedSessionInfo retained_{};
  SyncManifestImmutable immutable_{};
  SyncParsedCloudManifest cloudManifest_{};
  SessionSyncStatus status_{};
  SessionSyncState retryState_ = SessionSyncState::Idle;
  SessionSyncError pendingFailure_ = SessionSyncError::None;
  uint8_t* rawChunkBuffer_ = nullptr;
  char* base64Buffer_ = nullptr;
  char* jsonBuffer_ = nullptr;
  size_t jsonLength_ = 0U;
  uint32_t retryDeadlineMs_ = 0U;
  uint32_t retryIndex_ = 0U;
  uint32_t authRetryCount_ = 0U;
  uint32_t nextChunk_ = 0U;
  uint32_t resumeRecordsToSkip_ = 0U;
  uint32_t skipRecordsRemaining_ = 0U;
  uint32_t chunkRecordCount_ = 0U;
  uint32_t chunkRawBytes_ = 0U;
  uint32_t chunkCrc32c_ = 0U;
  uint32_t manifestCrc32c_ = 0U;
  uint64_t chunkFirstLogicalIndex_ = 0U;
  uint64_t chunkLastLogicalIndex_ = 0U;
  uint64_t uploadStartedAtMs_ = 0U;
  uint64_t uploadedRecords_ = 0U;
  bool uploadStartedAtValid_ = false;
  bool completionPreconditionFailed_ = false;
  bool cloudAlreadyComplete_ = false;
  bool cancellationRequested_ = false;
  char sessionKey_[SESSION_SYNC_SESSION_KEY_BYTES]{};
  char manifestEtag_[80]{};
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
