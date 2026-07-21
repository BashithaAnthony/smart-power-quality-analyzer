#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "RamPacketFifo.h"
#include "SessionStorage.h"
#include "SessionTypes.h"
#include "WallClockService.h"

class SessionLogger {
 public:
  bool begin(SessionStorage& storage, WallClockService& wallClock);
  bool startSession(uint32_t flushIntervalSeconds);
  bool submitValidatedPacket(const uint8_t* packetBytes,
                             size_t packetLength,
                             uint32_t stm32Sequence,
                             uint64_t captureTimestampUs);
  bool stopSession();
  bool clearRetainedSession();
  bool rescanRetainedStorage();

  bool isActive() const;
  SessionLoggerStatus getStatus() const;

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  SessionFlushDiagnostics getFlushDiagnostics() const;
#endif

  // Read-only RAM-FIFO test support. Do not call these while the storage
  // worker is consuming records or while a retained-session clear runs.
  bool getRecordMetadata(uint32_t ordinal,
                         RamPacketRecordMetadata& metadata) const;
  bool copyPacketBytes(uint32_t ordinal,
                       uint8_t* destination,
                       size_t destinationLength) const;
  bool packetBytesEqual(uint32_t ordinal,
                        const uint8_t* expected,
                        size_t expectedLength) const;

#if SESSION_LOGGER_DEBUG
  void printDebugSummary() const;
#endif

 private:
  static void workerTaskEntry(void* context);
  void workerTask();
  void handleStartRequest();
  void handleClearRequest();
  void handleRescanRequest();
  bool drainRecords(uint32_t recordLimit);
  void finishStopIfDrained();
  void transitionToStorageError(SessionStorageError error);
  void synchronizeRecoveredState(const SessionStorageStatus& storageStatus);
  void notifyWorker();
  void releaseWorkerBuffers();
  static bool isSupportedInterval(uint32_t seconds);
  static SessionLoggerError mapStorageError(SessionStorageError error);
  void setError(SessionLoggerError error);
  void clearSessionCountersLocked();
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  void beginFlushDiagnostics(uint32_t recordsRequested);
  void completeFlushDiagnostics(bool successful);
  void noteFlushRecordWritten();
#endif

  RamPacketFifo fifo_;
  SessionStorage* storage_ = nullptr;
  WallClockService* wallClock_ = nullptr;
  TaskHandle_t workerTaskHandle_ = nullptr;
  uint8_t* packetCopyBuffer_ = nullptr;
  uint8_t* encodedRecordBuffer_ = nullptr;
  uint8_t* readbackBuffer_ = nullptr;
  bool initialized_ = false;
  bool psramAvailable_ = false;
  bool stopDrainComplete_ = false;
  bool earlyFlushNotified_ = false;
  SessionLoggerState state_ = SessionLoggerState::Disabled;
  SessionLoggerError lastError_ = SessionLoggerError::None;
  uint64_t sessionId_ = 0;
  uint64_t nextGlobalLogicalRecordIndex_ = 0;
  uint32_t selectedIntervalSeconds_ = 0;
  uint32_t nextFlushDeadlineMs_ = 0;
  uint32_t bootId_ = 0;
  uint32_t producerInFlight_ = 0;
  bool stoppingStateApplied_ = false;
  bool flushCycleActive_ = false;
  bool storageCapacityReached_ = false;
  uint64_t validPacketsOffered_ = 0;
  uint64_t acceptedPacketCount_ = 0;
  uint64_t droppedPacketCount_ = 0;
  uint64_t invalidLengthRejectionCount_ = 0;
  uint32_t firstStm32Sequence_ = 0;
  uint32_t lastStm32Sequence_ = 0;
  uint64_t firstLogicalRecordIndex_ = 0;
  uint64_t lastLogicalRecordIndex_ = 0;
  bool hasRecords_ = false;
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  SessionFlushDiagnostics flushDiagnostics_{};
  uint32_t erasedSegmentsAtFlushStart_ = 0U;
#endif
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
