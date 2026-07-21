#pragma once

#include <stdint.h>

#ifndef SESSION_UI_CONTROLLER_HOST_TEST
#define SESSION_UI_CONTROLLER_HOST_TEST 0
#endif

enum class SessionUiLoggerPhase : uint8_t {
  Idle = 0,
  Starting,
  PreparingStorage,
  Active,
  Stopping,
  Finalizing,
  Retained,
  Clearing,
  BusyOrError
};

enum class SessionUiRequestDecision : uint8_t {
  Allowed = 0,
  Idempotent,
  LoggerBusy,
  StorageUnavailable,
  RecoveryBlocked,
  RetainedSessionBlocksStart,
  NoActiveSession,
  NoRetainedSession,
  AlreadySynchronized,
  WifiUnavailable,
  FirebaseUnavailable,
  SyncInProgress,
  BackendRejected
};

struct SessionUiRequestContext {
  SessionUiLoggerPhase loggerPhase;
  bool storageAvailable;
  bool recoveryBlocked;
  bool retainedSessionAvailable;
  bool retainedSessionEligibleForSync;
  bool synchronized;
  bool wifiConnected;
  bool firebaseEnabled;
  bool syncInProgress;
  bool clearInProgress;
  bool startRequestInProgress;
  bool stopRequestInProgress;
};

struct SessionUiStorageUsage {
  uint64_t committedRetainedRecords;
  uint64_t acceptedRecordsNotYetCommitted;
  uint64_t effectiveUsedRecords;
  uint64_t remainingRecords;
  uint32_t filledPercent;
};

enum class SessionUiClearAction : uint8_t {
  Armed = 0,
  Requested,
  AlreadyInProgress,
  NotArmed,
  Rejected
};

enum class SessionUiMessage : uint8_t {
  Standby = 0,
  Starting,
  PreparingStorage,
  Logging,
  Stopping,
  Draining,
  Finalizing,
  Finalized,
  StorageFull,
  Recovered,
  StartFailed,
  StopFailed,
  WaitingForWifi,
  Uploading,
  Verifying,
  Synced,
  SyncFailed,
  ClearConfirmation,
  ClearUnsyncedConfirmation,
  Clearing,
  StorageCleared,
  ClearFailed,
  StorageError
};

struct SessionUiStatusContext {
  SessionUiLoggerPhase loggerPhase;
  bool backendError;
  bool recoveredSession;
  bool fifoPending;
  bool storageCapacityReached;
  bool synchronized;
  bool clearConfirmationArmed;
  bool clearConfirmationUnsynced;
  bool clearInProgress;
  bool clearCompleted;
  bool clearSucceeded;
  bool syncError;
  bool syncInProgress;
  bool syncVerifying;
  bool wifiConnected;
  bool startBackendRejected;
  bool stopBackendRejected;
  bool syncWifiRejected;
  bool syncRejected;
};

class SessionUiClearConfirmation {
 public:
  static constexpr uint32_t kWindowMs = 5000U;

  void arm(uint32_t nowMs);
  void cancel();
  bool consume(uint32_t nowMs);
  bool isArmed() const;
  bool hasExpired(uint32_t nowMs) const;

 private:
  bool armed_ = false;
  uint32_t deadlineMs_ = 0U;
};

namespace SessionUiPolicy {

SessionUiRequestDecision evaluateStart(const SessionUiRequestContext& context);
SessionUiRequestDecision evaluateStop(const SessionUiRequestContext& context);
SessionUiRequestDecision evaluateSync(const SessionUiRequestContext& context);
SessionUiRequestDecision evaluateClear(const SessionUiRequestContext& context);
SessionUiMessage selectStatusMessage(const SessionUiStatusContext& context);
SessionUiStorageUsage calculateStorageUsage(
    uint32_t capacity,
    uint64_t committedRetainedRecords,
    uint64_t acceptedRecords,
    uint64_t committedSessionRecords,
    bool includeAcceptedPendingRecords,
    uint64_t minimumEffectiveUsedRecords);

}  // namespace SessionUiPolicy

#if !SESSION_UI_CONTROLLER_HOST_TEST

#include "FirebaseBridge.h"
#include "SessionLogger.h"
#include "SessionStorage.h"

struct SessionUiSnapshot {
  bool attached;
  bool storageAvailable;
  bool recoveryBlocked;
  bool retainedSessionAvailable;
  bool synchronized;
  bool uploadInProgress;
  bool sessionTimeValid;
  bool clearConfirmationArmed;
  bool clearConfirmationUnsynced;
  bool clearInProgress;
  bool clearCompleted;
  bool clearSucceeded;
  bool storageCapacityReached;
  bool startAvailable;
  bool stopAvailable;
  bool syncAvailable;
  bool clearAvailable;
  SessionLoggerState loggerState;
  PersistentSessionState persistentState;
  SessionSyncState syncState;
  SessionSynchronizationState localSynchronizationState;
  SessionUiMessage message;
  SessionLoggerError loggerError;
  SessionSyncError syncError;
  uint32_t fifoOccupancy;
  uint32_t fifoHighWaterMark;
  uint32_t maximumRecords;
  uint32_t currentChunk;
  uint32_t totalChunks;
  uint64_t retainedRecords;
  uint64_t committedRetainedRecords;
  uint64_t pendingAcceptedRecords;
  uint64_t effectiveUsedRecords;
  uint64_t remainingRecords;
  uint32_t storageFilledPercent;
  uint32_t preparationSectorsCompleted;
  uint32_t preparationSectorsTotal;
  uint32_t preparationPercent;
  uint64_t uploadedRecords;
  uint64_t droppedRecords;
};

class SessionUiController {
 public:
  void begin(SessionLogger& logger,
             SessionStorage& storage,
             FirebaseBridge& firebase);

  SessionUiRequestDecision requestStart(uint32_t flushIntervalSeconds);
  SessionUiRequestDecision requestStop();
  SessionUiRequestDecision requestSync();
  void requestSyncCancellation();
  SessionUiClearAction requestClear(uint32_t nowMs);
  SessionUiClearAction armClear(uint32_t nowMs);
  SessionUiClearAction confirmClear(uint32_t nowMs);
  void cancelClearConfirmation();
  void service(uint32_t nowMs, bool logScreenVisible);
  bool clearConfirmationArmed() const;
  bool clearInProgress() const;
  SessionUiSnapshot getSnapshot() const;

 private:
  enum class LastCommand : uint8_t {
    None = 0,
    Start,
    Stop,
    Sync,
    Clear
  };

  enum class ClearConfirmationSource : uint8_t {
    None = 0,
    ProductionUi,
    TestConsole
  };

  SessionUiRequestContext buildRequestContext(
      const SessionLoggerStatus& logger,
      const SessionStorageStatus& storage,
      const RetainedSessionInfo& retained,
      const SessionSyncStatus& sync) const;
  static SessionUiLoggerPhase mapLoggerPhase(SessionLoggerState state);
  static bool syncOperationActive(SessionSyncState state);
  static bool syncVerificationActive(SessionSyncState state);
  SessionUiClearAction armClearInternal(
      uint32_t nowMs,
      ClearConfirmationSource source);
  bool clearArmContextStillMatches(
      const SessionLoggerStatus& logger,
      const SessionStorageStatus& storage,
      const RetainedSessionInfo& retained,
      const SessionSyncStatus& sync) const;
  void resetClearResult();
  SessionUiMessage deriveMessage(const SessionLoggerStatus& logger,
                                 const SessionStorageStatus& storage,
                                 const SessionSyncStatus& sync) const;
  void remember(LastCommand command, SessionUiRequestDecision decision);

  SessionLogger* logger_ = nullptr;
  SessionStorage* storage_ = nullptr;
  FirebaseBridge* firebase_ = nullptr;
  LastCommand lastCommand_ = LastCommand::None;
  SessionUiRequestDecision lastDecision_ =
      SessionUiRequestDecision::Allowed;
  SessionUiClearConfirmation clearConfirmation_{};
  ClearConfirmationSource clearConfirmationSource_ =
      ClearConfirmationSource::None;
  bool clearConfirmationUnsynced_ = false;
  bool clearRequestIssued_ = false;
  bool startRequestIssued_ = false;
  bool stopRequestIssued_ = false;
  bool clearCompleted_ = false;
  bool clearSucceeded_ = false;
  SessionLoggerState clearArmLoggerState_ = SessionLoggerState::Disabled;
  PersistentSessionState clearArmPersistentState_ =
      PersistentSessionState::Empty;
  SessionSyncState clearArmSyncState_ = SessionSyncState::Disabled;
  SessionSynchronizationState clearArmLocalSyncState_ =
      SessionSynchronizationState::NotSynced;
  uint64_t clearArmSessionId_ = 0U;
  mutable uint64_t storageUsageSessionId_ = 0U;
  mutable uint64_t minimumActiveStorageUsage_ = 0U;
};

#endif  // !SESSION_UI_CONTROLLER_HOST_TEST
