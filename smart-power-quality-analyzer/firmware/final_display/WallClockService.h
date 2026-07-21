#pragma once

#include <stdint.h>
#include <time.h>
#include <freertos/FreeRTOS.h>

#include "WallClockTypes.h"

struct WallClockServiceStatus {
  bool initialized;
  bool sntpConfigured;
  bool systemClockValid;
  bool wifiConnected;
  uint32_t configurationAttempts;
  uint32_t nextRetryDelayMs;
  uint64_t currentUnixEpochMs;
};

class WallClockService {
 public:
  void begin();
  void service();
  WallClockSnapshot capture(uint32_t bootId) const;
  WallClockServiceStatus getStatus() const;
  bool readSriLankaLocalTime(struct tm& localTime) const;

 private:
  static bool readSystemUnixEpochMs(uint64_t& unixEpochMs);
  static uint32_t retryDelayMs(uint32_t attemptIndex);

  bool initialized_ = false;
  bool sntpConfigured_ = false;
  bool systemClockValid_ = false;
  uint32_t configurationAttempts_ = 0U;
  uint32_t nextRetryAtMs_ = 0U;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
