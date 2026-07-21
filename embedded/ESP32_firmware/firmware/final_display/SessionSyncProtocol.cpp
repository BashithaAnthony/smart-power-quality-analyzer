#include "SessionSyncProtocol.h"

#include <limits.h>
#include <string.h>

namespace {

constexpr uint32_t kCanonicalMagicOffset = 0U;
constexpr uint32_t kCanonicalVersionOffset = 4U;
constexpr uint32_t kCanonicalLengthOffset = 6U;
constexpr uint32_t kCanonicalSchemaOffset = 8U;
constexpr uint32_t kCanonicalDeviceIdOffset = 12U;
constexpr uint32_t kCanonicalSessionIdOffset = 28U;
constexpr uint32_t kCanonicalPersistentStateOffset = 36U;
constexpr uint32_t kCanonicalFlagsOffset = 37U;
constexpr uint32_t kCanonicalReservedOffset = 38U;
constexpr uint32_t kCanonicalRecordFormatOffset = 40U;
constexpr uint32_t kCanonicalRecordSizeOffset = 44U;
constexpr uint32_t kCanonicalRecordsPerChunkOffset = 48U;
constexpr uint32_t kCanonicalChunkCountOffset = 52U;
constexpr uint32_t kCanonicalRetainedCountOffset = 56U;
constexpr uint32_t kCanonicalTotalStoredOffset = 64U;
constexpr uint32_t kCanonicalOverwrittenCountOffset = 72U;
constexpr uint32_t kCanonicalFirstLogicalOffset = 80U;
constexpr uint32_t kCanonicalLastLogicalOffset = 88U;
constexpr uint32_t kCanonicalFirstStm32Offset = 96U;
constexpr uint32_t kCanonicalLastStm32Offset = 100U;
constexpr uint32_t kCanonicalSourceGenerationOffset = 104U;
constexpr uint32_t kCanonicalSessionTimeValidOffset = 112U;
constexpr uint32_t kCanonicalSessionEndTimeValidOffset = 113U;
constexpr uint32_t kCanonicalTimeSourceOffset = 114U;
constexpr uint32_t kCanonicalTimeReservedOffset = 115U;
constexpr uint32_t kCanonicalSessionBootIdOffset = 116U;
constexpr uint32_t kCanonicalSessionStartEpochOffset = 120U;
constexpr uint32_t kCanonicalSessionStartCaptureOffset = 128U;
constexpr uint32_t kCanonicalSessionEndEpochOffset = 136U;

static_assert(kCanonicalDeviceIdOffset + SESSION_SYNC_DEVICE_ID_BYTES ==
                  kCanonicalSessionIdOffset,
              "Canonical device ID must occupy exactly 16 bytes");
static_assert(kCanonicalSourceGenerationOffset + sizeof(uint64_t) ==
                  SESSION_SYNC_LEGACY_MANIFEST_CANONICAL_BYTES,
              "Legacy canonical source generation must be the final field");
static_assert(kCanonicalSessionEndEpochOffset + sizeof(uint64_t) ==
                  SESSION_SYNC_MANIFEST_CANONICAL_BYTES,
              "Time-aware canonical end epoch must be the final field");

constexpr uint16_t kLegacyCanonicalFormatVersion = 1U;
constexpr uint16_t kTimeAwareCanonicalFormatVersion = 2U;
constexpr uint8_t kManifestFlagTruncated = 1U << 0U;
constexpr uint8_t kManifestFlagRecoveredIncomplete = 1U << 1U;
constexpr uint8_t kManifestFlagCountersPartial = 1U << 2U;
constexpr uint8_t kManifestKnownFlags =
    kManifestFlagTruncated | kManifestFlagRecoveredIncomplete |
    kManifestFlagCountersPartial;

constexpr uint32_t kCrc32cReflectedPolynomial = 0x82F63B78U;
constexpr uint32_t kCrc32cInitialValue = 0xFFFFFFFFU;
constexpr uint32_t kCrc32cFinalXor = 0xFFFFFFFFU;

void writeLe16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

void writeLe64(uint8_t* destination, uint64_t value) {
  for (uint8_t index = 0U; index < 8U; ++index) {
    destination[index] =
        static_cast<uint8_t>(value >> (static_cast<uint32_t>(index) * 8U));
  }
}

bool fixedStringIsCanonical(const char* value, uint32_t capacity,
                            bool allowEmpty) {
  if (value == nullptr || capacity == 0U) {
    return false;
  }
  uint32_t terminator = capacity;
  for (uint32_t index = 0U; index < capacity; ++index) {
    if (value[index] == '\0') {
      terminator = index;
      break;
    }
  }
  if (terminator == capacity || (!allowEmpty && terminator == 0U)) {
    return false;
  }
  for (uint32_t index = terminator + 1U; index < capacity; ++index) {
    if (value[index] != '\0') {
      return false;
    }
  }
  return true;
}

bool recordFormatIsValid(const char recordFormat[5]) {
  return recordFormat != nullptr && recordFormat[0] == 'P' &&
         recordFormat[1] == 'Q' && recordFormat[2] == 'R' &&
         recordFormat[3] == '1' && recordFormat[4] == '\0';
}

bool persistentStateIsValid(SyncManifestPersistentState state) {
  return state == SyncManifestPersistentState::Finalized ||
         state == SyncManifestPersistentState::RecoveredIncomplete;
}

SessionSyncProtocolError validateManifest(
    const SyncManifestImmutable& manifest) {
  const bool legacySchema =
      manifest.schemaVersion == SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION;
  const bool currentSchema =
      manifest.schemaVersion == SESSION_SYNC_SCHEMA_VERSION;
  if ((!legacySchema && !currentSchema) ||
      !fixedStringIsCanonical(
          manifest.deviceId, SESSION_SYNC_DEVICE_ID_BYTES, false) ||
      manifest.sessionId == 0U ||
      !persistentStateIsValid(manifest.persistentState) ||
      !recordFormatIsValid(manifest.recordFormat) ||
      manifest.recordSize != SESSION_SYNC_RECORD_BYTES ||
      manifest.recordsPerChunk != SESSION_SYNC_RECORDS_PER_CHUNK ||
      manifest.retainedCount == 0U || manifest.totalStored < manifest.retainedCount ||
      manifest.overwrittenCount > manifest.totalStored ||
      manifest.lastLogicalIndex < manifest.firstLogicalIndex ||
      manifest.lastLogicalIndex - manifest.firstLogicalIndex !=
          manifest.retainedCount - 1U ||
      manifest.sourceMetadataGeneration == 0U ||
      (manifest.persistentState ==
           SyncManifestPersistentState::RecoveredIncomplete) !=
          manifest.recoveredIncomplete) {
      return SessionSyncProtocolError::InvalidManifest;
  }

  const bool legacyTimeFieldsValid =
      !manifest.sessionTimeValid && !manifest.sessionEndTimeValid &&
      manifest.timeSource == SyncManifestTimeSource::None &&
      manifest.sessionBootId == 0U &&
      manifest.sessionStartEpochMs == 0U &&
      manifest.sessionStartCaptureTimestampUs == 0U &&
      manifest.sessionEndEpochMs == 0U;
  const bool currentTimeFieldsValid =
      manifest.timeSource == SyncManifestTimeSource::Ntp &&
      manifest.sessionBootId != 0U &&
      (manifest.sessionTimeValid
           ? manifest.sessionStartEpochMs >= SESSION_SYNC_MIN_UNIX_EPOCH_MS
           : manifest.sessionStartEpochMs == 0U) &&
      (manifest.sessionEndTimeValid
           ? manifest.sessionEndEpochMs >= SESSION_SYNC_MIN_UNIX_EPOCH_MS
           : manifest.sessionEndEpochMs == 0U) &&
      (!manifest.sessionTimeValid || !manifest.sessionEndTimeValid ||
       manifest.sessionEndEpochMs >= manifest.sessionStartEpochMs);
  if ((legacySchema && !legacyTimeFieldsValid) ||
      (currentSchema && !currentTimeFieldsValid)) {
    return SessionSyncProtocolError::InvalidManifest;
  }

  uint32_t calculatedChunkCount = 0U;
  const SessionSyncProtocolError countResult =
      SessionSyncProtocol::calculateChunkCount(
          manifest.retainedCount, calculatedChunkCount);
  if (countResult != SessionSyncProtocolError::Ok ||
      calculatedChunkCount != manifest.chunkCount) {
    return SessionSyncProtocolError::InvalidManifest;
  }
  return SessionSyncProtocolError::Ok;
}

bool immutableBaseMatches(const SyncManifestImmutable& expected,
                          const SyncManifestImmutable& candidate) {
  return memcmp(expected.deviceId, candidate.deviceId,
                SESSION_SYNC_DEVICE_ID_BYTES) == 0 &&
         expected.sessionId == candidate.sessionId &&
         expected.persistentState == candidate.persistentState &&
         expected.truncated == candidate.truncated &&
         expected.recoveredIncomplete == candidate.recoveredIncomplete &&
         expected.countersPartial == candidate.countersPartial &&
         memcmp(expected.recordFormat, candidate.recordFormat, 5U) == 0 &&
         expected.recordSize == candidate.recordSize &&
         expected.recordsPerChunk == candidate.recordsPerChunk &&
         expected.chunkCount == candidate.chunkCount &&
         expected.retainedCount == candidate.retainedCount &&
         expected.totalStored == candidate.totalStored &&
         expected.overwrittenCount == candidate.overwrittenCount &&
         expected.firstLogicalIndex == candidate.firstLogicalIndex &&
         expected.lastLogicalIndex == candidate.lastLogicalIndex &&
         expected.firstStm32Sequence == candidate.firstStm32Sequence &&
         expected.lastStm32Sequence == candidate.lastStm32Sequence &&
         expected.sourceMetadataGeneration ==
             candidate.sourceMetadataGeneration;
}

bool cloudManifestMatches(const SyncManifestImmutable& local,
                          const SyncParsedCloudManifest& cloud) {
  uint32_t expectedCrc = 0U;
  if (local.schemaVersion == cloud.immutable.schemaVersion) {
    return SessionSyncProtocol::calculateManifestCrc32c(
               local, expectedCrc) == SessionSyncProtocolError::Ok &&
           cloud.manifestCrc32c == expectedCrc &&
           SessionSyncProtocol::immutableManifestMatches(
               local, cloud.immutable);
  }

  // A retained session created before NTP support has no valid wall-clock
  // anchors. It may already have a schema-1 cloud manifest. Accept that exact
  // legacy identity for resume/idempotency, but never discard a valid local
  // anchor to make an old cloud manifest match.
  if (local.schemaVersion == SESSION_SYNC_SCHEMA_VERSION &&
      cloud.immutable.schemaVersion ==
          SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION &&
      !local.sessionTimeValid && !local.sessionEndTimeValid &&
      immutableBaseMatches(local, cloud.immutable) &&
      SessionSyncProtocol::calculateManifestCrc32c(
          cloud.immutable, expectedCrc) == SessionSyncProtocolError::Ok) {
    return cloud.manifestCrc32c == expectedCrc;
  }
  return false;
}

}  // namespace

SessionSyncProtocolError SessionSyncProtocol::formatUint64Decimal(
    uint64_t value, char* destination, uint32_t destinationLength,
    uint32_t& writtenLength) {
  writtenLength = 0U;
  if (destination == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }

  char reversed[20]{};
  uint32_t digitCount = 0U;
  do {
    reversed[digitCount++] =
        static_cast<char>('0' + static_cast<uint8_t>(value % 10U));
    value /= 10U;
  } while (value != 0U);

  if (destinationLength <= digitCount) {
    if (destinationLength > 0U) {
      destination[0] = '\0';
    }
    return SessionSyncProtocolError::BufferTooShort;
  }
  for (uint32_t index = 0U; index < digitCount; ++index) {
    destination[index] = reversed[digitCount - 1U - index];
  }
  destination[digitCount] = '\0';
  writtenLength = digitCount;
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::parseUint64Decimal(
    const char* source, uint32_t sourceLength, uint64_t& value) {
  value = 0U;
  if (source == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  if (sourceLength == 0U || sourceLength > 20U ||
      (sourceLength > 1U && source[0] == '0')) {
    return SessionSyncProtocolError::InvalidDecimal;
  }

  uint64_t parsed = 0U;
  for (uint32_t index = 0U; index < sourceLength; ++index) {
    const char character = source[index];
    if (character < '0' || character > '9') {
      return SessionSyncProtocolError::InvalidDecimal;
    }
    const uint8_t digit = static_cast<uint8_t>(character - '0');
    if (parsed > (UINT64_MAX - digit) / 10U) {
      return SessionSyncProtocolError::DecimalOverflow;
    }
    parsed = parsed * 10U + digit;
  }
  value = parsed;
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::formatSessionKey(
    uint64_t sessionId, char* destination, uint32_t destinationLength) {
  if (destination == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  if (destinationLength < 4U) {
    if (destinationLength > 0U) {
      destination[0] = '\0';
    }
    return SessionSyncProtocolError::BufferTooShort;
  }
  char decimal[21]{};
  uint32_t decimalLength = 0U;
  const SessionSyncProtocolError result = formatUint64Decimal(
      sessionId, decimal, sizeof(decimal), decimalLength);
  if (result != SessionSyncProtocolError::Ok) {
    destination[0] = '\0';
    return result;
  }
  if (destinationLength < decimalLength + 3U) {
    destination[0] = '\0';
    return SessionSyncProtocolError::BufferTooShort;
  }
  destination[0] = 's';
  destination[1] = '_';
  memcpy(destination + 2U, decimal, decimalLength + 1U);
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::formatChunkKey(
    uint32_t chunkIndex, char* destination, uint32_t destinationLength) {
  if (destination == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  if (destinationLength < SESSION_SYNC_CHUNK_KEY_BYTES) {
    if (destinationLength > 0U) {
      destination[0] = '\0';
    }
    return SessionSyncProtocolError::BufferTooShort;
  }
  if (chunkIndex > SESSION_SYNC_MAX_CHUNK_INDEX) {
    destination[0] = '\0';
    return SessionSyncProtocolError::ValueOutOfRange;
  }
  uint32_t remainder = chunkIndex;
  for (int32_t index = 5; index >= 0; --index) {
    destination[index] = static_cast<char>('0' + (remainder % 10U));
    remainder /= 10U;
  }
  destination[6] = '\0';
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::formatCrc32cHex(
    uint32_t crc, char* destination, uint32_t destinationLength) {
  static const char kHex[] = "0123456789ABCDEF";
  if (destination == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  if (destinationLength < 9U) {
    if (destinationLength > 0U) {
      destination[0] = '\0';
    }
    return SessionSyncProtocolError::BufferTooShort;
  }
  for (uint32_t index = 0U; index < 8U; ++index) {
    const uint32_t shift = (7U - index) * 4U;
    destination[index] = kHex[(crc >> shift) & 0x0FU];
  }
  destination[8] = '\0';
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::parseCrc32cHex(
    const char* source, uint32_t sourceLength, uint32_t& crc) {
  crc = 0U;
  if (source == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  if (sourceLength != 8U) {
    return SessionSyncProtocolError::ValueOutOfRange;
  }
  uint32_t parsed = 0U;
  for (uint32_t index = 0U; index < sourceLength; ++index) {
    const char character = source[index];
    uint8_t nibble = 0U;
    if (character >= '0' && character <= '9') {
      nibble = static_cast<uint8_t>(character - '0');
    } else if (character >= 'A' && character <= 'F') {
      nibble = static_cast<uint8_t>(character - 'A' + 10);
    } else {
      return SessionSyncProtocolError::ValueOutOfRange;
    }
    parsed = (parsed << 4U) | nibble;
  }
  crc = parsed;
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::base64EncodedLength(
    uint32_t inputLength, uint32_t& encodedLength) {
  const uint64_t groups = (static_cast<uint64_t>(inputLength) + 2U) / 3U;
  const uint64_t calculated = groups * 4U;
  if (calculated > UINT32_MAX) {
    encodedLength = 0U;
    return SessionSyncProtocolError::ArithmeticOverflow;
  }
  encodedLength = static_cast<uint32_t>(calculated);
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::base64Encode(
    const uint8_t* source, uint32_t sourceLength, char* destination,
    uint32_t destinationLength, uint32_t& writtenLength) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  writtenLength = 0U;
  if ((source == nullptr && sourceLength != 0U) || destination == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  uint32_t required = 0U;
  const SessionSyncProtocolError lengthResult =
      base64EncodedLength(sourceLength, required);
  if (lengthResult != SessionSyncProtocolError::Ok) {
    return lengthResult;
  }
  if (destinationLength <= required) {
    if (destinationLength > 0U) {
      destination[0] = '\0';
    }
    return SessionSyncProtocolError::BufferTooShort;
  }

  uint32_t input = 0U;
  uint32_t output = 0U;
  while (sourceLength - input >= 3U) {
    const uint32_t value =
        (static_cast<uint32_t>(source[input]) << 16U) |
        (static_cast<uint32_t>(source[input + 1U]) << 8U) |
        static_cast<uint32_t>(source[input + 2U]);
    destination[output++] = kAlphabet[(value >> 18U) & 0x3FU];
    destination[output++] = kAlphabet[(value >> 12U) & 0x3FU];
    destination[output++] = kAlphabet[(value >> 6U) & 0x3FU];
    destination[output++] = kAlphabet[value & 0x3FU];
    input += 3U;
  }
  const uint32_t remaining = sourceLength - input;
  if (remaining == 1U) {
    const uint32_t value = static_cast<uint32_t>(source[input]) << 16U;
    destination[output++] = kAlphabet[(value >> 18U) & 0x3FU];
    destination[output++] = kAlphabet[(value >> 12U) & 0x3FU];
    destination[output++] = '=';
    destination[output++] = '=';
  } else if (remaining == 2U) {
    const uint32_t value =
        (static_cast<uint32_t>(source[input]) << 16U) |
        (static_cast<uint32_t>(source[input + 1U]) << 8U);
    destination[output++] = kAlphabet[(value >> 18U) & 0x3FU];
    destination[output++] = kAlphabet[(value >> 12U) & 0x3FU];
    destination[output++] = kAlphabet[(value >> 6U) & 0x3FU];
    destination[output++] = '=';
  }
  destination[output] = '\0';
  writtenLength = output;
  return SessionSyncProtocolError::Ok;
}

uint32_t SessionSyncProtocol::crc32c(const uint8_t* data,
                                     uint32_t length) {
  if (data == nullptr && length != 0U) {
    return 0U;
  }
  uint32_t crc = kCrc32cInitialValue;
  for (uint32_t byteIndex = 0U; byteIndex < length; ++byteIndex) {
    crc ^= data[byteIndex];
    for (uint8_t bitIndex = 0U; bitIndex < 8U; ++bitIndex) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (kCrc32cReflectedPolynomial & mask);
    }
  }
  return crc ^ kCrc32cFinalXor;
}

SessionSyncProtocolError SessionSyncProtocol::calculateChunkCount(
    uint64_t retainedCount, uint32_t& chunkCount) {
  const uint64_t calculated =
      retainedCount / SESSION_SYNC_RECORDS_PER_CHUNK +
      ((retainedCount % SESSION_SYNC_RECORDS_PER_CHUNK) != 0U ? 1U : 0U);
  if (calculated > static_cast<uint64_t>(SESSION_SYNC_MAX_CHUNK_INDEX) + 1U) {
    chunkCount = 0U;
    return SessionSyncProtocolError::ValueOutOfRange;
  }
  chunkCount = static_cast<uint32_t>(calculated);
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::calculateChunkBounds(
    uint64_t retainedCount, uint32_t chunkIndex, SyncChunkBounds& bounds) {
  bounds = SyncChunkBounds{};
  uint32_t chunkCount = 0U;
  const SessionSyncProtocolError countResult =
      calculateChunkCount(retainedCount, chunkCount);
  if (countResult != SessionSyncProtocolError::Ok) {
    return countResult;
  }
  if (chunkIndex >= chunkCount) {
    return SessionSyncProtocolError::ValueOutOfRange;
  }
  const uint64_t first =
      static_cast<uint64_t>(chunkIndex) * SESSION_SYNC_RECORDS_PER_CHUNK;
  const uint64_t remaining = retainedCount - first;
  const uint32_t records = static_cast<uint32_t>(
      remaining < SESSION_SYNC_RECORDS_PER_CHUNK
          ? remaining
          : SESSION_SYNC_RECORDS_PER_CHUNK);
  bounds.chunkIndex = chunkIndex;
  bounds.firstRecordOrdinal = first;
  bounds.recordCount = records;
  bounds.rawBytes = records * SESSION_SYNC_RECORD_BYTES;
  return SessionSyncProtocolError::Ok;
}

bool SessionSyncProtocol::progressIsValid(uint64_t retainedCount,
                                          uint32_t nextChunk,
                                          uint64_t uploadedRecords) {
  uint32_t chunkCount = 0U;
  if (calculateChunkCount(retainedCount, chunkCount) !=
          SessionSyncProtocolError::Ok ||
      nextChunk > chunkCount) {
    return false;
  }
  const uint64_t expected = nextChunk == chunkCount
      ? retainedCount
      : static_cast<uint64_t>(nextChunk) *
            SESSION_SYNC_RECORDS_PER_CHUNK;
  return uploadedRecords == expected;
}

bool SessionSyncProtocol::restRequestOptionsAreCompatible(
    bool printSilent,
    bool hasIfMatch,
    bool hasIfNoneMatch,
    bool shallow,
    bool hasQueryOrFilterParameters) {
  const bool conditional = hasIfMatch || hasIfNoneMatch;
  return !conditional ||
         (!printSilent && !shallow && !hasQueryOrFilterParameters);
}

bool SessionSyncProtocol::printSilentForRequestSite(
    SyncRestRequestSite site) {
  return site == SyncRestRequestSite::ChunkPut;
}

bool SessionSyncProtocol::isRetryableHttpStatus(int32_t httpStatus) {
  return httpStatus <= 0 || httpStatus == 408 || httpStatus == 429 ||
         httpStatus == 500 || httpStatus == 502 || httpStatus == 503 ||
         httpStatus == 504;
}

uint32_t SessionSyncProtocol::retryDelayMs(uint32_t retryIndex) {
  static const uint32_t kRetryDelaysMs[] = {
      1000U, 2000U, 4000U, 8000U, 15000U};
  const uint32_t lastIndex =
      static_cast<uint32_t>(sizeof(kRetryDelaysMs) /
                            sizeof(kRetryDelaysMs[0])) - 1U;
  return kRetryDelaysMs[retryIndex < lastIndex ? retryIndex : lastIndex];
}

SyncCancellationDecision SessionSyncProtocol::evaluateCancellation(
    bool cancellationRequested, bool transactionInFlight) {
  if (!cancellationRequested) {
    return SyncCancellationDecision::Continue;
  }
  return transactionInFlight
      ? SyncCancellationDecision::DeferUntilTransactionCompletes
      : SyncCancellationDecision::CancelNow;
}

SessionSyncProtocolError SessionSyncProtocol::encodeCanonicalManifest(
    const SyncManifestImmutable& manifest, uint8_t* destination,
    uint32_t destinationLength) {
  if (destination == nullptr) {
    return SessionSyncProtocolError::NullArgument;
  }
  const SessionSyncProtocolError validation = validateManifest(manifest);
  if (validation != SessionSyncProtocolError::Ok) {
    return validation;
  }
  const bool legacy = manifest.schemaVersion ==
                      SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION;
  const uint32_t canonicalLength = legacy
      ? SESSION_SYNC_LEGACY_MANIFEST_CANONICAL_BYTES
      : SESSION_SYNC_MANIFEST_CANONICAL_BYTES;
  if (destinationLength < canonicalLength) {
    return SessionSyncProtocolError::BufferTooShort;
  }

  memset(destination, 0, canonicalLength);
  destination[kCanonicalMagicOffset] = 'P';
  destination[kCanonicalMagicOffset + 1U] = 'Q';
  destination[kCanonicalMagicOffset + 2U] = 'M';
  destination[kCanonicalMagicOffset + 3U] = 'I';
  writeLe16(destination + kCanonicalVersionOffset,
            legacy ? kLegacyCanonicalFormatVersion
                   : kTimeAwareCanonicalFormatVersion);
  writeLe16(destination + kCanonicalLengthOffset,
            static_cast<uint16_t>(canonicalLength));
  writeLe32(destination + kCanonicalSchemaOffset, manifest.schemaVersion);
  memcpy(destination + kCanonicalDeviceIdOffset, manifest.deviceId,
         SESSION_SYNC_DEVICE_ID_BYTES);
  writeLe64(destination + kCanonicalSessionIdOffset, manifest.sessionId);
  destination[kCanonicalPersistentStateOffset] =
      static_cast<uint8_t>(manifest.persistentState);
  uint8_t flags = 0U;
  if (manifest.truncated) {
    flags |= kManifestFlagTruncated;
  }
  if (manifest.recoveredIncomplete) {
    flags |= kManifestFlagRecoveredIncomplete;
  }
  if (manifest.countersPartial) {
    flags |= kManifestFlagCountersPartial;
  }
  destination[kCanonicalFlagsOffset] = flags & kManifestKnownFlags;
  writeLe16(destination + kCanonicalReservedOffset, 0U);
  memcpy(destination + kCanonicalRecordFormatOffset,
         manifest.recordFormat, 4U);
  writeLe32(destination + kCanonicalRecordSizeOffset, manifest.recordSize);
  writeLe32(destination + kCanonicalRecordsPerChunkOffset,
            manifest.recordsPerChunk);
  writeLe32(destination + kCanonicalChunkCountOffset, manifest.chunkCount);
  writeLe64(destination + kCanonicalRetainedCountOffset,
            manifest.retainedCount);
  writeLe64(destination + kCanonicalTotalStoredOffset,
            manifest.totalStored);
  writeLe64(destination + kCanonicalOverwrittenCountOffset,
            manifest.overwrittenCount);
  writeLe64(destination + kCanonicalFirstLogicalOffset,
            manifest.firstLogicalIndex);
  writeLe64(destination + kCanonicalLastLogicalOffset,
            manifest.lastLogicalIndex);
  writeLe32(destination + kCanonicalFirstStm32Offset,
            manifest.firstStm32Sequence);
  writeLe32(destination + kCanonicalLastStm32Offset,
            manifest.lastStm32Sequence);
  writeLe64(destination + kCanonicalSourceGenerationOffset,
            manifest.sourceMetadataGeneration);
  if (!legacy) {
    destination[kCanonicalSessionTimeValidOffset] =
        manifest.sessionTimeValid ? 1U : 0U;
    destination[kCanonicalSessionEndTimeValidOffset] =
        manifest.sessionEndTimeValid ? 1U : 0U;
    destination[kCanonicalTimeSourceOffset] =
        static_cast<uint8_t>(manifest.timeSource);
    destination[kCanonicalTimeReservedOffset] = 0U;
    writeLe32(destination + kCanonicalSessionBootIdOffset,
              manifest.sessionBootId);
    writeLe64(destination + kCanonicalSessionStartEpochOffset,
              manifest.sessionStartEpochMs);
    writeLe64(destination + kCanonicalSessionStartCaptureOffset,
              manifest.sessionStartCaptureTimestampUs);
    writeLe64(destination + kCanonicalSessionEndEpochOffset,
              manifest.sessionEndEpochMs);
  }
  return SessionSyncProtocolError::Ok;
}

SessionSyncProtocolError SessionSyncProtocol::calculateManifestCrc32c(
    const SyncManifestImmutable& manifest, uint32_t& crc) {
  crc = 0U;
  uint8_t canonical[SESSION_SYNC_MANIFEST_CANONICAL_BYTES]{};
  const SessionSyncProtocolError result = encodeCanonicalManifest(
      manifest, canonical, sizeof(canonical));
  if (result != SessionSyncProtocolError::Ok) {
    return result;
  }
  const uint32_t canonicalLength =
      manifest.schemaVersion == SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION
          ? SESSION_SYNC_LEGACY_MANIFEST_CANONICAL_BYTES
          : SESSION_SYNC_MANIFEST_CANONICAL_BYTES;
  crc = crc32c(canonical, canonicalLength);
  return SessionSyncProtocolError::Ok;
}

bool SessionSyncProtocol::immutableManifestMatches(
    const SyncManifestImmutable& expected,
    const SyncManifestImmutable& candidate) {
  if (validateManifest(expected) != SessionSyncProtocolError::Ok ||
      validateManifest(candidate) != SessionSyncProtocolError::Ok) {
    return false;
  }
  return expected.schemaVersion == candidate.schemaVersion &&
         immutableBaseMatches(expected, candidate) &&
         expected.sessionTimeValid == candidate.sessionTimeValid &&
         expected.sessionEndTimeValid == candidate.sessionEndTimeValid &&
         expected.timeSource == candidate.timeSource &&
         expected.sessionBootId == candidate.sessionBootId &&
         expected.sessionStartEpochMs == candidate.sessionStartEpochMs &&
         expected.sessionStartCaptureTimestampUs ==
             candidate.sessionStartCaptureTimestampUs &&
         expected.sessionEndEpochMs == candidate.sessionEndEpochMs;
}

SyncManifestDecision SessionSyncProtocol::evaluateCloudManifest(
    const SyncManifestImmutable& local,
    const SyncParsedCloudManifest& cloud) {
  if (validateManifest(local) != SessionSyncProtocolError::Ok) {
    return SyncManifestDecision::CloudConflict;
  }
  if (cloud.state == SyncCloudManifestState::Absent) {
    return SyncManifestDecision::Create;
  }
  if (cloud.state == SyncCloudManifestState::Unknown ||
      (cloud.immutable.schemaVersion != SESSION_SYNC_SCHEMA_VERSION &&
       cloud.immutable.schemaVersion !=
           SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION)) {
    return SyncManifestDecision::UnsupportedCloudManifest;
  }
  if (!cloudManifestMatches(local, cloud) ||
      !cloud.uploadStartedAtValid ||
      !progressIsValid(local.retainedCount, cloud.nextChunk,
                       cloud.uploadedRecords)) {
    return SyncManifestDecision::CloudConflict;
  }
  if (cloud.state == SyncCloudManifestState::Uploading) {
    return cloud.uploadCompletedAtValid
        ? SyncManifestDecision::CloudConflict
        : SyncManifestDecision::Resume;
  }
  if (cloud.state == SyncCloudManifestState::Complete) {
    return cloud.uploadCompletedAtValid &&
                   cloud.nextChunk == local.chunkCount &&
                   cloud.uploadedRecords == local.retainedCount
        ? SyncManifestDecision::AlreadyComplete
        : SyncManifestDecision::CloudConflict;
  }
  return SyncManifestDecision::UnsupportedCloudManifest;
}

SyncCompletion412Decision SessionSyncProtocol::evaluateCompletionAfter412(
    const SyncManifestImmutable& local,
    const SyncParsedCloudManifest& cloud) {
  return evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::AlreadyComplete
      ? SyncCompletion412Decision::AcceptMatchingComplete
      : SyncCompletion412Decision::CloudConflict;
}

#if SESSION_SYNC_PROTOCOL_HOST_TEST

#include <stdio.h>

namespace {

uint32_t gFailures = 0U;
uint8_t gRawChunk[SESSION_SYNC_MAX_RAW_CHUNK_BYTES]{};
char gBase64Chunk[SESSION_SYNC_BASE64_BUFFER_BYTES]{};

void expect(bool condition, const char* testName) {
  if (!condition) {
    ++gFailures;
    printf("FAIL: %s\n", testName);
  }
}

SyncManifestImmutable sampleManifest() {
  SyncManifestImmutable manifest{};
  manifest.schemaVersion = SESSION_SYNC_SCHEMA_VERSION;
  memcpy(manifest.deviceId, "PQ-3PH-001", 11U);
  manifest.sessionId = 18446744073709551610ULL;
  manifest.persistentState = SyncManifestPersistentState::Finalized;
  manifest.truncated = true;
  manifest.recoveredIncomplete = false;
  manifest.countersPartial = false;
  memcpy(manifest.recordFormat, "PQR1", 5U);
  manifest.recordSize = SESSION_SYNC_RECORD_BYTES;
  manifest.recordsPerChunk = SESSION_SYNC_RECORDS_PER_CHUNK;
  manifest.retainedCount = 17U;
  manifest.totalStored = 31U;
  manifest.overwrittenCount = 14U;
  manifest.firstLogicalIndex = 742U;
  manifest.lastLogicalIndex = 758U;
  manifest.firstStm32Sequence = 100U;
  manifest.lastStm32Sequence = 116U;
  manifest.sourceMetadataGeneration = 9U;
  manifest.sessionTimeValid = true;
  manifest.sessionEndTimeValid = true;
  manifest.timeSource = SyncManifestTimeSource::Ntp;
  manifest.sessionBootId = 0x12345678U;
  manifest.sessionStartEpochMs = 1704067200000ULL;
  manifest.sessionStartCaptureTimestampUs = 5000000U;
  manifest.sessionEndEpochMs = 1704067201700ULL;
  SessionSyncProtocol::calculateChunkCount(
      manifest.retainedCount, manifest.chunkCount);
  return manifest;
}

SyncParsedCloudManifest cloudFrom(
    const SyncManifestImmutable& manifest,
    SyncCloudManifestState state) {
  SyncParsedCloudManifest cloud{};
  cloud.state = state;
  cloud.immutable = manifest;
  SessionSyncProtocol::calculateManifestCrc32c(
      manifest, cloud.manifestCrc32c);
  cloud.uploadStartedAtValid = true;
  cloud.uploadStartedAtMs = 123456789U;
  if (state == SyncCloudManifestState::Complete) {
    cloud.nextChunk = manifest.chunkCount;
    cloud.uploadedRecords = manifest.retainedCount;
    cloud.uploadCompletedAtValid = true;
    cloud.uploadCompletedAtMs = 123456999U;
  }
  return cloud;
}

void testDecimalAndKeys() {
  const uint64_t values[] = {
      0U, 1U, 9007199254740991ULL, 9007199254740992ULL, UINT64_MAX};
  for (uint32_t index = 0U;
       index < sizeof(values) / sizeof(values[0]); ++index) {
    char decimal[21]{};
    uint32_t length = 0U;
    uint64_t parsed = 0U;
    expect(SessionSyncProtocol::formatUint64Decimal(
               values[index], decimal, sizeof(decimal), length) ==
               SessionSyncProtocolError::Ok,
           "uint64 format");
    expect(SessionSyncProtocol::parseUint64Decimal(
               decimal, length, parsed) == SessionSyncProtocolError::Ok &&
               parsed == values[index],
           "uint64 round trip");
  }
  char decimal[21]{};
  uint32_t length = 0U;
  SessionSyncProtocol::formatUint64Decimal(
      UINT64_MAX, decimal, sizeof(decimal), length);
  expect(strcmp(decimal, "18446744073709551615") == 0,
         "UINT64_MAX exact decimal");
  uint64_t parsed = 0U;
  expect(SessionSyncProtocol::parseUint64Decimal(
             "18446744073709551616", 20U, parsed) ==
             SessionSyncProtocolError::DecimalOverflow,
         "uint64 overflow rejected");
  expect(SessionSyncProtocol::parseUint64Decimal("01", 2U, parsed) ==
             SessionSyncProtocolError::InvalidDecimal,
         "noncanonical leading zero rejected");
  expect(SessionSyncProtocol::parseUint64Decimal("+1", 2U, parsed) ==
             SessionSyncProtocolError::InvalidDecimal,
         "uint64 sign rejected");
  expect(SessionSyncProtocol::parseUint64Decimal(" 1", 2U, parsed) ==
             SessionSyncProtocolError::InvalidDecimal,
         "uint64 whitespace rejected");

  char sessionKey[SESSION_SYNC_SESSION_KEY_BYTES]{};
  expect(SessionSyncProtocol::formatSessionKey(
             UINT64_MAX, sessionKey, sizeof(sessionKey)) ==
             SessionSyncProtocolError::Ok &&
             strcmp(sessionKey, "s_18446744073709551615") == 0,
         "maximum session key");
  expect(SessionSyncProtocol::formatSessionKey(
             0U, sessionKey, sizeof(sessionKey)) ==
             SessionSyncProtocolError::Ok && strcmp(sessionKey, "s_0") == 0,
         "zero session key formatting");

  char chunkKey[SESSION_SYNC_CHUNK_KEY_BYTES]{};
  expect(SessionSyncProtocol::formatChunkKey(
             0U, chunkKey, sizeof(chunkKey)) ==
             SessionSyncProtocolError::Ok && strcmp(chunkKey, "000000") == 0,
         "first chunk key");
  expect(SessionSyncProtocol::formatChunkKey(
             1U, chunkKey, sizeof(chunkKey)) ==
             SessionSyncProtocolError::Ok && strcmp(chunkKey, "000001") == 0,
         "second chunk key");
  expect(SessionSyncProtocol::formatChunkKey(
             999999U, chunkKey, sizeof(chunkKey)) ==
             SessionSyncProtocolError::Ok && strcmp(chunkKey, "999999") == 0,
         "largest chunk key");
  expect(SessionSyncProtocol::formatChunkKey(
             1000000U, chunkKey, sizeof(chunkKey)) ==
             SessionSyncProtocolError::ValueOutOfRange,
         "oversize chunk key rejected");
}

void testBase64AndCrc() {
  struct Base64Vector {
    const char* plain;
    const char* encoded;
  };
  const Base64Vector vectors[] = {
      {"", ""},       {"f", "Zg=="},     {"fo", "Zm8="},
      {"foo", "Zm9v"}, {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
  for (uint32_t index = 0U;
       index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
    char encoded[16]{};
    uint32_t written = 0U;
    const uint32_t plainLength =
        static_cast<uint32_t>(strlen(vectors[index].plain));
    expect(SessionSyncProtocol::base64Encode(
               reinterpret_cast<const uint8_t*>(vectors[index].plain),
               plainLength, encoded, sizeof(encoded), written) ==
                   SessionSyncProtocolError::Ok &&
               strcmp(encoded, vectors[index].encoded) == 0 &&
               written == strlen(vectors[index].encoded),
           "Base64 known vector");
  }
  const uint32_t expectedLengths[][2] = {
      {0U, 0U}, {1U, 4U}, {2U, 4U}, {3U, 4U},
      {SESSION_SYNC_MAX_RAW_CHUNK_BYTES, SESSION_SYNC_MAX_BASE64_CHARS}};
  for (uint32_t index = 0U;
       index < sizeof(expectedLengths) / sizeof(expectedLengths[0]); ++index) {
    uint32_t calculated = 0U;
    expect(SessionSyncProtocol::base64EncodedLength(
               expectedLengths[index][0], calculated) ==
               SessionSyncProtocolError::Ok &&
               calculated == expectedLengths[index][1],
           "Base64 capacity calculation");
  }
  expect(SessionSyncProtocol::crc32c(
             reinterpret_cast<const uint8_t*>("123456789"), 9U) ==
             0xE3069283U,
         "CRC-32C standard vector");
  for (uint32_t index = 0U;
       index < SESSION_SYNC_MAX_RAW_CHUNK_BYTES; ++index) {
    gRawChunk[index] = static_cast<uint8_t>(index & 0xFFU);
  }
  expect(SessionSyncProtocol::crc32c(
             gRawChunk, sizeof(gRawChunk)) == 0xB1B9C698U,
         "maximum chunk CRC-32C");
  uint32_t written = 0U;
  expect(SessionSyncProtocol::base64Encode(
             gRawChunk, sizeof(gRawChunk), gBase64Chunk,
             sizeof(gBase64Chunk), written) ==
             SessionSyncProtocolError::Ok &&
             written == SESSION_SYNC_MAX_BASE64_CHARS &&
             gBase64Chunk[written] == '\0',
         "maximum chunk Base64 encoding");

  char crcHex[9]{};
  uint32_t parsedCrc = 0U;
  expect(SessionSyncProtocol::formatCrc32cHex(
             0xA1B2C3D4U, crcHex, sizeof(crcHex)) ==
             SessionSyncProtocolError::Ok &&
             strcmp(crcHex, "A1B2C3D4") == 0 &&
             SessionSyncProtocol::parseCrc32cHex(
                 crcHex, 8U, parsedCrc) == SessionSyncProtocolError::Ok &&
             parsedCrc == 0xA1B2C3D4U,
         "CRC uppercase hex round trip");
  expect(SessionSyncProtocol::parseCrc32cHex(
             "a1b2c3d4", 8U, parsedCrc) ==
             SessionSyncProtocolError::ValueOutOfRange,
         "lowercase CRC rejected");
}

void testChunkMathAndResume() {
  const struct {
    uint64_t retained;
    uint32_t chunks;
  } cases[] = {{0U, 0U}, {1U, 1U}, {8U, 1U}, {9U, 2U}, {2674U, 335U}};
  for (uint32_t index = 0U;
       index < sizeof(cases) / sizeof(cases[0]); ++index) {
    uint32_t chunkCount = 0U;
    expect(SessionSyncProtocol::calculateChunkCount(
               cases[index].retained, chunkCount) ==
               SessionSyncProtocolError::Ok &&
               chunkCount == cases[index].chunks,
           "chunk-count boundary");
  }
  SyncChunkBounds finalChunk{};
  expect(SessionSyncProtocol::calculateChunkBounds(
             2674U, 334U, finalChunk) == SessionSyncProtocolError::Ok &&
             finalChunk.firstRecordOrdinal == 2672U &&
             finalChunk.recordCount == 2U &&
             finalChunk.rawBytes == 8864U,
         "final partial chunk");
  expect(SessionSyncProtocol::calculateChunkBounds(
             2674U, 335U, finalChunk) ==
             SessionSyncProtocolError::ValueOutOfRange,
         "chunk beyond retained range rejected");

  for (uint32_t nextChunk = 0U; nextChunk <= 335U; ++nextChunk) {
    const uint64_t uploaded = nextChunk == 335U
        ? 2674U
        : static_cast<uint64_t>(nextChunk) * 8U;
    expect(SessionSyncProtocol::progressIsValid(
               2674U, nextChunk, uploaded),
           "every valid resume boundary");
    expect(!SessionSyncProtocol::progressIsValid(
               2674U, nextChunk, uploaded == 0U ? 1U : uploaded - 1U),
           "invalid resume count rejected");
  }
  expect(!SessionSyncProtocol::progressIsValid(2674U, 336U, 2674U),
         "resume chunk beyond completion rejected");

  // A reset after the deterministic PUT but before progress PATCH leaves the
  // same nextChunk. Rebuilding its bounds and key must select identical data.
  SyncChunkBounds beforeReset{};
  SyncChunkBounds afterReset{};
  char beforeKey[SESSION_SYNC_CHUNK_KEY_BYTES]{};
  char afterKey[SESSION_SYNC_CHUNK_KEY_BYTES]{};
  SessionSyncProtocol::calculateChunkBounds(2674U, 71U, beforeReset);
  SessionSyncProtocol::calculateChunkBounds(2674U, 71U, afterReset);
  SessionSyncProtocol::formatChunkKey(71U, beforeKey, sizeof(beforeKey));
  SessionSyncProtocol::formatChunkKey(71U, afterKey, sizeof(afterKey));
  expect(beforeReset.firstRecordOrdinal == afterReset.firstRecordOrdinal &&
             beforeReset.recordCount == afterReset.recordCount &&
             strcmp(beforeKey, afterKey) == 0,
         "PUT-before-PATCH deterministic replay");
}

void testManifestProtocol() {
  const SyncManifestImmutable local = sampleManifest();
  uint8_t canonical[SESSION_SYNC_MANIFEST_CANONICAL_BYTES]{};
  uint32_t canonicalCrc = 0U;
  expect(SessionSyncProtocol::encodeCanonicalManifest(
             local, canonical, sizeof(canonical)) ==
             SessionSyncProtocolError::Ok,
         "canonical immutable manifest encoding");
  expect(SessionSyncProtocol::calculateManifestCrc32c(
             local, canonicalCrc) == SessionSyncProtocolError::Ok &&
             canonicalCrc == SessionSyncProtocol::crc32c(
                                  canonical,
                                  SESSION_SYNC_MANIFEST_CANONICAL_BYTES),
         "time-aware canonical manifest CRC");

  SyncManifestImmutable legacy = local;
  legacy.schemaVersion = SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION;
  legacy.sessionTimeValid = false;
  legacy.sessionEndTimeValid = false;
  legacy.timeSource = SyncManifestTimeSource::None;
  legacy.sessionBootId = 0U;
  legacy.sessionStartEpochMs = 0U;
  legacy.sessionStartCaptureTimestampUs = 0U;
  legacy.sessionEndEpochMs = 0U;
  uint8_t legacyCanonical[SESSION_SYNC_LEGACY_MANIFEST_CANONICAL_BYTES]{};
  uint32_t legacyCrc = 0U;
  expect(SessionSyncProtocol::encodeCanonicalManifest(
             legacy, legacyCanonical, sizeof(legacyCanonical)) ==
             SessionSyncProtocolError::Ok &&
             SessionSyncProtocol::calculateManifestCrc32c(
                 legacy, legacyCrc) == SessionSyncProtocolError::Ok &&
             legacyCrc == SessionSyncProtocol::crc32c(
                              legacyCanonical, sizeof(legacyCanonical)) &&
             legacyCrc != canonicalCrc,
         "legacy canonical CRC remains versioned");

  SyncManifestImmutable invalidTime = local;
  invalidTime.sessionTimeValid = false;
  expect(SessionSyncProtocol::calculateManifestCrc32c(
             invalidTime, canonicalCrc) ==
             SessionSyncProtocolError::InvalidManifest,
         "invalid start anchor combination rejected");
  invalidTime = local;
  invalidTime.sessionEndEpochMs = invalidTime.sessionStartEpochMs - 1U;
  expect(SessionSyncProtocol::calculateManifestCrc32c(
             invalidTime, canonicalCrc) ==
             SessionSyncProtocolError::InvalidManifest,
         "end anchor before start rejected");

  SyncManifestImmutable mutated = local;
  mutated.sessionId--;
  expect(!SessionSyncProtocol::immutableManifestMatches(local, mutated),
         "immutable session conflict");
  mutated = local;
  mutated.firstLogicalIndex++;
  mutated.lastLogicalIndex++;
  expect(!SessionSyncProtocol::immutableManifestMatches(local, mutated),
         "immutable logical range conflict");
  mutated = local;
  mutated.sourceMetadataGeneration++;
  expect(!SessionSyncProtocol::immutableManifestMatches(local, mutated),
         "immutable source generation conflict");
  mutated = local;
  mutated.recordFormat[3] = '2';
  expect(!SessionSyncProtocol::immutableManifestMatches(local, mutated),
         "immutable record format conflict");
  mutated = local;
  mutated.sessionStartEpochMs++;
  expect(!SessionSyncProtocol::immutableManifestMatches(local, mutated),
         "immutable time anchor conflict");

  SyncParsedCloudManifest cloud{};
  cloud.state = SyncCloudManifestState::Absent;
  expect(SessionSyncProtocol::evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::Create,
         "absent manifest creation decision");

  cloud = cloudFrom(local, SyncCloudManifestState::Uploading);
  cloud.nextChunk = 1U;
  cloud.uploadedRecords = 8U;
  expect(SessionSyncProtocol::evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::Resume,
         "matching upload resume decision");

  SyncManifestImmutable maximum = local;
  maximum.retainedCount = 2674U;
  maximum.totalStored = 4000U;
  maximum.overwrittenCount = 1326U;
  maximum.firstLogicalIndex = 10000U;
  maximum.lastLogicalIndex = 12673U;
  SessionSyncProtocol::calculateChunkCount(
      maximum.retainedCount, maximum.chunkCount);
  SyncParsedCloudManifest resume =
      cloudFrom(maximum, SyncCloudManifestState::Uploading);
  for (uint32_t nextChunk = 0U; nextChunk < maximum.chunkCount;
       ++nextChunk) {
    resume.nextChunk = nextChunk;
    resume.uploadedRecords = static_cast<uint64_t>(nextChunk) * 8U;
    expect(SessionSyncProtocol::evaluateCloudManifest(maximum, resume) ==
               SyncManifestDecision::Resume,
            "manifest resume from every chunk boundary");
  }
  resume.nextChunk = maximum.chunkCount;
  resume.uploadedRecords = maximum.retainedCount;
  expect(SessionSyncProtocol::evaluateCloudManifest(maximum, resume) ==
             SyncManifestDecision::Resume,
         "manifest resume after final partial-chunk progress");

  // The chunk PUT has succeeded but manifest progress is unchanged. The
  // cloud decision must still resume at that same deterministic chunk.
  resume.nextChunk = 71U;
  resume.uploadedRecords = 71U * 8U;
  expect(SessionSyncProtocol::evaluateCloudManifest(maximum, resume) ==
             SyncManifestDecision::Resume,
         "manifest resumes after PUT-before-PATCH reset");
  cloud.manifestCrc32c ^= 1U;
  expect(SessionSyncProtocol::evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::CloudConflict,
         "manifest CRC conflict");

  cloud = cloudFrom(local, SyncCloudManifestState::Complete);
  expect(SessionSyncProtocol::evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::AlreadyComplete,
         "already-complete matching manifest");

  SyncManifestImmutable localWithoutTime = local;
  localWithoutTime.sessionTimeValid = false;
  localWithoutTime.sessionEndTimeValid = false;
  localWithoutTime.sessionStartEpochMs = 0U;
  localWithoutTime.sessionEndEpochMs = 0U;
  SyncParsedCloudManifest legacyComplete =
      cloudFrom(legacy, SyncCloudManifestState::Complete);
  expect(SessionSyncProtocol::evaluateCloudManifest(
             localWithoutTime, legacyComplete) ==
             SyncManifestDecision::AlreadyComplete,
         "matching old completed session remains idempotent");
  expect(SessionSyncProtocol::evaluateCloudManifest(local, legacyComplete) ==
             SyncManifestDecision::CloudConflict,
         "legacy manifest cannot discard a valid time anchor");
  expect(SessionSyncProtocol::evaluateCompletionAfter412(local, cloud) ==
             SyncCompletion412Decision::AcceptMatchingComplete,
         "HTTP 412 accepts matching complete manifest");
  cloud.uploadedRecords--;
  expect(SessionSyncProtocol::evaluateCompletionAfter412(local, cloud) ==
             SyncCompletion412Decision::CloudConflict,
         "HTTP 412 rejects conflicting manifest");

  cloud = cloudFrom(local, SyncCloudManifestState::Uploading);
  expect(SessionSyncProtocol::evaluateCompletionAfter412(local, cloud) ==
             SyncCompletion412Decision::CloudConflict,
         "HTTP 412 rejects still-uploading manifest");
  cloud = cloudFrom(local, SyncCloudManifestState::Unknown);
  expect(SessionSyncProtocol::evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::UnsupportedCloudManifest,
         "unknown cloud state rejected");
  cloud = cloudFrom(local, SyncCloudManifestState::Uploading);
  cloud.immutable.schemaVersion = 3U;
  expect(SessionSyncProtocol::evaluateCloudManifest(local, cloud) ==
             SyncManifestDecision::UnsupportedCloudManifest,
         "unknown cloud schema rejected");
}

void testRetryAndCancellation() {
  const int32_t retryable[] = {-1, 0, 408, 429, 500, 502, 503, 504};
  for (uint32_t index = 0U;
       index < sizeof(retryable) / sizeof(retryable[0]); ++index) {
    expect(SessionSyncProtocol::isRetryableHttpStatus(retryable[index]),
           "retryable HTTP status");
  }
  const int32_t terminal[] = {200, 204, 400, 401, 403, 404, 409, 412};
  for (uint32_t index = 0U;
       index < sizeof(terminal) / sizeof(terminal[0]); ++index) {
    expect(!SessionSyncProtocol::isRetryableHttpStatus(terminal[index]),
           "terminal HTTP status");
  }
  const uint32_t expectedDelays[] = {
      1000U, 2000U, 4000U, 8000U, 15000U, 15000U};
  for (uint32_t index = 0U;
       index < sizeof(expectedDelays) / sizeof(expectedDelays[0]); ++index) {
    expect(SessionSyncProtocol::retryDelayMs(index) == expectedDelays[index],
           "bounded retry delay");
  }
  expect(SessionSyncProtocol::evaluateCancellation(false, false) ==
             SyncCancellationDecision::Continue,
         "no cancellation");
  expect(SessionSyncProtocol::evaluateCancellation(true, true) ==
             SyncCancellationDecision::DeferUntilTransactionCompletes,
         "cancellation deferred during transaction");
  expect(SessionSyncProtocol::evaluateCancellation(true, false) ==
             SyncCancellationDecision::CancelNow,
         "cancellation at request boundary");
}

void testRestRequestOptionPolicy() {
  const bool initialPrintSilent =
      SessionSyncProtocol::printSilentForRequestSite(
          SyncRestRequestSite::InitialConditionalManifestPut);
  const bool chunkPrintSilent =
      SessionSyncProtocol::printSilentForRequestSite(
          SyncRestRequestSite::ChunkPut);
  const bool completionPrintSilent =
      SessionSyncProtocol::printSilentForRequestSite(
          SyncRestRequestSite::CompletionConditionalManifestPut);
  expect(!initialPrintSilent &&
             SessionSyncProtocol::restRequestOptionsAreCompatible(
                 initialPrintSilent, true, false, false, false),
         "initial conditional manifest PUT omits print=silent");
  expect(chunkPrintSilent &&
             SessionSyncProtocol::restRequestOptionsAreCompatible(
                 chunkPrintSilent, false, false, false, false),
         "ordinary chunk PUT may use print=silent");
  expect(!completionPrintSilent &&
             SessionSyncProtocol::restRequestOptionsAreCompatible(
                 completionPrintSilent, true, false, false, false),
         "completion conditional manifest PUT omits print=silent");
  expect(!SessionSyncProtocol::restRequestOptionsAreCompatible(
             true, true, false, false, false),
         "If-Match plus print=silent rejected");
  expect(!SessionSyncProtocol::restRequestOptionsAreCompatible(
             true, false, true, false, false),
         "If-None-Match plus print=silent rejected");
  expect(!SessionSyncProtocol::restRequestOptionsAreCompatible(
             false, true, false, true, false),
         "conditional shallow request rejected");
  expect(!SessionSyncProtocol::restRequestOptionsAreCompatible(
             false, true, false, false, true),
         "conditional query or filter request rejected");
}

}  // namespace

int main() {
  testDecimalAndKeys();
  testBase64AndCrc();
  testChunkMathAndResume();
  testManifestProtocol();
  testRetryAndCancellation();
  testRestRequestOptionPolicy();

  if (gFailures == 0U) {
    printf("SessionSyncProtocol host tests: PASS\n");
    return 0;
  }
  printf("SessionSyncProtocol host tests: FAIL (%lu)\n",
         static_cast<unsigned long>(gFailures));
  return 1;
}

#endif  // SESSION_SYNC_PROTOCOL_HOST_TEST
