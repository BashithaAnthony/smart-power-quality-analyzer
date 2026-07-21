#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "SessionSyncUploader.h"

class SessionLogger;
class SessionStorage;

struct LiveTelemetrySnapshot {
  uint32_t seq;
  uint32_t uptimeMs;
  float vRms;
  float iRms;
  float frequency;
  float powerFactor;
  float activePower;
  float apparentPower;
  float reactivePower;
  float crestFactorV;
  float crestFactorI;
  float swellFactor;
  float thdV;
  float thdI;
  bool logging;
  bool wifiConnected;
  int32_t wifiRssi;
};

class FirebaseBridge {
 public:
  bool begin();
  bool begin(SessionStorage& storage, SessionLogger& logger);
  void submit(const LiveTelemetrySnapshot& snapshot);
  bool requestSessionUpload();
  void cancelSessionUpload();
  SessionSyncStatus getSessionSyncStatus() const;
  static const char* deviceId();
  bool isAuthenticated();
  int lastHttpStatus();

 private:
  friend class SessionSyncUploader;

  static void workerTaskEntry(void* context);
  void workerTask();

  bool ensureAuthenticated();
  bool signInWithPassword();
  bool refreshIdToken();
  bool publish(const LiveTelemetrySnapshot& snapshot);
  bool performDatabaseRequest(
      FirebaseDatabaseMethod method,
      const char* relativePath,
      const uint8_t* requestBody,
      size_t requestLength,
      const FirebaseDatabaseRequestOptions& options,
      char* responseBody,
      size_t responseCapacity,
      FirebaseDatabaseResponse& response);
  bool tokenNeedsRefresh() const;
  bool servicePendingLiveTelemetry();
  void drainLatestSnapshot();
  void notifyWorker(uint32_t notificationBits);
  void noteRetry();
  void resetRetry();

  QueueHandle_t snapshotQueue_ = nullptr;
  TaskHandle_t workerTaskHandle_ = nullptr;
  volatile bool started_ = false;
  volatile bool authenticated_ = false;
  volatile int lastHttpStatus_ = 0;

  SessionStorage* sessionStorage_ = nullptr;
  SessionLogger* sessionLogger_ = nullptr;
  SessionSyncUploader sessionSyncUploader_;

  String idToken_;
  String refreshToken_;
  uint32_t tokenIssuedAtMs_ = 0;
  uint32_t tokenLifetimeSeconds_ = 0;
  uint32_t lastPublishMs_ = 0;
  uint32_t nextLiveAttemptMs_ = 0;
  uint32_t retryDelayMs_ = 1000;
  LiveTelemetrySnapshot pendingSnapshot_{};
  bool liveSnapshotPending_ = false;
};
