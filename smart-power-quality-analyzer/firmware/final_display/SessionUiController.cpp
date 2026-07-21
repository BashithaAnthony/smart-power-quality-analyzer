#include "SessionUiController.h"

#if SESSION_UI_CONTROLLER_HOST_TEST
#include <assert.h>
#include <stdio.h>
#endif

void SessionUiClearConfirmation::arm(uint32_t nowMs) {
  armed_ = true;
  deadlineMs_ = nowMs + kWindowMs;
}

void SessionUiClearConfirmation::cancel() {
  armed_ = false;
  deadlineMs_ = 0U;
}

bool SessionUiClearConfirmation::consume(uint32_t nowMs) {
  if (!armed_ || hasExpired(nowMs)) {
    cancel();
    return false;
  }
  cancel();
  return true;
}

bool SessionUiClearConfirmation::isArmed() const {
  return armed_;
}

bool SessionUiClearConfirmation::hasExpired(uint32_t nowMs) const {
  return armed_ && static_cast<int32_t>(nowMs - deadlineMs_) >= 0;
}

namespace SessionUiPolicy {

SessionUiRequestDecision evaluateStart(const SessionUiRequestContext& context) {
  if (context.startRequestInProgress) {
    return SessionUiRequestDecision::Idempotent;
  }
  if (!context.storageAvailable) {
    return SessionUiRequestDecision::StorageUnavailable;
  }
  if (context.recoveryBlocked) {
    return SessionUiRequestDecision::RecoveryBlocked;
  }
  if (context.loggerPhase == SessionUiLoggerPhase::Retained ||
      context.retainedSessionAvailable) {
    return SessionUiRequestDecision::RetainedSessionBlocksStart;
  }
  if (context.loggerPhase != SessionUiLoggerPhase::Idle) {
    return SessionUiRequestDecision::LoggerBusy;
  }
  return SessionUiRequestDecision::Allowed;
}

SessionUiRequestDecision evaluateStop(const SessionUiRequestContext& context) {
  if (context.stopRequestInProgress) {
    return SessionUiRequestDecision::Idempotent;
  }
  if (context.loggerPhase == SessionUiLoggerPhase::Active) {
    return SessionUiRequestDecision::Allowed;
  }
  if (context.loggerPhase == SessionUiLoggerPhase::Stopping ||
      context.loggerPhase == SessionUiLoggerPhase::Finalizing ||
      context.loggerPhase == SessionUiLoggerPhase::Retained) {
    return SessionUiRequestDecision::Idempotent;
  }
  return SessionUiRequestDecision::NoActiveSession;
}

SessionUiRequestDecision evaluateSync(const SessionUiRequestContext& context) {
  if (context.loggerPhase == SessionUiLoggerPhase::Starting ||
      context.loggerPhase == SessionUiLoggerPhase::PreparingStorage ||
      context.loggerPhase == SessionUiLoggerPhase::Active ||
      context.loggerPhase == SessionUiLoggerPhase::Stopping ||
      context.loggerPhase == SessionUiLoggerPhase::Finalizing ||
      context.loggerPhase == SessionUiLoggerPhase::BusyOrError) {
    return SessionUiRequestDecision::LoggerBusy;
  }
  if (context.syncInProgress) {
    return SessionUiRequestDecision::Idempotent;
  }
  if (!context.firebaseEnabled) {
    return SessionUiRequestDecision::FirebaseUnavailable;
  }
  if (!context.storageAvailable) {
    return SessionUiRequestDecision::StorageUnavailable;
  }
  if (context.recoveryBlocked) {
    return SessionUiRequestDecision::RecoveryBlocked;
  }
  if (!context.retainedSessionAvailable ||
      !context.retainedSessionEligibleForSync) {
    return SessionUiRequestDecision::NoRetainedSession;
  }
  if (context.synchronized) {
    return SessionUiRequestDecision::AlreadySynchronized;
  }
  if (!context.wifiConnected) {
    return SessionUiRequestDecision::WifiUnavailable;
  }
  return SessionUiRequestDecision::Allowed;
}

SessionUiRequestDecision evaluateClear(const SessionUiRequestContext& context) {
  if (!context.storageAvailable) {
    return SessionUiRequestDecision::StorageUnavailable;
  }
  if (context.recoveryBlocked) {
    return SessionUiRequestDecision::RecoveryBlocked;
  }
  if (context.clearInProgress ||
      context.loggerPhase == SessionUiLoggerPhase::Clearing) {
    return SessionUiRequestDecision::Idempotent;
  }
  if (context.syncInProgress) {
    return SessionUiRequestDecision::SyncInProgress;
  }
  if (context.loggerPhase == SessionUiLoggerPhase::Starting ||
      context.loggerPhase == SessionUiLoggerPhase::PreparingStorage ||
      context.loggerPhase == SessionUiLoggerPhase::Active ||
      context.loggerPhase == SessionUiLoggerPhase::Stopping ||
      context.loggerPhase == SessionUiLoggerPhase::Finalizing ||
      context.loggerPhase == SessionUiLoggerPhase::BusyOrError) {
    return SessionUiRequestDecision::LoggerBusy;
  }
  if (!context.retainedSessionAvailable) {
    return SessionUiRequestDecision::NoRetainedSession;
  }
  if (context.loggerPhase != SessionUiLoggerPhase::Retained) {
    return SessionUiRequestDecision::LoggerBusy;
  }
  return SessionUiRequestDecision::Allowed;
}

SessionUiMessage selectStatusMessage(const SessionUiStatusContext& context) {
  if (context.backendError) {
    return SessionUiMessage::StorageError;
  }
  if (context.clearConfirmationArmed) {
    return context.clearConfirmationUnsynced
               ? SessionUiMessage::ClearUnsyncedConfirmation
               : SessionUiMessage::ClearConfirmation;
  }
  if (context.clearInProgress) {
    return SessionUiMessage::Clearing;
  }
  if (context.clearCompleted) {
    return context.clearSucceeded ? SessionUiMessage::StorageCleared
                                  : SessionUiMessage::ClearFailed;
  }
  if (context.syncError) {
    return SessionUiMessage::SyncFailed;
  }
  if (context.syncInProgress) {
    if (!context.wifiConnected) {
      return SessionUiMessage::WaitingForWifi;
    }
    return context.syncVerifying ? SessionUiMessage::Verifying
                                 : SessionUiMessage::Uploading;
  }
  const bool loggerStableForSyncMessage =
      context.loggerPhase == SessionUiLoggerPhase::Idle ||
      context.loggerPhase == SessionUiLoggerPhase::Retained;
  if (loggerStableForSyncMessage && context.syncWifiRejected) {
    return SessionUiMessage::WaitingForWifi;
  }
  if (loggerStableForSyncMessage && context.syncRejected) {
    return SessionUiMessage::SyncFailed;
  }
  if (context.startBackendRejected &&
      context.loggerPhase == SessionUiLoggerPhase::Idle) {
    return SessionUiMessage::StartFailed;
  }
  if (context.stopBackendRejected &&
      context.loggerPhase == SessionUiLoggerPhase::Active) {
    return SessionUiMessage::StopFailed;
  }

  switch (context.loggerPhase) {
    case SessionUiLoggerPhase::Idle:
      return SessionUiMessage::Standby;
    case SessionUiLoggerPhase::Starting:
      return SessionUiMessage::Starting;
    case SessionUiLoggerPhase::PreparingStorage:
      return SessionUiMessage::PreparingStorage;
    case SessionUiLoggerPhase::Active:
      return SessionUiMessage::Logging;
    case SessionUiLoggerPhase::Stopping:
      return context.fifoPending ? SessionUiMessage::Draining
                                 : SessionUiMessage::Stopping;
    case SessionUiLoggerPhase::Finalizing:
      return SessionUiMessage::Finalizing;
    case SessionUiLoggerPhase::Retained:
      if (context.storageCapacityReached) {
        return SessionUiMessage::StorageFull;
      }
      if (context.synchronized) {
        return SessionUiMessage::Synced;
      }
      return context.recoveredSession ? SessionUiMessage::Recovered
                                      : SessionUiMessage::Finalized;
    case SessionUiLoggerPhase::Clearing:
      return SessionUiMessage::Clearing;
    case SessionUiLoggerPhase::BusyOrError:
    default:
      return SessionUiMessage::StorageError;
  }
}

SessionUiStorageUsage calculateStorageUsage(
    uint32_t capacity,
    uint64_t committedRetainedRecords,
    uint64_t acceptedRecords,
    uint64_t committedSessionRecords,
    bool includeAcceptedPendingRecords,
    uint64_t minimumEffectiveUsedRecords) {
  SessionUiStorageUsage usage{};
  usage.committedRetainedRecords = committedRetainedRecords;
  if (includeAcceptedPendingRecords &&
      acceptedRecords >= committedSessionRecords) {
    usage.acceptedRecordsNotYetCommitted =
        acceptedRecords - committedSessionRecords;
  }

  const uint64_t capacity64 = capacity;
  uint64_t effective = committedRetainedRecords;
  if (UINT64_MAX - effective < usage.acceptedRecordsNotYetCommitted) {
    effective = UINT64_MAX;
  } else {
    effective += usage.acceptedRecordsNotYetCommitted;
  }
  if (includeAcceptedPendingRecords &&
      effective < minimumEffectiveUsedRecords) {
    effective = minimumEffectiveUsedRecords;
  }
  if (effective > capacity64) {
    effective = capacity64;
  }

  usage.effectiveUsedRecords = effective;
  usage.remainingRecords = capacity64 - effective;
  usage.filledPercent = capacity == 0U
                            ? 0U
                            : static_cast<uint32_t>(
                                  (effective * 100U) / capacity64);
  return usage;
}

}  // namespace SessionUiPolicy

#if !SESSION_UI_CONTROLLER_HOST_TEST

void SessionUiController::begin(SessionLogger& logger,
                                SessionStorage& storage,
                                FirebaseBridge& firebase) {
  logger_ = &logger;
  storage_ = &storage;
  firebase_ = &firebase;
  lastCommand_ = LastCommand::None;
  lastDecision_ = SessionUiRequestDecision::Allowed;
  clearConfirmation_.cancel();
  clearConfirmationSource_ = ClearConfirmationSource::None;
  clearConfirmationUnsynced_ = false;
  clearRequestIssued_ = false;
  startRequestIssued_ = false;
  stopRequestIssued_ = false;
  clearCompleted_ = false;
  clearSucceeded_ = false;
  storageUsageSessionId_ = 0U;
  minimumActiveStorageUsage_ = 0U;
}

SessionUiRequestDecision SessionUiController::requestStart(
    uint32_t flushIntervalSeconds) {
  cancelClearConfirmation();
  resetClearResult();
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    remember(LastCommand::Start,
             SessionUiRequestDecision::BackendRejected);
    return lastDecision_;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();
  SessionUiRequestDecision decision = SessionUiPolicy::evaluateStart(
      buildRequestContext(logger, storage, retained, sync));
  if (decision == SessionUiRequestDecision::Allowed) {
    // Latch before entering the backend. Even if another UI activation uses
    // the same stale frame, it cannot enqueue a second Start request.
    startRequestIssued_ = true;
    if (!logger_->startSession(flushIntervalSeconds)) {
      startRequestIssued_ = false;
      decision = SessionUiRequestDecision::BackendRejected;
    }
  }
  remember(LastCommand::Start, decision);
  return decision;
}

SessionUiRequestDecision SessionUiController::requestStop() {
  cancelClearConfirmation();
  resetClearResult();
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    remember(LastCommand::Stop,
             SessionUiRequestDecision::BackendRejected);
    return lastDecision_;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();
  SessionUiRequestDecision decision = SessionUiPolicy::evaluateStop(
      buildRequestContext(logger, storage, retained, sync));
  if (decision == SessionUiRequestDecision::Allowed) {
    // Latch before entering the backend so repeated level-triggered Select
    // input cannot submit a second Stop request from a stale UI snapshot.
    stopRequestIssued_ = true;
    if (!logger_->stopSession()) {
      stopRequestIssued_ = false;
      decision = SessionUiRequestDecision::BackendRejected;
    }
  }
  remember(LastCommand::Stop, decision);
  return decision;
}

SessionUiRequestDecision SessionUiController::requestSync() {
  cancelClearConfirmation();
  resetClearResult();
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    remember(LastCommand::Sync,
             SessionUiRequestDecision::FirebaseUnavailable);
    return lastDecision_;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();

  SessionUiRequestDecision decision = SessionUiPolicy::evaluateSync(
      buildRequestContext(logger, storage, retained, sync));
  if (decision == SessionUiRequestDecision::Allowed &&
      !firebase_->requestSessionUpload()) {
    const SessionSyncError error =
        firebase_->getSessionSyncStatus().lastError;
    switch (error) {
      case SessionSyncError::AlreadyInProgress:
        decision = SessionUiRequestDecision::Idempotent;
        break;
      case SessionSyncError::WifiUnavailable:
        decision = SessionUiRequestDecision::WifiUnavailable;
        break;
      case SessionSyncError::FirebaseUnavailable:
      case SessionSyncError::Disabled:
        decision = SessionUiRequestDecision::FirebaseUnavailable;
        break;
      case SessionSyncError::NoRetainedSession:
      case SessionSyncError::EmptySession:
        decision = SessionUiRequestDecision::NoRetainedSession;
        break;
      case SessionSyncError::LoggerBusy:
        decision = SessionUiRequestDecision::LoggerBusy;
        break;
      case SessionSyncError::RecoveryBlocked:
        decision = SessionUiRequestDecision::RecoveryBlocked;
        break;
      default:
        decision = SessionUiRequestDecision::BackendRejected;
        break;
    }
  }
  remember(LastCommand::Sync, decision);
  return decision;
}

void SessionUiController::requestSyncCancellation() {
  cancelClearConfirmation();
  if (firebase_ != nullptr) {
    firebase_->cancelSessionUpload();
  }
}

SessionUiClearAction SessionUiController::requestClear(uint32_t nowMs) {
  if (clearRequestIssued_) {
    return SessionUiClearAction::AlreadyInProgress;
  }
  if (clearConfirmation_.isArmed()) {
    if (!clearConfirmation_.hasExpired(nowMs)) {
      return confirmClear(nowMs);
    }
    cancelClearConfirmation();
  }
  return armClearInternal(nowMs, ClearConfirmationSource::ProductionUi);
}

SessionUiClearAction SessionUiController::armClear(uint32_t nowMs) {
  return armClearInternal(nowMs, ClearConfirmationSource::TestConsole);
}

SessionUiClearAction SessionUiController::armClearInternal(
    uint32_t nowMs,
    ClearConfirmationSource source) {
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    remember(LastCommand::Clear, SessionUiRequestDecision::BackendRejected);
    resetClearResult();
    return SessionUiClearAction::Rejected;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();

  const SessionUiRequestDecision decision = SessionUiPolicy::evaluateClear(
      buildRequestContext(logger, storage, retained, sync));
  remember(LastCommand::Clear, decision);
  resetClearResult();
  if (decision == SessionUiRequestDecision::Idempotent) {
    return SessionUiClearAction::AlreadyInProgress;
  }
  if (decision != SessionUiRequestDecision::Allowed) {
    cancelClearConfirmation();
    return SessionUiClearAction::Rejected;
  }

  clearConfirmation_.arm(nowMs);
  clearConfirmationSource_ = source;
  clearConfirmationUnsynced_ =
      retained.synchronizationState != SessionSynchronizationState::Synced;
  clearArmLoggerState_ = logger.state;
  clearArmPersistentState_ = storage.persistentSessionState;
  clearArmSyncState_ = sync.state;
  clearArmLocalSyncState_ = retained.synchronizationState;
  clearArmSessionId_ = retained.sessionId;
  return SessionUiClearAction::Armed;
}

SessionUiClearAction SessionUiController::confirmClear(uint32_t nowMs) {
  if (clearRequestIssued_) {
    return SessionUiClearAction::AlreadyInProgress;
  }
  if (!clearConfirmation_.isArmed() ||
      clearConfirmation_.hasExpired(nowMs)) {
    cancelClearConfirmation();
    remember(LastCommand::Clear, SessionUiRequestDecision::BackendRejected);
    return SessionUiClearAction::NotArmed;
  }
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    cancelClearConfirmation();
    remember(LastCommand::Clear, SessionUiRequestDecision::BackendRejected);
    return SessionUiClearAction::Rejected;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();
  if (!clearArmContextStillMatches(logger, storage, retained, sync)) {
    cancelClearConfirmation();
    remember(LastCommand::None, SessionUiRequestDecision::Allowed);
    return SessionUiClearAction::NotArmed;
  }
  SessionUiRequestDecision decision = SessionUiPolicy::evaluateClear(
      buildRequestContext(logger, storage, retained, sync));
  if (decision != SessionUiRequestDecision::Allowed) {
    cancelClearConfirmation();
    remember(LastCommand::Clear, decision);
    return decision == SessionUiRequestDecision::Idempotent
               ? SessionUiClearAction::AlreadyInProgress
               : SessionUiClearAction::Rejected;
  }

  if (!clearConfirmation_.consume(nowMs)) {
    cancelClearConfirmation();
    remember(LastCommand::Clear, SessionUiRequestDecision::BackendRejected);
    return SessionUiClearAction::NotArmed;
  }
  clearConfirmationSource_ = ClearConfirmationSource::None;
  clearConfirmationUnsynced_ = false;

  // Latch before calling the logger so repeated UI/console input cannot
  // submit another worker request, even if the backend rejects this one.
  clearRequestIssued_ = true;
  clearCompleted_ = false;
  clearSucceeded_ = false;
  if (!logger_->clearRetainedSession()) {
    clearRequestIssued_ = false;
    clearCompleted_ = true;
    clearSucceeded_ = false;
    decision = SessionUiRequestDecision::BackendRejected;
  }
  remember(LastCommand::Clear, decision);
  return decision == SessionUiRequestDecision::Allowed
             ? SessionUiClearAction::Requested
             : SessionUiClearAction::Rejected;
}

void SessionUiController::cancelClearConfirmation() {
  clearConfirmation_.cancel();
  clearConfirmationSource_ = ClearConfirmationSource::None;
  clearConfirmationUnsynced_ = false;
}

void SessionUiController::service(uint32_t nowMs, bool logScreenVisible) {
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    return;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();

  if (startRequestIssued_ && logger.state != SessionLoggerState::Idle) {
    startRequestIssued_ = false;
  }
  if (stopRequestIssued_ && logger.state != SessionLoggerState::Active) {
    stopRequestIssued_ = false;
  }

  if (clearConfirmation_.isArmed() &&
      (clearConfirmation_.hasExpired(nowMs) ||
       (clearConfirmationSource_ == ClearConfirmationSource::ProductionUi &&
        !logScreenVisible) ||
       !clearArmContextStillMatches(logger, storage, retained, sync))) {
    cancelClearConfirmation();
  }

  if (!clearRequestIssued_) {
    return;
  }
  if (logger.state == SessionLoggerState::Idle &&
      storage.persistentSessionState == PersistentSessionState::Empty &&
      !retained.available) {
    clearRequestIssued_ = false;
    clearCompleted_ = true;
    clearSucceeded_ = true;
  } else if (logger.state == SessionLoggerState::ErrorIncomplete ||
             logger.state == SessionLoggerState::Error) {
    clearRequestIssued_ = false;
    clearCompleted_ = true;
    clearSucceeded_ = false;
  }
}

bool SessionUiController::clearConfirmationArmed() const {
  return clearConfirmation_.isArmed();
}

bool SessionUiController::clearInProgress() const {
  return clearRequestIssued_;
}

SessionUiSnapshot SessionUiController::getSnapshot() const {
  SessionUiSnapshot snapshot{};
  snapshot.loggerState = SessionLoggerState::Disabled;
  snapshot.persistentState = PersistentSessionState::Empty;
  snapshot.syncState = SessionSyncState::Disabled;
  snapshot.localSynchronizationState =
      SessionSynchronizationState::NotSynced;
  snapshot.message = SessionUiMessage::StorageError;
  snapshot.loggerError = SessionLoggerError::StorageNotAvailable;
  snapshot.syncError = SessionSyncError::FirebaseUnavailable;
  if (logger_ == nullptr || storage_ == nullptr || firebase_ == nullptr) {
    return snapshot;
  }

  const SessionLoggerStatus logger = logger_->getStatus();
  const SessionStorageStatus storage = storage_->getStatus();
  const RetainedSessionInfo retained = storage_->getRetainedSessionInfo();
  const SessionSyncStatus sync = firebase_->getSessionSyncStatus();

  snapshot.attached = true;
  snapshot.storageAvailable = storage.available;
  snapshot.recoveryBlocked = storage.recoveryBlocked;
  snapshot.retainedSessionAvailable = retained.available;
  snapshot.synchronized =
      retained.synchronizationState == SessionSynchronizationState::Synced;
  snapshot.uploadInProgress = syncOperationActive(sync.state);
  snapshot.sessionTimeValid = retained.startWallClockValid;
  snapshot.clearConfirmationArmed = clearConfirmation_.isArmed();
  snapshot.clearConfirmationUnsynced = clearConfirmationUnsynced_;
  snapshot.clearInProgress = clearRequestIssued_ ||
                             logger.state == SessionLoggerState::Clearing;
  snapshot.clearCompleted = clearCompleted_;
  snapshot.clearSucceeded = clearSucceeded_;
  snapshot.storageCapacityReached = logger.storageCapacityReached;
  snapshot.loggerState = logger.state;
  snapshot.persistentState = storage.persistentSessionState;
  snapshot.syncState = sync.state;
  snapshot.localSynchronizationState = retained.synchronizationState;
  const SessionUiRequestContext requestContext = buildRequestContext(
      logger, storage, retained, sync);
  const SessionUiRequestDecision startDecision =
      SessionUiPolicy::evaluateStart(requestContext);
  const SessionUiRequestDecision stopDecision =
      SessionUiPolicy::evaluateStop(requestContext);
  const SessionUiRequestDecision syncDecision =
      SessionUiPolicy::evaluateSync(requestContext);
  snapshot.startAvailable =
      startDecision == SessionUiRequestDecision::Allowed;
  snapshot.stopAvailable =
      stopDecision == SessionUiRequestDecision::Allowed ||
      ((requestContext.loggerPhase == SessionUiLoggerPhase::Stopping ||
        requestContext.loggerPhase == SessionUiLoggerPhase::Finalizing) &&
       stopDecision == SessionUiRequestDecision::Idempotent);
  snapshot.syncAvailable =
      syncDecision == SessionUiRequestDecision::Allowed ||
      syncDecision == SessionUiRequestDecision::AlreadySynchronized;
  snapshot.clearAvailable = clearConfirmation_.isArmed() ||
      SessionUiPolicy::evaluateClear(requestContext) ==
          SessionUiRequestDecision::Allowed;
  snapshot.message = deriveMessage(logger, storage, sync);
  snapshot.loggerError = logger.lastError;
  snapshot.syncError = sync.lastError;
  snapshot.fifoOccupancy = logger.fifoOccupancy;
  snapshot.fifoHighWaterMark = logger.fifoHighWaterMark;
  snapshot.maximumRecords = storage.maximumRecords;
  snapshot.currentChunk = sync.nextChunk;
  snapshot.totalChunks = sync.chunkCount;
  snapshot.retainedRecords = storage.retainedRecordCount;
  const bool estimatePendingRecords =
      logger.state == SessionLoggerState::Starting ||
      logger.state == SessionLoggerState::PreparingStorage ||
      logger.state == SessionLoggerState::Active ||
      logger.state == SessionLoggerState::Stopping ||
      logger.state == SessionLoggerState::Finalizing;
  if (!estimatePendingRecords ||
      storageUsageSessionId_ != logger.sessionId) {
    storageUsageSessionId_ = logger.sessionId;
    minimumActiveStorageUsage_ = 0U;
  }
  const SessionUiStorageUsage usage =
      SessionUiPolicy::calculateStorageUsage(
          storage.maximumRecords,
          storage.retainedRecordCount,
          logger.acceptedPacketCount,
          storage.storedRecordCount,
          estimatePendingRecords,
          estimatePendingRecords ? minimumActiveStorageUsage_ : 0U);
  if (estimatePendingRecords) {
    minimumActiveStorageUsage_ = usage.effectiveUsedRecords;
  } else {
    minimumActiveStorageUsage_ = 0U;
  }
  snapshot.committedRetainedRecords = usage.committedRetainedRecords;
  snapshot.pendingAcceptedRecords =
      usage.acceptedRecordsNotYetCommitted;
  snapshot.effectiveUsedRecords = usage.effectiveUsedRecords;
  snapshot.remainingRecords = usage.remainingRecords;
  snapshot.storageFilledPercent = usage.filledPercent;
  snapshot.preparationSectorsCompleted =
      logger.preparationSectorsCompleted;
  snapshot.preparationSectorsTotal = logger.preparationSectorsTotal;
  snapshot.preparationPercent = logger.preparationSectorsTotal == 0U
      ? 0U
      : static_cast<uint32_t>(
            (static_cast<uint64_t>(logger.preparationSectorsCompleted) *
             100U) /
            logger.preparationSectorsTotal);
  snapshot.uploadedRecords = sync.uploadedRecords;
  snapshot.droppedRecords = logger.droppedPacketCount;
  return snapshot;
}

SessionUiRequestContext SessionUiController::buildRequestContext(
    const SessionLoggerStatus& logger,
    const SessionStorageStatus& storage,
    const RetainedSessionInfo& retained,
    const SessionSyncStatus& sync) const {
  SessionUiRequestContext context{};
  context.loggerPhase = mapLoggerPhase(logger.state);
  context.storageAvailable = storage.available;
  context.recoveryBlocked = storage.recoveryBlocked;
  context.retainedSessionAvailable = retained.available;
  context.retainedSessionEligibleForSync =
      retained.available && retained.retainedRecordCount > 0U &&
      (retained.finalized || retained.recoveredIncomplete);
  context.synchronized = retained.synchronizationState ==
                         SessionSynchronizationState::Synced;
  context.wifiConnected = sync.wifiConnected;
  context.firebaseEnabled = sync.state != SessionSyncState::Disabled;
  context.syncInProgress = syncOperationActive(sync.state);
  context.clearInProgress = clearRequestIssued_ ||
                             logger.state == SessionLoggerState::Clearing;
  context.startRequestInProgress = startRequestIssued_;
  context.stopRequestInProgress = stopRequestIssued_;
  return context;
}

SessionUiLoggerPhase SessionUiController::mapLoggerPhase(
    SessionLoggerState state) {
  switch (state) {
    case SessionLoggerState::Idle:
      return SessionUiLoggerPhase::Idle;
    case SessionLoggerState::Starting:
      return SessionUiLoggerPhase::Starting;
    case SessionLoggerState::PreparingStorage:
      return SessionUiLoggerPhase::PreparingStorage;
    case SessionLoggerState::Active:
      return SessionUiLoggerPhase::Active;
    case SessionLoggerState::Stopping:
      return SessionUiLoggerPhase::Stopping;
    case SessionLoggerState::Finalizing:
      return SessionUiLoggerPhase::Finalizing;
    case SessionLoggerState::Finalized:
    case SessionLoggerState::RecoveredIncomplete:
      return SessionUiLoggerPhase::Retained;
    case SessionLoggerState::Clearing:
      return SessionUiLoggerPhase::Clearing;
    default:
      return SessionUiLoggerPhase::BusyOrError;
  }
}

bool SessionUiController::syncOperationActive(SessionSyncState state) {
  return state != SessionSyncState::Disabled &&
         state != SessionSyncState::Idle &&
         state != SessionSyncState::Cancelled &&
         state != SessionSyncState::Complete &&
         state != SessionSyncState::Error;
}

bool SessionUiController::syncVerificationActive(SessionSyncState state) {
  switch (state) {
    case SessionSyncState::VerifyingChunk:
    case SessionSyncState::ReadingCompletionManifest:
    case SessionSyncState::CommittingCompletion:
    case SessionSyncState::VerifyingCompletion:
    case SessionSyncState::UpdatingIndex:
    case SessionSyncState::VerifyingIndex:
    case SessionSyncState::PersistingSynced:
      return true;
    default:
      return false;
  }
}

bool SessionUiController::clearArmContextStillMatches(
    const SessionLoggerStatus& logger,
    const SessionStorageStatus& storage,
    const RetainedSessionInfo& retained,
    const SessionSyncStatus& sync) const {
  return logger.state == clearArmLoggerState_ &&
         storage.persistentSessionState == clearArmPersistentState_ &&
         sync.state == clearArmSyncState_ &&
         retained.synchronizationState == clearArmLocalSyncState_ &&
         retained.sessionId == clearArmSessionId_;
}

void SessionUiController::resetClearResult() {
  clearCompleted_ = false;
  clearSucceeded_ = false;
}

SessionUiMessage SessionUiController::deriveMessage(
    const SessionLoggerStatus& logger,
    const SessionStorageStatus& storage,
    const SessionSyncStatus& sync) const {
  // Backend truth always outranks a stale button result. The pure selector is
  // shared with host tests so screen redraws cannot invent state transitions.
  SessionUiStatusContext context{};
  context.loggerPhase = mapLoggerPhase(logger.state);
  context.backendError = !storage.available || storage.recoveryBlocked ||
      logger.state == SessionLoggerState::ErrorIncomplete ||
      logger.state == SessionLoggerState::Error;
  context.recoveredSession =
      logger.state == SessionLoggerState::RecoveredIncomplete;
  context.fifoPending = logger.fifoOccupancy > 0U;
  context.storageCapacityReached = logger.storageCapacityReached;
  context.synchronized = sync.state == SessionSyncState::Complete ||
      storage.synchronizationState == SessionSynchronizationState::Synced ||
      (lastCommand_ == LastCommand::Sync &&
       lastDecision_ == SessionUiRequestDecision::AlreadySynchronized);
  context.clearConfirmationArmed = clearConfirmation_.isArmed();
  context.clearConfirmationUnsynced = clearConfirmationUnsynced_;
  context.clearInProgress = clearRequestIssued_ ||
      logger.state == SessionLoggerState::Clearing;
  context.clearCompleted = clearCompleted_;
  context.clearSucceeded = clearSucceeded_;
  context.syncError = sync.state == SessionSyncState::Error ||
      storage.synchronizationState == SessionSynchronizationState::SyncError;
  context.syncInProgress = syncOperationActive(sync.state);
  context.syncVerifying = syncVerificationActive(sync.state);
  context.wifiConnected = sync.wifiConnected;
  context.startBackendRejected = lastCommand_ == LastCommand::Start &&
      lastDecision_ == SessionUiRequestDecision::BackendRejected;
  context.stopBackendRejected = lastCommand_ == LastCommand::Stop &&
      lastDecision_ == SessionUiRequestDecision::BackendRejected;
  context.syncWifiRejected = lastCommand_ == LastCommand::Sync &&
      lastDecision_ == SessionUiRequestDecision::WifiUnavailable;
  context.syncRejected = lastCommand_ == LastCommand::Sync &&
      lastDecision_ != SessionUiRequestDecision::Allowed &&
      lastDecision_ != SessionUiRequestDecision::Idempotent &&
      lastDecision_ != SessionUiRequestDecision::AlreadySynchronized &&
      lastDecision_ != SessionUiRequestDecision::WifiUnavailable;
  return SessionUiPolicy::selectStatusMessage(context);
}

void SessionUiController::remember(LastCommand command,
                                   SessionUiRequestDecision decision) {
  lastCommand_ = command;
  lastDecision_ = decision;
}

#else

namespace {

SessionUiRequestContext baseContext() {
  SessionUiRequestContext context{};
  context.loggerPhase = SessionUiLoggerPhase::Idle;
  context.storageAvailable = true;
  context.retainedSessionEligibleForSync = true;
  context.wifiConnected = true;
  context.firebaseEnabled = true;
  return context;
}

}  // namespace

int main() {
  SessionUiRequestContext context = baseContext();
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::Allowed);

  context.loggerPhase = SessionUiLoggerPhase::Retained;
  context.retainedSessionAvailable = true;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::RetainedSessionBlocksStart);
  context = baseContext();
  context.loggerPhase = SessionUiLoggerPhase::Active;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::Stopping;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::PreparingStorage;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::Finalizing;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context = baseContext();
  context.startRequestInProgress = true;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::Idempotent);

  context = baseContext();
  context.loggerPhase = SessionUiLoggerPhase::Active;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::Allowed);
  context.loggerPhase = SessionUiLoggerPhase::Stopping;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::Idempotent);
  context.loggerPhase = SessionUiLoggerPhase::Retained;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::Idempotent);
  context.loggerPhase = SessionUiLoggerPhase::Finalizing;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::Idempotent);
  context.loggerPhase = SessionUiLoggerPhase::Idle;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::NoActiveSession);
  context.loggerPhase = SessionUiLoggerPhase::PreparingStorage;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::NoActiveSession);
  context.loggerPhase = SessionUiLoggerPhase::Starting;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::NoActiveSession);
  context = baseContext();
  context.stopRequestInProgress = true;
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::Idempotent);

  context = baseContext();
  context.retainedSessionAvailable = true;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::Allowed);
  context.loggerPhase = SessionUiLoggerPhase::Active;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::PreparingStorage;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::Finalizing;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context = baseContext();
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::NoRetainedSession);
  context.retainedSessionAvailable = true;
  context.synchronized = true;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::AlreadySynchronized);
  context.synchronized = false;
  context.wifiConnected = false;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::WifiUnavailable);
  context.wifiConnected = true;
  context.firebaseEnabled = false;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::FirebaseUnavailable);
  context.firebaseEnabled = true;
  context.syncInProgress = true;
  assert(SessionUiPolicy::evaluateSync(context) ==
         SessionUiRequestDecision::Idempotent);

  context = baseContext();
  context.loggerPhase = SessionUiLoggerPhase::Retained;
  context.retainedSessionAvailable = true;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::Allowed);
  context.synchronized = true;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::Allowed);
  context.loggerPhase = SessionUiLoggerPhase::Active;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::Stopping;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::PreparingStorage;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::Finalizing;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::LoggerBusy);
  context.loggerPhase = SessionUiLoggerPhase::Retained;
  context.syncInProgress = true;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::SyncInProgress);
  context.syncInProgress = false;
  context.clearInProgress = true;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::Idempotent);
  context.clearInProgress = false;
  context.storageAvailable = false;
  assert(SessionUiPolicy::evaluateClear(context) ==
         SessionUiRequestDecision::StorageUnavailable);

  // The first activation only arms. A single successful consume models the
  // one backend request; repeated presses cannot consume it again.
  SessionUiClearConfirmation confirmation;
  confirmation.arm(100U);
  assert(confirmation.isArmed());
  assert(!confirmation.hasExpired(5099U));
  assert(confirmation.consume(5099U));
  assert(!confirmation.isArmed());
  assert(!confirmation.consume(5099U));

  confirmation.arm(100U);
  assert(confirmation.hasExpired(5100U));
  assert(!confirmation.consume(5100U));
  assert(!confirmation.isArmed());

  confirmation.arm(UINT32_MAX - 1000U);
  assert(!confirmation.hasExpired(UINT32_MAX - 500U));
  confirmation.cancel();
  assert(!confirmation.isArmed());

  // Once the retained session is explicitly cleared, Start policy allows a
  // later session again. No implicit Start-side clear is involved.
  context = baseContext();
  context.loggerPhase = SessionUiLoggerPhase::Retained;
  context.retainedSessionAvailable = true;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::RetainedSessionBlocksStart);
  context.loggerPhase = SessionUiLoggerPhase::Idle;
  context.retainedSessionAvailable = false;
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::Allowed);

  SessionUiStorageUsage usage = SessionUiPolicy::calculateStorageUsage(
      2674U, 100U, 135U, 100U, true, 0U);
  assert(usage.committedRetainedRecords == 100U);
  assert(usage.acceptedRecordsNotYetCommitted == 35U);
  assert(usage.effectiveUsedRecords == 135U);
  assert(usage.remainingRecords == 2539U);
  assert(usage.filledPercent == 5U);

  // A worker lease remains counted by accepted minus committed without also
  // adding FIFO occupancy, so committed and pending records cannot overlap.
  usage = SessionUiPolicy::calculateStorageUsage(
      2674U, 108U, 135U, 108U, true, 0U);
  assert(usage.acceptedRecordsNotYetCommitted == 27U);
  assert(usage.effectiveUsedRecords == 135U);

  // The active-session floor keeps visible usage monotonic while the bounded
  // prepared area approaches its explicit capacity stop.
  usage = SessionUiPolicy::calculateStorageUsage(
      2674U, 2660U, 2675U, 2674U, true, 2674U);
  assert(usage.effectiveUsedRecords == 2674U);
  assert(usage.remainingRecords == 0U);
  assert(usage.filledPercent == 100U);

  usage = SessionUiPolicy::calculateStorageUsage(
      2674U, 2674U, UINT64_MAX, 0U, true, 0U);
  assert(usage.effectiveUsedRecords == 2674U);
  assert(usage.remainingRecords == 0U);

  // Finalized/recovered/cleared views ignore volatile accepted counters and
  // use the authoritative retained count immediately.
  usage = SessionUiPolicy::calculateStorageUsage(
      2674U, 90U, 135U, 100U, false, 135U);
  assert(usage.acceptedRecordsNotYetCommitted == 0U);
  assert(usage.effectiveUsedRecords == 90U);
  usage = SessionUiPolicy::calculateStorageUsage(
      2674U, 0U, 135U, 100U, false, 0U);
  assert(usage.effectiveUsedRecords == 0U);
  assert(usage.remainingRecords == 2674U);

  // Message priority is backend error, clear, sync, logger transition, then
  // retained/idle state. Repeated disallowed actions do not replace backend
  // truth such as Logging or Draining.
  SessionUiStatusContext status{};
  status.loggerPhase = SessionUiLoggerPhase::Active;
  status.wifiConnected = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Logging);
  status.startBackendRejected = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Logging);
  status.syncRejected = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Logging);
  status.loggerPhase = SessionUiLoggerPhase::PreparingStorage;
  status.stopBackendRejected = false;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::PreparingStorage);
  status.loggerPhase = SessionUiLoggerPhase::Stopping;
  status.fifoPending = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Draining);
  status.fifoPending = false;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Stopping);
  status.loggerPhase = SessionUiLoggerPhase::Finalizing;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Finalizing);
  status.clearConfirmationArmed = true;
  status.clearConfirmationUnsynced = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::ClearUnsyncedConfirmation);
  status.backendError = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::StorageError);
  status = {};
  status.loggerPhase = SessionUiLoggerPhase::Retained;
  status.storageCapacityReached = true;
  status.synchronized = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::StorageFull);
  status.storageCapacityReached = false;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Synced);
  status.synchronized = false;
  status.recoveredSession = true;
  assert(SessionUiPolicy::selectStatusMessage(status) ==
         SessionUiMessage::Recovered);

  // A stale frame may cause two activations, but the request latch makes the
  // second decision idempotent before any second backend invocation.
  context = baseContext();
  uint32_t modeledStartBackendCalls = 0U;
  if (SessionUiPolicy::evaluateStart(context) ==
      SessionUiRequestDecision::Allowed) {
    ++modeledStartBackendCalls;
    context.startRequestInProgress = true;
  }
  assert(SessionUiPolicy::evaluateStart(context) ==
         SessionUiRequestDecision::Idempotent);
  assert(modeledStartBackendCalls == 1U);
  context = baseContext();
  context.loggerPhase = SessionUiLoggerPhase::Active;
  uint32_t modeledStopBackendCalls = 0U;
  if (SessionUiPolicy::evaluateStop(context) ==
      SessionUiRequestDecision::Allowed) {
    ++modeledStopBackendCalls;
    context.stopRequestInProgress = true;
  }
  assert(SessionUiPolicy::evaluateStop(context) ==
         SessionUiRequestDecision::Idempotent);
  assert(modeledStopBackendCalls == 1U);

  puts("Session UI policy host tests passed");
  return 0;
}

#endif  // !SESSION_UI_CONTROLLER_HOST_TEST
