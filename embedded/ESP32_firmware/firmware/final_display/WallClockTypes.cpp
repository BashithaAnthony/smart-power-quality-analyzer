#include "WallClockTypes.h"

#include <limits.h>

bool HistoricalTime::isPlausibleUnixEpochMs(uint64_t unixEpochMs) {
  return unixEpochMs >= WALL_CLOCK_MIN_UNIX_EPOCH_MS;
}

bool HistoricalTime::isValidSnapshot(const WallClockSnapshot& snapshot) {
  if (snapshot.bootId == 0U) {
    return false;
  }
  if (!snapshot.valid) {
    return snapshot.unixEpochMs == 0U;
  }
  return isPlausibleUnixEpochMs(snapshot.unixEpochMs);
}

bool HistoricalTime::isValidEndSnapshot(const WallClockSnapshot& start,
                                        const WallClockSnapshot& end) {
  if (!isValidSnapshot(start) || !isValidSnapshot(end) ||
      end.uptimeUs < start.uptimeUs) {
    return false;
  }
  if (!end.valid) {
    return true;
  }
  if (start.bootId != end.bootId) {
    return false;
  }
  return !start.valid || end.unixEpochMs >= start.unixEpochMs;
}

HistoricalTimeError HistoricalTime::calculatePacketEpochMs(
    uint64_t sessionStartEpochMs,
    uint64_t sessionStartCaptureTimestampUs,
    uint32_t sessionBootId,
    uint64_t packetCaptureTimestampUs,
    uint32_t packetBootId,
    uint64_t& packetEpochMs) {
  packetEpochMs = 0U;
  if (!isPlausibleUnixEpochMs(sessionStartEpochMs)) {
    return HistoricalTimeError::InvalidEpoch;
  }
  if (sessionBootId == 0U || packetBootId != sessionBootId) {
    return HistoricalTimeError::BootIdMismatch;
  }
  if (packetCaptureTimestampUs < sessionStartCaptureTimestampUs) {
    return HistoricalTimeError::CaptureBeforeAnchor;
  }
  const uint64_t elapsedMs =
      (packetCaptureTimestampUs - sessionStartCaptureTimestampUs) /
      WALL_CLOCK_MICROSECONDS_PER_MILLISECOND;
  if (sessionStartEpochMs > UINT64_MAX - elapsedMs) {
    return HistoricalTimeError::ArithmeticOverflow;
  }
  packetEpochMs = sessionStartEpochMs + elapsedMs;
  return HistoricalTimeError::None;
}

bool HistoricalTime::calculateSriLankaDisplayEpochSeconds(
    uint64_t unixEpochMs,
    uint64_t& localEpochSeconds) {
  localEpochSeconds = 0U;
  if (!isPlausibleUnixEpochMs(unixEpochMs)) {
    return false;
  }
  const uint64_t utcEpochSeconds = unixEpochMs / 1000U;
  if (utcEpochSeconds >
      UINT64_MAX - WALL_CLOCK_SRI_LANKA_UTC_OFFSET_SECONDS) {
    return false;
  }
  localEpochSeconds =
      utcEpochSeconds + WALL_CLOCK_SRI_LANKA_UTC_OFFSET_SECONDS;
  return true;
}

#if WALL_CLOCK_TYPES_HOST_TEST

#include <stdio.h>
#include <time.h>

namespace {

uint32_t failures = 0U;

void expect(bool condition, const char* name) {
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", name);
  }
}

}  // namespace

int main() {
  expect(!HistoricalTime::isPlausibleUnixEpochMs(0U),
         "clock invalid before synchronization");
  expect(!HistoricalTime::isPlausibleUnixEpochMs(
             WALL_CLOCK_MIN_UNIX_EPOCH_MS - 1U) &&
             HistoricalTime::isPlausibleUnixEpochMs(
                 WALL_CLOCK_MIN_UNIX_EPOCH_MS),
         "plausible epoch boundary");

  const WallClockSnapshot invalid{0U, 100U, 7U, false};
  const WallClockSnapshot validStart{
      WALL_CLOCK_MIN_UNIX_EPOCH_MS, 2000000U, 7U, true};
  const WallClockSnapshot validEnd{
      WALL_CLOCK_MIN_UNIX_EPOCH_MS + 2500U, 4500000U, 7U, true};
  expect(HistoricalTime::isValidSnapshot(invalid), "invalid anchor encoding");
  expect(HistoricalTime::isValidSnapshot(validStart), "valid start anchor");
  expect(!HistoricalTime::isValidSnapshot(
             {WALL_CLOCK_MIN_UNIX_EPOCH_MS - 1U, 2000000U, 7U, true}),
         "invalid start anchor");
  expect(HistoricalTime::isValidEndSnapshot(validStart, validEnd),
         "valid end anchor");
  expect(!HistoricalTime::isValidEndSnapshot(
             validStart,
             {WALL_CLOCK_MIN_UNIX_EPOCH_MS - 1U, 4500000U, 7U, true}),
         "invalid end anchor");

  uint64_t packetEpochMs = 0U;
  expect(HistoricalTime::calculatePacketEpochMs(
             validStart.unixEpochMs,
             validStart.uptimeUs,
             validStart.bootId,
             validStart.uptimeUs + 1234567U,
             validStart.bootId,
             packetEpochMs) == HistoricalTimeError::None &&
             packetEpochMs == validStart.unixEpochMs + 1234U,
         "microseconds to milliseconds conversion");
  expect(HistoricalTime::calculatePacketEpochMs(
             validStart.unixEpochMs,
             validStart.uptimeUs,
             validStart.bootId,
             validStart.uptimeUs + 1000U,
             validStart.bootId + 1U,
             packetEpochMs) == HistoricalTimeError::BootIdMismatch,
         "boot ID consistency");
  expect(HistoricalTime::calculatePacketEpochMs(
             validStart.unixEpochMs,
             validStart.uptimeUs,
             validStart.bootId,
             validStart.uptimeUs - 1U,
             validStart.bootId,
             packetEpochMs) == HistoricalTimeError::CaptureBeforeAnchor,
         "capture before anchor rejection");

  uint64_t localEpochSeconds = 0U;
  expect(HistoricalTime::calculateSriLankaDisplayEpochSeconds(
             1704067200000ULL,
             localEpochSeconds) &&
             localEpochSeconds == 1704087000ULL,
         "Sri Lanka local time at UTC midnight");
  time_t localCalendarSeconds = static_cast<time_t>(localEpochSeconds);
  const struct tm* localCalendar = gmtime(&localCalendarSeconds);
  expect(localCalendar != nullptr && localCalendar->tm_year == 124 &&
             localCalendar->tm_mon == 0 && localCalendar->tm_mday == 1 &&
             localCalendar->tm_hour == 5 && localCalendar->tm_min == 30,
         "Sri Lanka UTC midnight formats as 2024-01-01 05:30");
  expect(HistoricalTime::calculateSriLankaDisplayEpochSeconds(
             1704139200000ULL,
             localEpochSeconds) &&
             localEpochSeconds == 1704159000ULL,
         "Sri Lanka local date crosses after UTC 20:00");
  localCalendarSeconds = static_cast<time_t>(localEpochSeconds);
  localCalendar = gmtime(&localCalendarSeconds);
  expect(localCalendar != nullptr && localCalendar->tm_year == 124 &&
             localCalendar->tm_mon == 0 && localCalendar->tm_mday == 2 &&
             localCalendar->tm_hour == 1 && localCalendar->tm_min == 30,
         "Sri Lanka conversion crosses to 2024-01-02 01:30");
  expect(!HistoricalTime::calculateSriLankaDisplayEpochSeconds(
             0U,
             localEpochSeconds) &&
             localEpochSeconds == 0U,
         "invalid clock cannot produce local display time");

  if (failures == 0U) {
    printf("WallClockTypes host tests: PASS\n");
    return 0;
  }
  printf("WallClockTypes host tests: FAIL (%lu)\n",
         static_cast<unsigned long>(failures));
  return 1;
}

#endif
