#include "SessionStorageFormats.h"

#include <string.h>

#include "WallClockTypes.h"

namespace {

constexpr uint32_t kMetadataMagicOffset = 0U;
constexpr uint32_t kMetadataVersionOffset = 4U;
constexpr uint32_t kMetadataLengthOffset = 6U;
constexpr uint32_t kMetadataGenerationOffset = 8U;
constexpr uint32_t kMetadataSessionIdOffset = 16U;
constexpr uint32_t kMetadataStateOffset = 24U;
constexpr uint32_t kMetadataSyncStateOffset = 25U;
constexpr uint32_t kMetadataReserved0Offset = 26U;
constexpr uint32_t kMetadataFlagsOffset = 28U;
constexpr uint32_t kMetadataFlushIntervalOffset = 32U;
constexpr uint32_t kMetadataBootIdOffset = 36U;
constexpr uint32_t kMetadataStartUptimeOffset = 40U;
constexpr uint32_t kMetadataEndUptimeOffset = 48U;
constexpr uint32_t kMetadataStartWallTimeOffset = 56U;
constexpr uint32_t kMetadataEndWallTimeOffset = 64U;
constexpr uint32_t kMetadataSessionStartLogicalOffset = 72U;
constexpr uint32_t kMetadataFirstRetainedLogicalOffset = 80U;
constexpr uint32_t kMetadataLastRetainedLogicalOffset = 88U;
constexpr uint32_t kMetadataNextLogicalOffset = 96U;
constexpr uint32_t kMetadataTotalStoredOffset = 104U;
constexpr uint32_t kMetadataRetainedCountOffset = 112U;
constexpr uint32_t kMetadataOverwrittenCountOffset = 120U;
constexpr uint32_t kMetadataDroppedCountOffset = 128U;
constexpr uint32_t kMetadataNextSessionIdOffset = 136U;
constexpr uint32_t kMetadataNextSegmentSequenceOffset = 144U;
constexpr uint32_t kMetadataFirstStm32SequenceOffset = 152U;
constexpr uint32_t kMetadataLastStm32SequenceOffset = 156U;
constexpr uint32_t kMetadataOldestPhysicalSegmentOffset = 160U;
constexpr uint32_t kMetadataOldestSlotOffset = 162U;
constexpr uint32_t kMetadataNextWriteSegmentOffset = 164U;
constexpr uint32_t kMetadataNextWriteSlotOffset = 166U;
constexpr uint32_t kMetadataRecordSizeOffset = 168U;
constexpr uint32_t kMetadataRecordsPerSegmentOffset = 172U;
constexpr uint32_t kMetadataSegmentHeaderSizeOffset = 174U;
constexpr uint32_t kMetadataDataSegmentCountOffset = 176U;
constexpr uint32_t kMetadataReserved1Offset = 178U;
constexpr uint32_t kMetadataMaximumRecordsOffset = 180U;
constexpr uint32_t kMetadataSourceGenerationOffset = 184U;
constexpr uint32_t kMetadataReservedBytesOffset = 192U;
constexpr uint32_t kMetadataReservedBytesLength = 56U;

static_assert(kMetadataSourceGenerationOffset + 8U ==
                  kMetadataReservedBytesOffset,
              "Source generation must occupy metadata bytes 184 through 191");
static_assert(kMetadataReservedBytesOffset + kMetadataReservedBytesLength ==
                  SESSION_METADATA_CRC_OFFSET,
              "Metadata bytes 192 through 247 must remain reserved");

constexpr uint32_t kSegmentMagicOffset = 0U;
constexpr uint32_t kSegmentVersionOffset = 4U;
constexpr uint32_t kSegmentLengthOffset = 6U;
constexpr uint32_t kSegmentSequenceOffset = 8U;
constexpr uint32_t kSegmentSessionIdOffset = 16U;
constexpr uint32_t kSegmentFirstLogicalOffset = 24U;
constexpr uint32_t kSegmentCreationUptimeOffset = 32U;
constexpr uint32_t kSegmentBootIdOffset = 40U;
constexpr uint32_t kSegmentRecordSizeOffset = 44U;
constexpr uint32_t kSegmentRecordsPerSegmentOffset = 48U;
constexpr uint32_t kSegmentPhysicalIndexOffset = 50U;
constexpr uint32_t kSegmentFlagsOffset = 52U;
constexpr uint32_t kSegmentReservedOffset = 56U;
constexpr uint32_t kSegmentReservedLength = 64U;

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
        static_cast<uint8_t>(value >> (index * 8U));
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
  for (uint8_t index = 0U; index < 8U; ++index) {
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  }
  return value;
}

bool bytesAreZero(const uint8_t* bytes, uint32_t length) {
  for (uint32_t index = 0U; index < length; ++index) {
    if (bytes[index] != 0U) {
      return false;
    }
  }
  return true;
}

bool persistentStateValid(uint8_t state) {
  return state <=
      static_cast<uint8_t>(PersistentSessionState::ErrorIncomplete);
}

bool synchronizationStateValid(uint8_t state) {
  return state <=
      static_cast<uint8_t>(SessionSynchronizationState::SyncError);
}

bool physicalIndexValidOrSentinel(uint16_t index) {
  return index < SESSION_DATA_SEGMENT_COUNT ||
         index == SESSION_INVALID_PHYSICAL_INDEX;
}

bool metadataTimeAnchorsValid(uint32_t flags,
                              uint64_t startWallClockUnixMs,
                              uint64_t endWallClockUnixMs) {
  const bool startValid =
      (flags & SessionMetadataStartWallTimeValid) != 0U;
  const bool endValid =
      (flags & SessionMetadataEndWallTimeValid) != 0U;
  if ((startValid
           ? !HistoricalTime::isPlausibleUnixEpochMs(startWallClockUnixMs)
           : startWallClockUnixMs != 0U) ||
      (endValid
           ? !HistoricalTime::isPlausibleUnixEpochMs(endWallClockUnixMs)
           : endWallClockUnixMs != 0U)) {
    return false;
  }
  return !startValid || !endValid ||
         endWallClockUnixMs >= startWallClockUnixMs;
}

bool metadataEqual(const PersistentSessionMetadata& left,
                   const PersistentSessionMetadata& right) {
  return left.generation == right.generation &&
         left.sourceMetadataGeneration == right.sourceMetadataGeneration &&
         left.sessionId == right.sessionId && left.state == right.state &&
         left.synchronizationState == right.synchronizationState &&
         left.flags == right.flags &&
         left.selectedFlushIntervalSeconds ==
             right.selectedFlushIntervalSeconds &&
         left.bootId == right.bootId &&
         left.startUptimeUs == right.startUptimeUs &&
         left.endUptimeUs == right.endUptimeUs &&
         left.startWallClockUnixMs == right.startWallClockUnixMs &&
         left.endWallClockUnixMs == right.endWallClockUnixMs &&
         left.sessionStartLogicalIndex == right.sessionStartLogicalIndex &&
         left.firstRetainedLogicalIndex == right.firstRetainedLogicalIndex &&
         left.lastRetainedLogicalIndex == right.lastRetainedLogicalIndex &&
         left.nextGlobalLogicalIndex == right.nextGlobalLogicalIndex &&
         left.totalStoredRecords == right.totalStoredRecords &&
         left.retainedRecordCount == right.retainedRecordCount &&
         left.overwrittenRecordCount == right.overwrittenRecordCount &&
         left.droppedRecordCount == right.droppedRecordCount &&
         left.nextSessionId == right.nextSessionId &&
         left.nextSegmentSequence == right.nextSegmentSequence &&
         left.firstStm32Sequence == right.firstStm32Sequence &&
         left.lastStm32Sequence == right.lastStm32Sequence &&
         left.oldestPhysicalSegment == right.oldestPhysicalSegment &&
         left.oldestPhysicalSlot == right.oldestPhysicalSlot &&
         left.nextWritePhysicalSegment == right.nextWritePhysicalSegment &&
         left.nextWriteSlot == right.nextWriteSlot;
}

}  // namespace

SessionStorageFormatError SessionStorageFormats::encodeMetadata(
    uint8_t* destination,
    uint32_t destinationLength,
    const PersistentSessionMetadata& metadata) {
  if (destination == nullptr) {
    return SessionStorageFormatError::NullArgument;
  }
  if (destinationLength < SESSION_METADATA_ENTRY_BYTES) {
    return SessionStorageFormatError::BufferTooShort;
  }
  if (!persistentStateValid(static_cast<uint8_t>(metadata.state)) ||
      !synchronizationStateValid(
          static_cast<uint8_t>(metadata.synchronizationState)) ||
      metadata.sourceMetadataGeneration > metadata.generation) {
    return SessionStorageFormatError::BadState;
  }
  if ((metadata.flags & ~SESSION_METADATA_KNOWN_FLAGS) != 0U) {
    return SessionStorageFormatError::BadFlags;
  }
  if (!metadataTimeAnchorsValid(metadata.flags,
                                metadata.startWallClockUnixMs,
                                metadata.endWallClockUnixMs)) {
    return SessionStorageFormatError::BadTimeAnchor;
  }
  if (!physicalIndexValidOrSentinel(metadata.oldestPhysicalSegment) ||
      !physicalIndexValidOrSentinel(metadata.nextWritePhysicalSegment) ||
      (metadata.oldestPhysicalSlot >= SESSION_RECORDS_PER_SEGMENT &&
       metadata.oldestPhysicalSlot != SESSION_INVALID_PHYSICAL_INDEX) ||
      (metadata.nextWriteSlot >= SESSION_RECORDS_PER_SEGMENT &&
       metadata.nextWriteSlot != SESSION_INVALID_PHYSICAL_INDEX)) {
    return SessionStorageFormatError::BadGeometry;
  }

  memset(destination, 0, SESSION_METADATA_ENTRY_BYTES);
  destination[0] = 'P';
  destination[1] = 'Q';
  destination[2] = 'M';
  destination[3] = 'D';
  writeLe16(destination + kMetadataVersionOffset,
            SESSION_METADATA_FORMAT_VERSION);
  writeLe16(destination + kMetadataLengthOffset,
            static_cast<uint16_t>(SESSION_METADATA_ENTRY_BYTES));
  writeLe64(destination + kMetadataGenerationOffset, metadata.generation);
  writeLe64(destination + kMetadataSessionIdOffset, metadata.sessionId);
  destination[kMetadataStateOffset] = static_cast<uint8_t>(metadata.state);
  destination[kMetadataSyncStateOffset] =
      static_cast<uint8_t>(metadata.synchronizationState);
  writeLe32(destination + kMetadataFlagsOffset, metadata.flags);
  writeLe32(destination + kMetadataFlushIntervalOffset,
            metadata.selectedFlushIntervalSeconds);
  writeLe32(destination + kMetadataBootIdOffset, metadata.bootId);
  writeLe64(destination + kMetadataStartUptimeOffset,
            metadata.startUptimeUs);
  writeLe64(destination + kMetadataEndUptimeOffset, metadata.endUptimeUs);
  writeLe64(destination + kMetadataStartWallTimeOffset,
            metadata.startWallClockUnixMs);
  writeLe64(destination + kMetadataEndWallTimeOffset,
            metadata.endWallClockUnixMs);
  writeLe64(destination + kMetadataSessionStartLogicalOffset,
            metadata.sessionStartLogicalIndex);
  writeLe64(destination + kMetadataFirstRetainedLogicalOffset,
            metadata.firstRetainedLogicalIndex);
  writeLe64(destination + kMetadataLastRetainedLogicalOffset,
            metadata.lastRetainedLogicalIndex);
  writeLe64(destination + kMetadataNextLogicalOffset,
            metadata.nextGlobalLogicalIndex);
  writeLe64(destination + kMetadataTotalStoredOffset,
            metadata.totalStoredRecords);
  writeLe64(destination + kMetadataRetainedCountOffset,
            metadata.retainedRecordCount);
  writeLe64(destination + kMetadataOverwrittenCountOffset,
            metadata.overwrittenRecordCount);
  writeLe64(destination + kMetadataDroppedCountOffset,
            metadata.droppedRecordCount);
  writeLe64(destination + kMetadataNextSessionIdOffset,
            metadata.nextSessionId);
  writeLe64(destination + kMetadataNextSegmentSequenceOffset,
            metadata.nextSegmentSequence);
  writeLe32(destination + kMetadataFirstStm32SequenceOffset,
            metadata.firstStm32Sequence);
  writeLe32(destination + kMetadataLastStm32SequenceOffset,
            metadata.lastStm32Sequence);
  writeLe16(destination + kMetadataOldestPhysicalSegmentOffset,
            metadata.oldestPhysicalSegment);
  writeLe16(destination + kMetadataOldestSlotOffset,
            metadata.oldestPhysicalSlot);
  writeLe16(destination + kMetadataNextWriteSegmentOffset,
            metadata.nextWritePhysicalSegment);
  writeLe16(destination + kMetadataNextWriteSlotOffset,
            metadata.nextWriteSlot);
  writeLe32(destination + kMetadataRecordSizeOffset, FLASH_RECORD_BYTES);
  writeLe16(destination + kMetadataRecordsPerSegmentOffset,
            SESSION_RECORDS_PER_SEGMENT);
  writeLe16(destination + kMetadataSegmentHeaderSizeOffset,
            SESSION_SEGMENT_HEADER_BYTES);
  writeLe16(destination + kMetadataDataSegmentCountOffset,
            SESSION_DATA_SEGMENT_COUNT);
  writeLe32(destination + kMetadataMaximumRecordsOffset,
            SESSION_MAX_RETAINED_RECORDS);
  writeLe64(destination + kMetadataSourceGenerationOffset,
            metadata.sourceMetadataGeneration);

  writeLe32(destination + SESSION_METADATA_CRC_OFFSET,
            crc32c(destination, SESSION_METADATA_CRC_OFFSET));
  writeLe32(destination + SESSION_METADATA_COMMIT_OFFSET,
            SESSION_METADATA_COMMIT_MARKER);
  return SessionStorageFormatError::Ok;
}

SessionStorageFormatError SessionStorageFormats::validateMetadata(
    const uint8_t* source,
    uint32_t sourceLength) {
  if (source == nullptr) {
    return SessionStorageFormatError::NullArgument;
  }
  if (sourceLength < SESSION_METADATA_ENTRY_BYTES) {
    return SessionStorageFormatError::BufferTooShort;
  }
  if (source[kMetadataMagicOffset] != 'P' ||
      source[kMetadataMagicOffset + 1U] != 'Q' ||
      source[kMetadataMagicOffset + 2U] != 'M' ||
      source[kMetadataMagicOffset + 3U] != 'D') {
    return SessionStorageFormatError::BadMagic;
  }
  const uint16_t formatVersion =
      readLe16(source + kMetadataVersionOffset);
  if (formatVersion != SESSION_METADATA_LEGACY_FORMAT_VERSION &&
      formatVersion != SESSION_METADATA_FORMAT_VERSION) {
    return SessionStorageFormatError::UnsupportedVersion;
  }
  if (readLe16(source + kMetadataLengthOffset) !=
      SESSION_METADATA_ENTRY_BYTES) {
    return SessionStorageFormatError::BadLength;
  }
  const uint8_t synchronizationState = source[kMetadataSyncStateOffset];
  const bool legacySynchronizationStateValid =
      formatVersion != SESSION_METADATA_LEGACY_FORMAT_VERSION ||
      synchronizationState <=
          static_cast<uint8_t>(SessionSynchronizationState::Synced);
  if (!persistentStateValid(source[kMetadataStateOffset]) ||
      !synchronizationStateValid(synchronizationState) ||
      !legacySynchronizationStateValid) {
    return SessionStorageFormatError::BadState;
  }
  const uint32_t metadataFlags =
      readLe32(source + kMetadataFlagsOffset);
  if ((metadataFlags & ~SESSION_METADATA_KNOWN_FLAGS) != 0U) {
    return SessionStorageFormatError::BadFlags;
  }
  if (!metadataTimeAnchorsValid(
          metadataFlags,
          readLe64(source + kMetadataStartWallTimeOffset),
          readLe64(source + kMetadataEndWallTimeOffset))) {
    return SessionStorageFormatError::BadTimeAnchor;
  }
  const bool sourceGenerationValid =
      (formatVersion != SESSION_METADATA_LEGACY_FORMAT_VERSION ||
       bytesAreZero(source + kMetadataSourceGenerationOffset, 8U)) &&
      readLe64(source + kMetadataSourceGenerationOffset) <=
          readLe64(source + kMetadataGenerationOffset);
  if (!bytesAreZero(source + kMetadataReserved0Offset, 2U) ||
      !bytesAreZero(source + kMetadataReserved1Offset, 2U) ||
      !sourceGenerationValid ||
      !bytesAreZero(source + kMetadataReservedBytesOffset,
                    kMetadataReservedBytesLength)) {
    return SessionStorageFormatError::NonZeroReserved;
  }
  if (readLe32(source + kMetadataRecordSizeOffset) != FLASH_RECORD_BYTES ||
      readLe16(source + kMetadataRecordsPerSegmentOffset) !=
          SESSION_RECORDS_PER_SEGMENT ||
      readLe16(source + kMetadataSegmentHeaderSizeOffset) !=
          SESSION_SEGMENT_HEADER_BYTES ||
      readLe16(source + kMetadataDataSegmentCountOffset) !=
          SESSION_DATA_SEGMENT_COUNT ||
      readLe32(source + kMetadataMaximumRecordsOffset) !=
          SESSION_MAX_RETAINED_RECORDS) {
    return SessionStorageFormatError::BadGeometry;
  }
  const uint16_t oldestSegment =
      readLe16(source + kMetadataOldestPhysicalSegmentOffset);
  const uint16_t oldestSlot =
      readLe16(source + kMetadataOldestSlotOffset);
  const uint16_t writeSegment =
      readLe16(source + kMetadataNextWriteSegmentOffset);
  const uint16_t writeSlot = readLe16(source + kMetadataNextWriteSlotOffset);
  if (!physicalIndexValidOrSentinel(oldestSegment) ||
      !physicalIndexValidOrSentinel(writeSegment) ||
      (oldestSlot >= SESSION_RECORDS_PER_SEGMENT &&
       oldestSlot != SESSION_INVALID_PHYSICAL_INDEX) ||
      (writeSlot >= SESSION_RECORDS_PER_SEGMENT &&
       writeSlot != SESSION_INVALID_PHYSICAL_INDEX)) {
    return SessionStorageFormatError::BadGeometry;
  }
  if (readLe32(source + SESSION_METADATA_COMMIT_OFFSET) !=
      SESSION_METADATA_COMMIT_MARKER) {
    return SessionStorageFormatError::MissingCommitMarker;
  }
  if (readLe32(source + SESSION_METADATA_CRC_OFFSET) !=
      crc32c(source, SESSION_METADATA_CRC_OFFSET)) {
    return SessionStorageFormatError::BadCrc;
  }
  return SessionStorageFormatError::Ok;
}

SessionStorageFormatError SessionStorageFormats::decodeMetadata(
    const uint8_t* source,
    uint32_t sourceLength,
    PersistentSessionMetadata& metadata) {
  const SessionStorageFormatError validation =
      validateMetadata(source, sourceLength);
  if (validation != SessionStorageFormatError::Ok) {
    return validation;
  }

  PersistentSessionMetadata decoded{};
  const uint16_t formatVersion =
      readLe16(source + kMetadataVersionOffset);
  decoded.generation = readLe64(source + kMetadataGenerationOffset);
  decoded.sourceMetadataGeneration =
      formatVersion == SESSION_METADATA_LEGACY_FORMAT_VERSION
          ? 0U
          : readLe64(source + kMetadataSourceGenerationOffset);
  decoded.sessionId = readLe64(source + kMetadataSessionIdOffset);
  decoded.state = static_cast<PersistentSessionState>(
      source[kMetadataStateOffset]);
  decoded.synchronizationState =
      static_cast<SessionSynchronizationState>(
          source[kMetadataSyncStateOffset]);
  decoded.flags = readLe32(source + kMetadataFlagsOffset);
  decoded.selectedFlushIntervalSeconds =
      readLe32(source + kMetadataFlushIntervalOffset);
  decoded.bootId = readLe32(source + kMetadataBootIdOffset);
  decoded.startUptimeUs = readLe64(source + kMetadataStartUptimeOffset);
  decoded.endUptimeUs = readLe64(source + kMetadataEndUptimeOffset);
  decoded.startWallClockUnixMs =
      readLe64(source + kMetadataStartWallTimeOffset);
  decoded.endWallClockUnixMs =
      readLe64(source + kMetadataEndWallTimeOffset);
  decoded.sessionStartLogicalIndex =
      readLe64(source + kMetadataSessionStartLogicalOffset);
  decoded.firstRetainedLogicalIndex =
      readLe64(source + kMetadataFirstRetainedLogicalOffset);
  decoded.lastRetainedLogicalIndex =
      readLe64(source + kMetadataLastRetainedLogicalOffset);
  decoded.nextGlobalLogicalIndex =
      readLe64(source + kMetadataNextLogicalOffset);
  decoded.totalStoredRecords =
      readLe64(source + kMetadataTotalStoredOffset);
  decoded.retainedRecordCount =
      readLe64(source + kMetadataRetainedCountOffset);
  decoded.overwrittenRecordCount =
      readLe64(source + kMetadataOverwrittenCountOffset);
  decoded.droppedRecordCount =
      readLe64(source + kMetadataDroppedCountOffset);
  decoded.nextSessionId = readLe64(source + kMetadataNextSessionIdOffset);
  decoded.nextSegmentSequence =
      readLe64(source + kMetadataNextSegmentSequenceOffset);
  decoded.firstStm32Sequence =
      readLe32(source + kMetadataFirstStm32SequenceOffset);
  decoded.lastStm32Sequence =
      readLe32(source + kMetadataLastStm32SequenceOffset);
  decoded.oldestPhysicalSegment =
      readLe16(source + kMetadataOldestPhysicalSegmentOffset);
  decoded.oldestPhysicalSlot =
      readLe16(source + kMetadataOldestSlotOffset);
  decoded.nextWritePhysicalSegment =
      readLe16(source + kMetadataNextWriteSegmentOffset);
  decoded.nextWriteSlot = readLe16(source + kMetadataNextWriteSlotOffset);
  metadata = decoded;
  return SessionStorageFormatError::Ok;
}

MetadataSelectionResult SessionStorageFormats::selectMetadataCopies(
    const uint8_t* copyA,
    uint32_t copyALength,
    const uint8_t* copyB,
    uint32_t copyBLength,
    PersistentSessionMetadata& selected,
    bool& copyAValid,
    bool& copyBValid) {
  PersistentSessionMetadata metadataA{};
  PersistentSessionMetadata metadataB{};
  copyAValid = decodeMetadata(copyA, copyALength, metadataA) ==
      SessionStorageFormatError::Ok;
  copyBValid = decodeMetadata(copyB, copyBLength, metadataB) ==
      SessionStorageFormatError::Ok;

  if (!copyAValid && !copyBValid) {
    return MetadataSelectionResult::NoneValid;
  }
  if (copyAValid && !copyBValid) {
    selected = metadataA;
    return MetadataSelectionResult::CopyA;
  }
  if (!copyAValid && copyBValid) {
    selected = metadataB;
    return MetadataSelectionResult::CopyB;
  }
  if (metadataA.generation == metadataB.generation) {
    if (!metadataEqual(metadataA, metadataB)) {
      return MetadataSelectionResult::Conflict;
    }
    selected = metadataA;
    return MetadataSelectionResult::CopyA;
  }
  if (metadataA.generation > metadataB.generation) {
    selected = metadataA;
    return MetadataSelectionResult::CopyA;
  }
  selected = metadataB;
  return MetadataSelectionResult::CopyB;
}

SessionStorageFormatError SessionStorageFormats::encodeSegmentHeader(
    uint8_t* destination,
    uint32_t destinationLength,
    const SessionSegmentHeader& header) {
  if (destination == nullptr) {
    return SessionStorageFormatError::NullArgument;
  }
  if (destinationLength < SESSION_SEGMENT_HEADER_BYTES) {
    return SessionStorageFormatError::BufferTooShort;
  }
  if (header.segmentSequence == 0U || header.sessionId == 0U ||
      header.physicalSegmentIndex >= SESSION_DATA_SEGMENT_COUNT) {
    return SessionStorageFormatError::BadGeometry;
  }

  memset(destination, 0, SESSION_SEGMENT_HEADER_BYTES);
  destination[0] = 'P';
  destination[1] = 'Q';
  destination[2] = 'S';
  destination[3] = 'G';
  writeLe16(destination + kSegmentVersionOffset,
            SESSION_SEGMENT_HEADER_FORMAT_VERSION);
  writeLe16(destination + kSegmentLengthOffset,
            SESSION_SEGMENT_HEADER_BYTES);
  writeLe64(destination + kSegmentSequenceOffset,
            header.segmentSequence);
  writeLe64(destination + kSegmentSessionIdOffset, header.sessionId);
  writeLe64(destination + kSegmentFirstLogicalOffset,
            header.firstLogicalRecordIndex);
  writeLe64(destination + kSegmentCreationUptimeOffset,
            header.creationUptimeUs);
  writeLe32(destination + kSegmentBootIdOffset, header.bootId);
  writeLe32(destination + kSegmentRecordSizeOffset, FLASH_RECORD_BYTES);
  writeLe16(destination + kSegmentRecordsPerSegmentOffset,
            SESSION_RECORDS_PER_SEGMENT);
  writeLe16(destination + kSegmentPhysicalIndexOffset,
            header.physicalSegmentIndex);
  writeLe32(destination + kSegmentFlagsOffset, header.flags);
  writeLe32(destination + SESSION_SEGMENT_HEADER_CRC_OFFSET,
            crc32c(destination, SESSION_SEGMENT_HEADER_CRC_OFFSET));
  writeLe32(destination + SESSION_SEGMENT_HEADER_COMMIT_OFFSET,
            SESSION_SEGMENT_HEADER_COMMIT_MARKER);
  return SessionStorageFormatError::Ok;
}

SessionStorageFormatError SessionStorageFormats::validateSegmentHeader(
    const uint8_t* source,
    uint32_t sourceLength,
    uint16_t expectedPhysicalSegment) {
  if (source == nullptr) {
    return SessionStorageFormatError::NullArgument;
  }
  if (sourceLength < SESSION_SEGMENT_HEADER_BYTES) {
    return SessionStorageFormatError::BufferTooShort;
  }
  if (source[kSegmentMagicOffset] != 'P' ||
      source[kSegmentMagicOffset + 1U] != 'Q' ||
      source[kSegmentMagicOffset + 2U] != 'S' ||
      source[kSegmentMagicOffset + 3U] != 'G') {
    return SessionStorageFormatError::BadMagic;
  }
  if (readLe16(source + kSegmentVersionOffset) !=
      SESSION_SEGMENT_HEADER_FORMAT_VERSION) {
    return SessionStorageFormatError::UnsupportedVersion;
  }
  if (readLe16(source + kSegmentLengthOffset) !=
      SESSION_SEGMENT_HEADER_BYTES) {
    return SessionStorageFormatError::BadLength;
  }
  const uint16_t physical =
      readLe16(source + kSegmentPhysicalIndexOffset);
  if (readLe64(source + kSegmentSequenceOffset) == 0U ||
      readLe64(source + kSegmentSessionIdOffset) == 0U ||
      readLe32(source + kSegmentRecordSizeOffset) != FLASH_RECORD_BYTES ||
      readLe16(source + kSegmentRecordsPerSegmentOffset) !=
          SESSION_RECORDS_PER_SEGMENT ||
      physical >= SESSION_DATA_SEGMENT_COUNT ||
      (expectedPhysicalSegment != SESSION_INVALID_PHYSICAL_INDEX &&
       physical != expectedPhysicalSegment)) {
    return SessionStorageFormatError::BadGeometry;
  }
  if (!bytesAreZero(source + kSegmentReservedOffset,
                    kSegmentReservedLength)) {
    return SessionStorageFormatError::NonZeroReserved;
  }
  if (readLe32(source + SESSION_SEGMENT_HEADER_COMMIT_OFFSET) !=
      SESSION_SEGMENT_HEADER_COMMIT_MARKER) {
    return SessionStorageFormatError::MissingCommitMarker;
  }
  if (readLe32(source + SESSION_SEGMENT_HEADER_CRC_OFFSET) !=
      crc32c(source, SESSION_SEGMENT_HEADER_CRC_OFFSET)) {
    return SessionStorageFormatError::BadCrc;
  }
  return SessionStorageFormatError::Ok;
}

SessionStorageFormatError SessionStorageFormats::decodeSegmentHeader(
    const uint8_t* source,
    uint32_t sourceLength,
    SessionSegmentHeader& header,
    uint16_t expectedPhysicalSegment) {
  const SessionStorageFormatError validation = validateSegmentHeader(
      source, sourceLength, expectedPhysicalSegment);
  if (validation != SessionStorageFormatError::Ok) {
    return validation;
  }
  SessionSegmentHeader decoded{};
  decoded.segmentSequence = readLe64(source + kSegmentSequenceOffset);
  decoded.sessionId = readLe64(source + kSegmentSessionIdOffset);
  decoded.firstLogicalRecordIndex =
      readLe64(source + kSegmentFirstLogicalOffset);
  decoded.creationUptimeUs =
      readLe64(source + kSegmentCreationUptimeOffset);
  decoded.bootId = readLe32(source + kSegmentBootIdOffset);
  decoded.physicalSegmentIndex =
      readLe16(source + kSegmentPhysicalIndexOffset);
  decoded.flags = readLe32(source + kSegmentFlagsOffset);
  header = decoded;
  return SessionStorageFormatError::Ok;
}

void SessionStorageFormats::sortSegmentSummaries(
    StorageSegmentSummary* summaries,
    uint16_t count) {
  if (summaries == nullptr) {
    return;
  }
  for (uint16_t index = 1U; index < count; ++index) {
    const StorageSegmentSummary value = summaries[index];
    uint16_t position = index;
    while (position > 0U &&
           summaries[position - 1U].segmentSequence >
               value.segmentSequence) {
      summaries[position] = summaries[position - 1U];
      --position;
    }
    summaries[position] = value;
  }
}

bool SessionStorageFormats::findRecordLocation(
    const StorageSegmentSummary* summaries,
    uint16_t count,
    uint64_t sessionId,
    uint64_t logicalRecordIndex,
    uint16_t& physicalSegmentIndex,
    uint16_t& slotIndex) {
  if (summaries == nullptr) {
    return false;
  }
  bool found = false;
  uint64_t selectedSegmentSequence = 0U;
  for (uint16_t index = 0U; index < count; ++index) {
    const StorageSegmentSummary& summary = summaries[index];
    if (!summary.valid || summary.sessionId != sessionId ||
        logicalRecordIndex < summary.firstLogicalRecordIndex) {
      continue;
    }
    const uint64_t delta =
        logicalRecordIndex - summary.firstLogicalRecordIndex;
    if (delta >= SESSION_RECORDS_PER_SEGMENT) {
      continue;
    }
    const uint16_t candidateSlot = static_cast<uint16_t>(delta);
    if ((summary.validRecordMask & (1U << candidateSlot)) == 0U) {
      continue;
    }
    if (found && summary.segmentSequence <= selectedSegmentSequence) {
      continue;
    }
    found = true;
    selectedSegmentSequence = summary.segmentSequence;
    physicalSegmentIndex = summary.physicalSegmentIndex;
    slotIndex = candidateSlot;
  }
  return found;
}

ContiguousSuffixResult SessionStorageFormats::newestContiguousSuffix(
    const StorageRecordLocation* records,
    uint32_t count) {
  ContiguousSuffixResult result{};
  if (records == nullptr || count == 0U) {
    return result;
  }
  result.startIndex = 0U;
  result.count = count;
  for (uint32_t index = 1U; index < count; ++index) {
    const uint64_t previous = records[index - 1U].logicalRecordIndex;
    if (previous == UINT64_MAX ||
        records[index].logicalRecordIndex != previous + 1U) {
      result.startIndex = index;
      result.count = count - index;
      result.gapOrDuplicateDetected = true;
    }
  }
  return result;
}

uint16_t SessionStorageFormats::nextPhysicalSegment(
    uint16_t currentPhysicalSegment) {
  if (currentPhysicalSegment >= SESSION_DATA_SEGMENT_COUNT) {
    return SESSION_INVALID_PHYSICAL_INDEX;
  }
  return static_cast<uint16_t>(
      (currentPhysicalSegment + 1U) % SESSION_DATA_SEGMENT_COUNT);
}

uint32_t SessionStorageFormats::crc32c(const uint8_t* data,
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

#if SESSION_STORAGE_FORMAT_HOST_TEST

#include <stdio.h>

namespace {

uint32_t failures = 0U;

void expect(bool condition, const char* name) {
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", name);
  }
}

PersistentSessionMetadata sampleMetadata(uint64_t generation) {
  PersistentSessionMetadata metadata{};
  metadata.generation = generation;
  metadata.sourceMetadataGeneration = 1U;
  metadata.sessionId = 7U;
  metadata.state = PersistentSessionState::Finalized;
  metadata.synchronizationState =
      SessionSynchronizationState::NotSynced;
  metadata.flags = SessionMetadataHasRetainedRecords |
                   SessionMetadataStartWallTimeValid |
                   SessionMetadataEndWallTimeValid |
                   SessionMetadataStorageTruncated |
                   SessionMetadataFinalized;
  metadata.selectedFlushIntervalSeconds = 10U;
  metadata.bootId = 0x12345678U;
  metadata.startUptimeUs = 1000U;
  metadata.endUptimeUs = 2000U;
  metadata.startWallClockUnixMs = 1704067200000ULL;
  metadata.endWallClockUnixMs = 1704067201000ULL;
  metadata.sessionStartLogicalIndex = 500U;
  metadata.firstRetainedLogicalIndex = 514U;
  metadata.lastRetainedLogicalIndex = 520U;
  metadata.nextGlobalLogicalIndex = 521U;
  metadata.totalStoredRecords = 21U;
  metadata.retainedRecordCount = 7U;
  metadata.overwrittenRecordCount = 14U;
  metadata.droppedRecordCount = 3U;
  metadata.nextSessionId = 8U;
  metadata.nextSegmentSequence = 99U;
  metadata.firstStm32Sequence = 100U;
  metadata.lastStm32Sequence = 120U;
  metadata.oldestPhysicalSegment = 190U;
  metadata.oldestPhysicalSlot = 7U;
  metadata.nextWritePhysicalSegment = 0U;
  metadata.nextWriteSlot = 7U;
  return metadata;
}

void convertToLegacyV1(uint8_t* encodedMetadata) {
  writeLe16(encodedMetadata + kMetadataVersionOffset,
            SESSION_METADATA_LEGACY_FORMAT_VERSION);
  memset(encodedMetadata + kMetadataSourceGenerationOffset, 0, 8U);
  writeLe32(encodedMetadata + SESSION_METADATA_CRC_OFFSET,
            SessionStorageFormats::crc32c(
                encodedMetadata, SESSION_METADATA_CRC_OFFSET));
}

}  // namespace

int main() {
  uint8_t metadataA[SESSION_METADATA_ENTRY_BYTES]{};
  uint8_t metadataB[SESSION_METADATA_ENTRY_BYTES]{};
  uint8_t legacyMetadata[SESSION_METADATA_ENTRY_BYTES]{};
  PersistentSessionMetadata decoded{};
  PersistentSessionMetadata a = sampleMetadata(1U);
  PersistentSessionMetadata b = sampleMetadata(2U);

  expect(SessionStorageFormats::crc32c(
             reinterpret_cast<const uint8_t*>("123456789"), 9U) ==
             0xE3069283U,
         "CRC-32C known vector");
  expect(SessionStorageFormats::encodeMetadata(
             metadataA, sizeof(metadataA), a) ==
             SessionStorageFormatError::Ok,
         "metadata A encode");
  expect(SessionStorageFormats::encodeMetadata(
             metadataB, sizeof(metadataB), b) ==
             SessionStorageFormatError::Ok,
         "metadata B encode");
  expect(SessionStorageFormats::decodeMetadata(
             metadataB, sizeof(metadataB), decoded) ==
             SessionStorageFormatError::Ok && metadataEqual(decoded, b),
         "metadata v2 decode");
  expect(decoded.sourceMetadataGeneration == 1U,
         "metadata v2 source generation");
  expect(decoded.startWallClockUnixMs == a.startWallClockUnixMs &&
             decoded.endWallClockUnixMs == a.endWallClockUnixMs &&
             (decoded.flags & SessionMetadataStartWallTimeValid) != 0U &&
             (decoded.flags & SessionMetadataEndWallTimeValid) != 0U,
         "metadata wall-clock anchors round trip");
  PersistentSessionMetadata invalidAnchor = a;
  invalidAnchor.startWallClockUnixMs = 0U;
  expect(SessionStorageFormats::encodeMetadata(
             legacyMetadata, sizeof(legacyMetadata), invalidAnchor) ==
             SessionStorageFormatError::BadTimeAnchor,
         "invalid valid-start anchor rejected");
  invalidAnchor = a;
  invalidAnchor.endWallClockUnixMs = a.startWallClockUnixMs - 1U;
  expect(SessionStorageFormats::encodeMetadata(
             legacyMetadata, sizeof(legacyMetadata), invalidAnchor) ==
             SessionStorageFormatError::BadTimeAnchor,
         "end anchor before start rejected");

  expect(SessionStorageFormats::encodeMetadata(
             legacyMetadata, sizeof(legacyMetadata), a) ==
             SessionStorageFormatError::Ok,
         "legacy metadata preparation");
  convertToLegacyV1(legacyMetadata);
  PersistentSessionMetadata legacyDecoded{};
  expect(SessionStorageFormats::decodeMetadata(
             legacyMetadata, sizeof(legacyMetadata), legacyDecoded) ==
             SessionStorageFormatError::Ok &&
             legacyDecoded.generation == a.generation &&
             legacyDecoded.sourceMetadataGeneration == 0U,
         "legacy metadata v1 decode");

  PersistentSessionMetadata recovered = b;
  recovered.state = PersistentSessionState::RecoveredIncomplete;
  recovered.flags &= ~(SessionMetadataFinalized |
                       SessionMetadataEndWallTimeValid);
  recovered.flags |= SessionMetadataInterruptedRecovered |
                     SessionMetadataCounterPartial;
  recovered.endWallClockUnixMs = 0U;
  recovered.endUptimeUs = 0U;
  expect(SessionStorageFormats::encodeMetadata(
             metadataA, sizeof(metadataA), recovered) ==
             SessionStorageFormatError::Ok &&
             SessionStorageFormats::decodeMetadata(
                 metadataA, sizeof(metadataA), decoded) ==
             SessionStorageFormatError::Ok &&
             decoded.startWallClockUnixMs ==
                 recovered.startWallClockUnixMs &&
             (decoded.flags & SessionMetadataStartWallTimeValid) != 0U &&
             (decoded.flags & SessionMetadataEndWallTimeValid) == 0U,
         "recovery preserves committed start anchor without inventing end");
  SessionStorageFormats::encodeMetadata(metadataA, sizeof(metadataA), a);

  expect(static_cast<uint8_t>(SessionSynchronizationState::NotSynced) == 0U &&
             static_cast<uint8_t>(SessionSynchronizationState::Synced) == 1U &&
             static_cast<uint8_t>(SessionSynchronizationState::Uploading) == 2U &&
             static_cast<uint8_t>(SessionSynchronizationState::SyncError) == 3U,
         "synchronization-state persistent values");
  bool synchronizationRoundTrip = true;
  for (uint8_t state = 0U; state <= 3U; ++state) {
    PersistentSessionMetadata transitionMetadata = a;
    transitionMetadata.synchronizationState =
        static_cast<SessionSynchronizationState>(state);
    uint8_t transitionBytes[SESSION_METADATA_ENTRY_BYTES]{};
    PersistentSessionMetadata transitionDecoded{};
    if (SessionStorageFormats::encodeMetadata(
            transitionBytes,
            sizeof(transitionBytes),
            transitionMetadata) != SessionStorageFormatError::Ok ||
        SessionStorageFormats::decodeMetadata(
            transitionBytes,
            sizeof(transitionBytes),
            transitionDecoded) != SessionStorageFormatError::Ok ||
        transitionDecoded.synchronizationState !=
            transitionMetadata.synchronizationState ||
        transitionDecoded.sourceMetadataGeneration != 1U) {
      synchronizationRoundTrip = false;
      break;
    }
  }
  expect(synchronizationRoundTrip,
         "synchronization states preserve source generation");
  PersistentSessionMetadata futureSource = b;
  futureSource.sourceMetadataGeneration = b.generation + 1U;
  expect(SessionStorageFormats::encodeMetadata(
             legacyMetadata, sizeof(legacyMetadata), futureSource) ==
             SessionStorageFormatError::BadState,
         "future source generation rejected");
  convertToLegacyV1(legacyMetadata);

  bool aValid = false;
  bool bValid = false;
  expect(SessionStorageFormats::selectMetadataCopies(
             legacyMetadata, sizeof(legacyMetadata),
             metadataB, sizeof(metadataB),
             decoded, aValid, bValid) == MetadataSelectionResult::CopyB &&
             aValid && bValid && decoded.generation == 2U,
         "mixed v1/v2 metadata selection");
  expect(SessionStorageFormats::selectMetadataCopies(
             metadataA, sizeof(metadataA), metadataB, sizeof(metadataB),
             decoded, aValid, bValid) == MetadataSelectionResult::CopyB &&
             aValid && bValid && decoded.generation == 2U,
         "A/B newest generation");

  metadataB[SESSION_METADATA_COMMIT_OFFSET] = 0U;
  expect(SessionStorageFormats::selectMetadataCopies(
             metadataA, sizeof(metadataA), metadataB, sizeof(metadataB),
             decoded, aValid, bValid) == MetadataSelectionResult::CopyA &&
             aValid && !bValid,
         "A/B missing commit fallback");
  expect(SessionStorageFormats::validateMetadata(
             metadataB, sizeof(metadataB)) ==
             SessionStorageFormatError::MissingCommitMarker,
         "metadata missing commit");

  SessionStorageFormats::encodeMetadata(metadataB, sizeof(metadataB), a);
  metadataB[kMetadataBootIdOffset] ^= 1U;
  expect(SessionStorageFormats::validateMetadata(
             metadataB, sizeof(metadataB)) ==
             SessionStorageFormatError::BadCrc,
         "corrupted metadata CRC");

  PersistentSessionMetadata conflicting = a;
  conflicting.bootId++;
  SessionStorageFormats::encodeMetadata(
      metadataB, sizeof(metadataB), conflicting);
  expect(SessionStorageFormats::selectMetadataCopies(
             metadataA, sizeof(metadataA), metadataB, sizeof(metadataB),
             decoded, aValid, bValid) == MetadataSelectionResult::Conflict,
         "equal-generation conflict");

  uint8_t headerBytes[SESSION_SEGMENT_HEADER_BYTES]{};
  SessionSegmentHeader header{};
  header.segmentSequence = 10U;
  header.sessionId = 7U;
  header.firstLogicalRecordIndex = 100U;
  header.creationUptimeUs = 1234U;
  header.bootId = 0xAABBCCDDU;
  header.physicalSegmentIndex = 190U;
  expect(SessionStorageFormats::encodeSegmentHeader(
             headerBytes, sizeof(headerBytes), header) ==
             SessionStorageFormatError::Ok,
         "segment header encode");
  SessionSegmentHeader decodedHeader{};
  expect(SessionStorageFormats::decodeSegmentHeader(
             headerBytes, sizeof(headerBytes), decodedHeader, 190U) ==
             SessionStorageFormatError::Ok &&
             decodedHeader.segmentSequence == 10U &&
             decodedHeader.physicalSegmentIndex == 190U,
         "segment header decode");
  headerBytes[SESSION_SEGMENT_HEADER_COMMIT_OFFSET] = 0U;
  expect(SessionStorageFormats::validateSegmentHeader(
             headerBytes, sizeof(headerBytes), 190U) ==
             SessionStorageFormatError::MissingCommitMarker,
         "segment missing commit");

  StorageRecordLocation gapRecords[4] = {
      {10U, 1U, 5U, 0U}, {11U, 2U, 5U, 1U},
      {13U, 3U, 5U, 3U}, {14U, 4U, 5U, 4U}};
  const ContiguousSuffixResult suffix =
      SessionStorageFormats::newestContiguousSuffix(gapRecords, 4U);
  expect(suffix.startIndex == 2U && suffix.count == 2U &&
             suffix.gapOrDuplicateDetected,
         "newest contiguous suffix after record gap");

  StorageSegmentSummary summaries[2]{};
  summaries[0] = {true, 0U, 0x3FFFU, 11U, 7U, 114U};
  summaries[1] = {true, 190U, 0x3FFFU, 10U, 7U, 100U};
  SessionStorageFormats::sortSegmentSummaries(summaries, 2U);
  expect(summaries[0].physicalSegmentIndex == 190U &&
             summaries[1].physicalSegmentIndex == 0U,
         "segment sequence sort across physical wrap");
  uint16_t physical = SESSION_INVALID_PHYSICAL_INDEX;
  uint16_t slot = SESSION_INVALID_PHYSICAL_INDEX;
  bool readerMappingValid = true;
  for (uint64_t logical = 100U; logical <= 127U; ++logical) {
    if (!SessionStorageFormats::findRecordLocation(
            summaries, 2U, 7U, logical, physical, slot)) {
      readerMappingValid = false;
      break;
    }
    const uint16_t expectedPhysical = logical < 114U ? 190U : 0U;
    if (physical != expectedPhysical ||
        slot != static_cast<uint16_t>((logical - 100U) % 14U)) {
      readerMappingValid = false;
      break;
    }
  }
  expect(readerMappingValid, "chronological reader physical wrap mapping");
  StorageSegmentSummary overlapping[2]{};
  overlapping[0] = {true, 10U, 0U, 1U, 7U, 100U};
  overlapping[1] = {true, 11U, 1U, 2U, 7U, 100U};
  expect(SessionStorageFormats::findRecordLocation(
             overlapping, 2U, 7U, 100U, physical, slot) &&
             physical == 11U && slot == 0U,
         "newest valid overlapping record selected");
  expect(SessionStorageFormats::nextPhysicalSegment(190U) == 0U,
         "circular physical index wrap");
  expect(2675U - 2661U == 14U,
         "segment-granular overwritten accounting");

  uint8_t payload[FLASH_RECORD_PAYLOAD_BYTES]{};
  uint8_t record[FLASH_RECORD_BYTES]{};
  expect(FlashRecordCodec::encode(
             record, sizeof(record), payload, sizeof(payload), 7U, 100U,
             500U, 1U, 1U, 0U, 2U) == FlashRecordCodecError::Ok,
         "record encode for recovery test");
  memset(record + FLASH_RECORD_COMMIT_OFFSET, 0, 4U);
  expect(FlashRecordCodec::validate(record, sizeof(record)) ==
             FlashRecordCodecError::MissingCommitMarker,
         "record missing commit rejected");

  if (failures == 0U) {
    printf("Session storage format/math tests: PASS\n");
    return 0;
  }
  printf("Session storage format/math tests: FAIL (%u)\n", failures);
  return 1;
}

#endif
