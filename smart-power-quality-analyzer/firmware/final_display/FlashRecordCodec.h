#pragma once

#include <stdint.h>

#ifndef FLASH_RECORD_CODEC_SELF_TEST
#define FLASH_RECORD_CODEC_SELF_TEST 0
#endif

constexpr uint32_t FLASH_RECORD_BYTES = 4432U;
constexpr uint32_t FLASH_RECORD_HEADER_BYTES = 64U;
constexpr uint32_t FLASH_RECORD_PAYLOAD_BYTES = 4354U;
constexpr uint32_t FLASH_RECORD_COMMIT_OFFSET = 4428U;
constexpr uint16_t FLASH_RECORD_FORMAT_VERSION = 1U;
constexpr uint32_t FLASH_RECORD_COMMIT_MARKER = 0x54494D43U;  // "CMIT" in LE

enum class FlashRecordCodecError : uint8_t {
  Ok = 0,
  NullArgument,
  BufferTooShort,
  InvalidPayloadLength,
  BadMagic,
  UnsupportedVersion,
  BadLength,
  NonZeroReserved,
  NonZeroPadding,
  MissingCommitMarker,
  BadHeaderCrc,
  BadPayloadCrc,
  BadWholeRecordCrc
};

struct FlashRecordMetadata {
  uint16_t recordFormatVersion;
  uint64_t sessionId;
  uint64_t logicalRecordIndex;
  uint64_t captureTimestampUs;
  uint32_t stm32Sequence;
  uint16_t packetFormatVersion;
  uint16_t flags;
  uint32_t bootId;
};

class FlashRecordCodec {
 public:
  static FlashRecordCodecError encode(
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
      uint32_t bootId);

  static FlashRecordCodecError validate(const uint8_t* record,
                                        uint32_t recordLength);

  static FlashRecordCodecError decodeMetadata(
      const uint8_t* record,
      uint32_t recordLength,
      FlashRecordMetadata& metadata);

  static FlashRecordCodecError copyPayload(
      const uint8_t* record,
      uint32_t recordLength,
      uint8_t* destination,
      uint32_t destinationLength);

#if FLASH_RECORD_CODEC_SELF_TEST
  static bool runSelfTest();
#endif

 private:
  static uint32_t crc32c(const uint8_t* data, uint32_t length);
};
