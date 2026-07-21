#pragma once

#include <stdint.h>

#ifndef WALL_CLOCK_TYPES_HOST_TEST
#define WALL_CLOCK_TYPES_HOST_TEST 0
#endif

constexpr uint64_t WALL_CLOCK_MIN_UNIX_EPOCH_MS = 1704067200000ULL;
constexpr uint64_t WALL_CLOCK_MICROSECONDS_PER_MILLISECOND = 1000ULL;
constexpr uint32_t WALL_CLOCK_SRI_LANKA_UTC_OFFSET_SECONDS =
    (5U * 60U * 60U) + (30U * 60U);

struct WallClockSnapshot {
  uint64_t unixEpochMs;
  uint64_t uptimeUs;
  uint32_t bootId;
  bool valid;
};

enum class HistoricalTimeError : uint8_t {
  None = 0,
  InvalidEpoch,
  CaptureBeforeAnchor,
  BootIdMismatch,
  ArithmeticOverflow
};

class HistoricalTime {
 public:
  static bool isPlausibleUnixEpochMs(uint64_t unixEpochMs);
  static bool isValidSnapshot(const WallClockSnapshot& snapshot);
  static bool isValidEndSnapshot(const WallClockSnapshot& start,
                                 const WallClockSnapshot& end);
  static HistoricalTimeError calculatePacketEpochMs(
      uint64_t sessionStartEpochMs,
      uint64_t sessionStartCaptureTimestampUs,
      uint32_t sessionBootId,
      uint64_t packetCaptureTimestampUs,
      uint32_t packetBootId,
      uint64_t& packetEpochMs);
  static bool calculateSriLankaDisplayEpochSeconds(
      uint64_t unixEpochMs,
      uint64_t& localEpochSeconds);
};
