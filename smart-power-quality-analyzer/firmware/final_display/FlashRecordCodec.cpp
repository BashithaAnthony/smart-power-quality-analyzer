#include "FlashRecordCodec.h"

#include <string.h>

#if FLASH_RECORD_CODEC_SELF_TEST
#include <Arduino.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#endif

namespace {

constexpr uint32_t kMagicOffset = 0U;
constexpr uint32_t kRecordVersionOffset = 4U;
constexpr uint32_t kHeaderLengthOffset = 6U;
constexpr uint32_t kTotalLengthOffset = 8U;
constexpr uint32_t kPayloadLengthOffset = 12U;
constexpr uint32_t kSessionIdOffset = 16U;
constexpr uint32_t kLogicalRecordIndexOffset = 24U;
constexpr uint32_t kCaptureTimestampOffset = 32U;
constexpr uint32_t kStm32SequenceOffset = 40U;
constexpr uint32_t kPacketFormatVersionOffset = 44U;
constexpr uint32_t kFlagsOffset = 46U;
constexpr uint32_t kBootIdOffset = 48U;
constexpr uint32_t kReserved0Offset = 52U;
constexpr uint32_t kReserved1Offset = 56U;
constexpr uint32_t kHeaderCrcOffset = 60U;
constexpr uint32_t kPayloadOffset = 64U;
constexpr uint32_t kPaddingOffset = 4418U;
constexpr uint32_t kPayloadCrcOffset = 4420U;
constexpr uint32_t kWholeRecordCrcOffset = 4424U;

constexpr uint32_t kHeaderCrcCoverageBytes = 60U;
constexpr uint32_t kPaddingBytes = 2U;
constexpr uint32_t kWholeRecordCrcCoverageBytes = 4424U;
constexpr uint32_t kCrc32cReflectedPolynomial = 0x82F63B78U;
constexpr uint32_t kCrc32cInitialValue = 0xFFFFFFFFU;
constexpr uint32_t kCrc32cFinalXor = 0xFFFFFFFFU;

constexpr uint8_t kMagic0 = 'P';
constexpr uint8_t kMagic1 = 'Q';
constexpr uint8_t kMagic2 = 'R';
constexpr uint8_t kMagic3 = '1';

static_assert(kRecordVersionOffset == kMagicOffset + 4U,
              "Record version must follow the four magic bytes");
static_assert(kHeaderLengthOffset == kRecordVersionOffset + 2U,
              "Header length offset mismatch");
static_assert(kTotalLengthOffset == kHeaderLengthOffset + 2U,
              "Total length offset mismatch");
static_assert(kPayloadLengthOffset == kTotalLengthOffset + 4U,
              "Payload length offset mismatch");
static_assert(kSessionIdOffset == kPayloadLengthOffset + 4U,
              "Session ID offset mismatch");
static_assert(kLogicalRecordIndexOffset == kSessionIdOffset + 8U,
              "Logical record index offset mismatch");
static_assert(kCaptureTimestampOffset == kLogicalRecordIndexOffset + 8U,
              "Capture timestamp offset mismatch");
static_assert(kStm32SequenceOffset == kCaptureTimestampOffset + 8U,
              "STM32 sequence offset mismatch");
static_assert(kPacketFormatVersionOffset == kStm32SequenceOffset + 4U,
              "Packet-format version offset mismatch");
static_assert(kFlagsOffset == kPacketFormatVersionOffset + 2U,
              "Record flags offset mismatch");
static_assert(kBootIdOffset == kFlagsOffset + 2U,
              "Boot ID offset mismatch");
static_assert(kReserved0Offset == kBootIdOffset + 4U,
              "First reserved field offset mismatch");
static_assert(kReserved1Offset == kReserved0Offset + 4U,
              "Second reserved field offset mismatch");
static_assert(kHeaderCrcOffset == kReserved1Offset + 4U,
              "Header CRC offset mismatch");
static_assert(kHeaderCrcOffset + 4U == FLASH_RECORD_HEADER_BYTES,
              "Header must be exactly 64 bytes");
static_assert(kPayloadOffset == FLASH_RECORD_HEADER_BYTES,
              "Payload must immediately follow the header");
static_assert(kPaddingOffset ==
                  kPayloadOffset + FLASH_RECORD_PAYLOAD_BYTES,
              "Padding offset mismatch");
static_assert(kPayloadCrcOffset == kPaddingOffset + kPaddingBytes,
              "Payload CRC offset mismatch");
static_assert(kWholeRecordCrcOffset == kPayloadCrcOffset + 4U,
              "Whole-record CRC offset mismatch");
static_assert(FLASH_RECORD_COMMIT_OFFSET == kWholeRecordCrcOffset + 4U,
              "Commit marker offset mismatch");
static_assert(FLASH_RECORD_COMMIT_OFFSET + 4U == FLASH_RECORD_BYTES,
              "Record must be exactly 4432 bytes");
static_assert(kHeaderCrcCoverageBytes == kHeaderCrcOffset,
              "Header CRC must cover bytes 0 through 59");
static_assert(kWholeRecordCrcCoverageBytes == kWholeRecordCrcOffset,
              "Whole-record CRC must cover bytes 0 through 4423");

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
  for (uint8_t byteIndex = 0U; byteIndex < 8U; ++byteIndex) {
    destination[byteIndex] =
        static_cast<uint8_t>(value >> (byteIndex * 8U));
  }
}

uint16_t readLe16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t readLe32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8U) |
         (static_cast<uint32_t>(source[2]) << 16U) |
         (static_cast<uint32_t>(source[3]) << 24U);
}

uint64_t readLe64(const uint8_t* source) {
  uint64_t value = 0U;
  for (uint8_t byteIndex = 0U; byteIndex < 8U; ++byteIndex) {
    value |= static_cast<uint64_t>(source[byteIndex]) << (byteIndex * 8U);
  }
  return value;
}

#if FLASH_RECORD_CODEC_SELF_TEST
bool expectSelfTest(bool condition,
                    const char* testName,
                    uint32_t& failureCount) {
  if (condition) {
    return true;
  }

  ++failureCount;
  Serial.print("Flash record codec self-test failure: ");
  Serial.println(testName);
  return false;
}
#endif

}  // namespace

FlashRecordCodecError FlashRecordCodec::encode(
    uint8_t* destination,
    uint32_t destinationLength,
    const uint8_t* packetBytes,
    uint32_t packetLength,
    uint64_t sessionId,
    uint64_t logicalRecordIndex,
    uint64_t captureTimestampUs,
    uint32_t stm32Sequence,
    uint16_t packetFormatVersion,
    uint16_t flags,
    uint32_t bootId) {
  if (destination == nullptr || packetBytes == nullptr) {
    return FlashRecordCodecError::NullArgument;
  }
  if (destinationLength < FLASH_RECORD_BYTES) {
    return FlashRecordCodecError::BufferTooShort;
  }
  if (packetLength != FLASH_RECORD_PAYLOAD_BYTES) {
    return FlashRecordCodecError::InvalidPayloadLength;
  }

  // A zero marker represents an uncommitted RAM record during construction.
  // The fixed commit marker is deliberately written only after both CRCs.
  writeLe32(destination + FLASH_RECORD_COMMIT_OFFSET, 0U);

  destination[kMagicOffset] = kMagic0;
  destination[kMagicOffset + 1U] = kMagic1;
  destination[kMagicOffset + 2U] = kMagic2;
  destination[kMagicOffset + 3U] = kMagic3;
  writeLe16(destination + kRecordVersionOffset,
            FLASH_RECORD_FORMAT_VERSION);
  writeLe16(destination + kHeaderLengthOffset,
            static_cast<uint16_t>(FLASH_RECORD_HEADER_BYTES));
  writeLe32(destination + kTotalLengthOffset, FLASH_RECORD_BYTES);
  writeLe32(destination + kPayloadLengthOffset,
            FLASH_RECORD_PAYLOAD_BYTES);
  writeLe64(destination + kSessionIdOffset, sessionId);
  writeLe64(destination + kLogicalRecordIndexOffset, logicalRecordIndex);
  writeLe64(destination + kCaptureTimestampOffset, captureTimestampUs);
  writeLe32(destination + kStm32SequenceOffset, stm32Sequence);
  writeLe16(destination + kPacketFormatVersionOffset, packetFormatVersion);
  writeLe16(destination + kFlagsOffset, flags);
  writeLe32(destination + kBootIdOffset, bootId);
  writeLe32(destination + kReserved0Offset, 0U);
  writeLe32(destination + kReserved1Offset, 0U);

  const uint32_t headerCrc = crc32c(destination, kHeaderCrcCoverageBytes);
  writeLe32(destination + kHeaderCrcOffset, headerCrc);

  memcpy(destination + kPayloadOffset,
         packetBytes,
         FLASH_RECORD_PAYLOAD_BYTES);
  destination[kPaddingOffset] = 0U;
  destination[kPaddingOffset + 1U] = 0U;

  const uint32_t payloadCrc =
      crc32c(destination + kPayloadOffset, FLASH_RECORD_PAYLOAD_BYTES);
  writeLe32(destination + kPayloadCrcOffset, payloadCrc);

  const uint32_t wholeRecordCrc =
      crc32c(destination, kWholeRecordCrcCoverageBytes);
  writeLe32(destination + kWholeRecordCrcOffset, wholeRecordCrc);

  writeLe32(destination + FLASH_RECORD_COMMIT_OFFSET,
            FLASH_RECORD_COMMIT_MARKER);
  return FlashRecordCodecError::Ok;
}

FlashRecordCodecError FlashRecordCodec::validate(
    const uint8_t* record,
    uint32_t recordLength) {
  if (record == nullptr) {
    return FlashRecordCodecError::NullArgument;
  }
  if (recordLength < FLASH_RECORD_BYTES) {
    return FlashRecordCodecError::BufferTooShort;
  }

  if (record[kMagicOffset] != kMagic0 ||
      record[kMagicOffset + 1U] != kMagic1 ||
      record[kMagicOffset + 2U] != kMagic2 ||
      record[kMagicOffset + 3U] != kMagic3) {
    return FlashRecordCodecError::BadMagic;
  }

  if (readLe16(record + kRecordVersionOffset) !=
      FLASH_RECORD_FORMAT_VERSION) {
    return FlashRecordCodecError::UnsupportedVersion;
  }

  if (readLe16(record + kHeaderLengthOffset) !=
          FLASH_RECORD_HEADER_BYTES ||
      readLe32(record + kTotalLengthOffset) != FLASH_RECORD_BYTES ||
      readLe32(record + kPayloadLengthOffset) !=
          FLASH_RECORD_PAYLOAD_BYTES) {
    return FlashRecordCodecError::BadLength;
  }

  if (readLe32(record + kReserved0Offset) != 0U ||
      readLe32(record + kReserved1Offset) != 0U) {
    return FlashRecordCodecError::NonZeroReserved;
  }

  if (record[kPaddingOffset] != 0U ||
      record[kPaddingOffset + 1U] != 0U) {
    return FlashRecordCodecError::NonZeroPadding;
  }

  if (readLe32(record + FLASH_RECORD_COMMIT_OFFSET) !=
      FLASH_RECORD_COMMIT_MARKER) {
    return FlashRecordCodecError::MissingCommitMarker;
  }

  const uint32_t storedHeaderCrc = readLe32(record + kHeaderCrcOffset);
  const uint32_t calculatedHeaderCrc =
      crc32c(record, kHeaderCrcCoverageBytes);
  if (storedHeaderCrc != calculatedHeaderCrc) {
    return FlashRecordCodecError::BadHeaderCrc;
  }

  const uint32_t storedPayloadCrc = readLe32(record + kPayloadCrcOffset);
  const uint32_t calculatedPayloadCrc =
      crc32c(record + kPayloadOffset, FLASH_RECORD_PAYLOAD_BYTES);
  if (storedPayloadCrc != calculatedPayloadCrc) {
    return FlashRecordCodecError::BadPayloadCrc;
  }

  const uint32_t storedWholeRecordCrc =
      readLe32(record + kWholeRecordCrcOffset);
  const uint32_t calculatedWholeRecordCrc =
      crc32c(record, kWholeRecordCrcCoverageBytes);
  if (storedWholeRecordCrc != calculatedWholeRecordCrc) {
    return FlashRecordCodecError::BadWholeRecordCrc;
  }

  return FlashRecordCodecError::Ok;
}

FlashRecordCodecError FlashRecordCodec::decodeMetadata(
    const uint8_t* record,
    uint32_t recordLength,
    FlashRecordMetadata& metadata) {
  const FlashRecordCodecError validation = validate(record, recordLength);
  if (validation != FlashRecordCodecError::Ok) {
    return validation;
  }

  FlashRecordMetadata decoded{};
  decoded.recordFormatVersion =
      readLe16(record + kRecordVersionOffset);
  decoded.sessionId = readLe64(record + kSessionIdOffset);
  decoded.logicalRecordIndex =
      readLe64(record + kLogicalRecordIndexOffset);
  decoded.captureTimestampUs =
      readLe64(record + kCaptureTimestampOffset);
  decoded.stm32Sequence = readLe32(record + kStm32SequenceOffset);
  decoded.packetFormatVersion =
      readLe16(record + kPacketFormatVersionOffset);
  decoded.flags = readLe16(record + kFlagsOffset);
  decoded.bootId = readLe32(record + kBootIdOffset);
  metadata = decoded;
  return FlashRecordCodecError::Ok;
}

FlashRecordCodecError FlashRecordCodec::copyPayload(
    const uint8_t* record,
    uint32_t recordLength,
    uint8_t* destination,
    uint32_t destinationLength) {
  if (record == nullptr || destination == nullptr) {
    return FlashRecordCodecError::NullArgument;
  }
  if (destinationLength < FLASH_RECORD_PAYLOAD_BYTES) {
    return FlashRecordCodecError::BufferTooShort;
  }

  const FlashRecordCodecError validation = validate(record, recordLength);
  if (validation != FlashRecordCodecError::Ok) {
    return validation;
  }

  memcpy(destination,
         record + kPayloadOffset,
         FLASH_RECORD_PAYLOAD_BYTES);
  return FlashRecordCodecError::Ok;
}

uint32_t FlashRecordCodec::crc32c(const uint8_t* data, uint32_t length) {
  // CRC-32C (Castagnoli): reflected polynomial 0x82F63B78, initial value
  // 0xFFFFFFFF, reflected/least-significant-bit-first input processing, and
  // final XOR 0xFFFFFFFF. No platform-specific CRC peripheral is required.
  uint32_t crc = kCrc32cInitialValue;
  for (uint32_t byteIndex = 0U; byteIndex < length; ++byteIndex) {
    crc ^= data[byteIndex];
    for (uint8_t bitIndex = 0U; bitIndex < 8U; ++bitIndex) {
      const uint32_t polynomialMask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^
            (kCrc32cReflectedPolynomial & polynomialMask);
    }
  }
  return crc ^ kCrc32cFinalXor;
}

#if FLASH_RECORD_CODEC_SELF_TEST
bool FlashRecordCodec::runSelfTest() {
  constexpr uint32_t kTestAllocationBytes =
      FLASH_RECORD_BYTES + FLASH_RECORD_PAYLOAD_BYTES;
  constexpr uint64_t kSessionId = 0x0102030405060708ULL;
  constexpr uint64_t kLogicalRecordIndex = 0x1112131415161718ULL;
  constexpr uint64_t kCaptureTimestampUs = 0x2122232425262728ULL;
  constexpr uint32_t kStm32Sequence = 0xA1B2C3D4U;
  constexpr uint16_t kPacketFormatVersion = 0x0102U;
  constexpr uint16_t kFlags = 0xA55AU;
  constexpr uint32_t kBootId = 0x5A6B7C8DU;

  uint32_t failureCount = 0U;
  if (!psramFound()) {
    Serial.println("Flash record codec RAM self-test: FAIL (PSRAM unavailable)");
    return false;
  }

  uint8_t* testMemory = static_cast<uint8_t*>(heap_caps_malloc(
      kTestAllocationBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (testMemory == nullptr) {
    Serial.println("Flash record codec RAM self-test: FAIL (PSRAM allocation)");
    return false;
  }

  uint8_t* record = testMemory;
  uint8_t* payload = testMemory + FLASH_RECORD_BYTES;
  for (uint32_t index = 0U; index < FLASH_RECORD_PAYLOAD_BYTES; ++index) {
    payload[index] = static_cast<uint8_t>((index * 37U + 11U) & 0xFFU);
  }

  FlashRecordCodecError result = encode(
      record,
      FLASH_RECORD_BYTES,
      payload,
      FLASH_RECORD_PAYLOAD_BYTES,
      kSessionId,
      kLogicalRecordIndex,
      kCaptureTimestampUs,
      kStm32Sequence,
      kPacketFormatVersion,
      kFlags,
      kBootId);
  expectSelfTest(result == FlashRecordCodecError::Ok,
                 "encode",
                 failureCount);
  expectSelfTest(validate(record, FLASH_RECORD_BYTES) ==
                     FlashRecordCodecError::Ok,
                 "validate",
                 failureCount);

  FlashRecordMetadata metadata{};
  result = decodeMetadata(record, FLASH_RECORD_BYTES, metadata);
  const bool metadataMatches =
      result == FlashRecordCodecError::Ok &&
      metadata.recordFormatVersion == FLASH_RECORD_FORMAT_VERSION &&
      metadata.sessionId == kSessionId &&
      metadata.logicalRecordIndex == kLogicalRecordIndex &&
      metadata.captureTimestampUs == kCaptureTimestampUs &&
      metadata.stm32Sequence == kStm32Sequence &&
      metadata.packetFormatVersion == kPacketFormatVersion &&
      metadata.flags == kFlags && metadata.bootId == kBootId;
  expectSelfTest(metadataMatches, "metadata decode", failureCount);

  memset(payload, 0, FLASH_RECORD_PAYLOAD_BYTES);
  result = copyPayload(record,
                       FLASH_RECORD_BYTES,
                       payload,
                       FLASH_RECORD_PAYLOAD_BYTES);
  bool payloadMatches = result == FlashRecordCodecError::Ok;
  for (uint32_t index = 0U;
       payloadMatches && index < FLASH_RECORD_PAYLOAD_BYTES;
       ++index) {
    const uint8_t expected =
        static_cast<uint8_t>((index * 37U + 11U) & 0xFFU);
    payloadMatches = payload[index] == expected;
  }
  expectSelfTest(payloadMatches, "payload copy", failureCount);

  record[kBootIdOffset] ^= 0x01U;
  expectSelfTest(validate(record, FLASH_RECORD_BYTES) ==
                     FlashRecordCodecError::BadHeaderCrc,
                 "header corruption rejection",
                 failureCount);
  record[kBootIdOffset] ^= 0x01U;

  constexpr uint32_t kPayloadCorruptionIndex = 123U;
  record[kPayloadOffset + kPayloadCorruptionIndex] ^= 0x01U;
  expectSelfTest(validate(record, FLASH_RECORD_BYTES) ==
                     FlashRecordCodecError::BadPayloadCrc,
                 "payload corruption rejection",
                 failureCount);
  record[kPayloadOffset + kPayloadCorruptionIndex] ^= 0x01U;

  writeLe32(record + FLASH_RECORD_COMMIT_OFFSET, 0U);
  expectSelfTest(validate(record, FLASH_RECORD_BYTES) ==
                     FlashRecordCodecError::MissingCommitMarker,
                 "missing commit rejection",
                 failureCount);
  writeLe32(record + FLASH_RECORD_COMMIT_OFFSET,
            FLASH_RECORD_COMMIT_MARKER);

  record[kWholeRecordCrcOffset] ^= 0x01U;
  expectSelfTest(validate(record, FLASH_RECORD_BYTES) ==
                     FlashRecordCodecError::BadWholeRecordCrc,
                 "whole-record CRC rejection",
                 failureCount);
  record[kWholeRecordCrcOffset] ^= 0x01U;

  result = encode(record,
                  FLASH_RECORD_BYTES - 1U,
                  payload,
                  FLASH_RECORD_PAYLOAD_BYTES,
                  kSessionId,
                  kLogicalRecordIndex,
                  kCaptureTimestampUs,
                  kStm32Sequence,
                  kPacketFormatVersion,
                  kFlags,
                  kBootId);
  expectSelfTest(result == FlashRecordCodecError::BufferTooShort,
                 "short destination rejection",
                 failureCount);

  result = encode(record,
                  FLASH_RECORD_BYTES,
                  payload,
                  FLASH_RECORD_PAYLOAD_BYTES - 1U,
                  kSessionId,
                  kLogicalRecordIndex,
                  kCaptureTimestampUs,
                  kStm32Sequence,
                  kPacketFormatVersion,
                  kFlags,
                  kBootId);
  expectSelfTest(result == FlashRecordCodecError::InvalidPayloadLength,
                 "invalid payload length rejection",
                 failureCount);

  constexpr uint8_t kCrcCheck[] = {
      '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  expectSelfTest(crc32c(kCrcCheck, 9U) == 0xE3069283U,
                 "CRC-32C check vector",
                 failureCount);

  heap_caps_free(testMemory);
  if (failureCount == 0U) {
    Serial.println("Flash record codec RAM self-test: PASS");
    return true;
  }

  Serial.print("Flash record codec RAM self-test: FAIL (");
  Serial.print(static_cast<unsigned long>(failureCount));
  Serial.println(" checks)");
  return false;
}
#endif
