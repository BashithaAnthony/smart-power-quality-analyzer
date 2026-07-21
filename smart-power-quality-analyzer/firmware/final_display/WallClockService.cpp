#include "WallClockService.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <limits>
#include <sys/time.h>
#include <time.h>

namespace {

constexpr char kPrimaryNtpServer[] = "pool.ntp.org";
constexpr char kSecondaryNtpServer[] = "time.nist.gov";
constexpr char kTertiaryNtpServer[] = "time.google.com";

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

}  // namespace

void WallClockService::begin() {
  portENTER_CRITICAL(&mux_);
  initialized_ = true;
  sntpConfigured_ = false;
  systemClockValid_ = false;
  configurationAttempts_ = 0U;
  nextRetryAtMs_ = 0U;
  portEXIT_CRITICAL(&mux_);
}

void WallClockService::service() {
  portENTER_CRITICAL(&mux_);
  const bool initialized = initialized_;
  const bool alreadyValid = systemClockValid_;
  const uint32_t nextRetryAtMs = nextRetryAtMs_;
  const uint32_t attempts = configurationAttempts_;
  portEXIT_CRITICAL(&mux_);
  if (!initialized) {
    return;
  }

  uint64_t currentEpochMs = 0U;
  if (readSystemUnixEpochMs(currentEpochMs)) {
    if (!alreadyValid) {
      portENTER_CRITICAL(&mux_);
      systemClockValid_ = true;
      portEXIT_CRITICAL(&mux_);
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const uint32_t nowMs = millis();
  if (attempts != 0U && !deadlineReached(nowMs, nextRetryAtMs)) {
    return;
  }

  // configTime starts the ESP-IDF SNTP client and returns without waiting for
  // a reply. UTC is maintained by setting both fixed offsets to zero.
  configTime(0, 0, kPrimaryNtpServer, kSecondaryNtpServer,
             kTertiaryNtpServer);
  const uint32_t nextAttempt = attempts + 1U;
  portENTER_CRITICAL(&mux_);
  sntpConfigured_ = true;
  configurationAttempts_ = nextAttempt;
  nextRetryAtMs_ = nowMs + retryDelayMs(nextAttempt - 1U);
  portEXIT_CRITICAL(&mux_);
}

WallClockSnapshot WallClockService::capture(uint32_t bootId) const {
  const uint64_t beforeUs = static_cast<uint64_t>(esp_timer_get_time());
  struct timeval currentTime{};
  const int result = gettimeofday(&currentTime, nullptr);
  const uint64_t afterUs = static_cast<uint64_t>(esp_timer_get_time());

  WallClockSnapshot snapshot{};
  snapshot.uptimeUs = beforeUs + ((afterUs - beforeUs) / 2U);
  snapshot.bootId = bootId;
  if (result == 0 && currentTime.tv_sec >= 0) {
    snapshot.unixEpochMs =
        static_cast<uint64_t>(currentTime.tv_sec) * 1000U +
        static_cast<uint64_t>(currentTime.tv_usec) / 1000U;
    snapshot.valid = HistoricalTime::isPlausibleUnixEpochMs(
        snapshot.unixEpochMs);
  }
  if (!snapshot.valid) {
    snapshot.unixEpochMs = 0U;
  }
  return snapshot;
}

WallClockServiceStatus WallClockService::getStatus() const {
  WallClockServiceStatus status{};
  portENTER_CRITICAL(&mux_);
  status.initialized = initialized_;
  status.sntpConfigured = sntpConfigured_;
  status.systemClockValid = systemClockValid_;
  status.configurationAttempts = configurationAttempts_;
  const uint32_t nextRetryAtMs = nextRetryAtMs_;
  portEXIT_CRITICAL(&mux_);
  status.wifiConnected = WiFi.status() == WL_CONNECTED;
  status.systemClockValid = readSystemUnixEpochMs(status.currentUnixEpochMs);
  if (status.configurationAttempts > 0U && !status.systemClockValid) {
    const uint32_t nowMs = millis();
    status.nextRetryDelayMs = deadlineReached(nowMs, nextRetryAtMs)
        ? 0U
        : nextRetryAtMs - nowMs;
  }
  return status;
}

bool WallClockService::readSriLankaLocalTime(struct tm& localTime) const {
  uint64_t unixEpochMs = 0U;
  uint64_t localEpochSeconds = 0U;
  if (!readSystemUnixEpochMs(unixEpochMs) ||
      !HistoricalTime::calculateSriLankaDisplayEpochSeconds(
          unixEpochMs, localEpochSeconds) ||
      localEpochSeconds >
          static_cast<uint64_t>(std::numeric_limits<time_t>::max())) {
    return false;
  }

  // Asia/Colombo is UTC+05:30 with no daylight-saving adjustment. Shift only
  // this display copy of the epoch, then use the core's re-entrant UTC calendar
  // conversion. The ESP32 system epoch and all historical anchors remain UTC.
  const time_t displayEpoch = static_cast<time_t>(localEpochSeconds);
  return gmtime_r(&displayEpoch, &localTime) != nullptr;
}

bool WallClockService::readSystemUnixEpochMs(uint64_t& unixEpochMs) {
  unixEpochMs = 0U;
  struct timeval currentTime{};
  if (gettimeofday(&currentTime, nullptr) != 0 || currentTime.tv_sec < 0) {
    return false;
  }
  unixEpochMs = static_cast<uint64_t>(currentTime.tv_sec) * 1000U +
                static_cast<uint64_t>(currentTime.tv_usec) / 1000U;
  if (!HistoricalTime::isPlausibleUnixEpochMs(unixEpochMs)) {
    unixEpochMs = 0U;
    return false;
  }
  return true;
}

uint32_t WallClockService::retryDelayMs(uint32_t attemptIndex) {
  constexpr uint32_t kRetryDelaysMs[] = {
      15000U, 30000U, 60000U, 120000U, 300000U};
  constexpr uint32_t kLastIndex =
      sizeof(kRetryDelaysMs) / sizeof(kRetryDelaysMs[0]) - 1U;
  return kRetryDelaysMs[attemptIndex < kLastIndex
                            ? attemptIndex
                            : kLastIndex];
}
