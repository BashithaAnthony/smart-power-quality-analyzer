#pragma once

#include <stdint.h>

#include "FlashRecordCodec.h"

#ifndef SESSION_STORAGE_FORMAT_HOST_TEST
#define SESSION_STORAGE_FORMAT_HOST_TEST 0
#endif

constexpr uint32_t SESSION_METADATA_ENTRY_BYTES = 256U;
constexpr uint32_t SESSION_METADATA_CRC_OFFSET = 248U;
constexpr uint32_t SESSION_METADATA_COMMIT_OFFSET = 252U;
constexpr uint32_t SESSION_METADATA_COPY_A_OFFSET = 0x00000000U;
constexpr uint32_t SESSION_METADATA_COPY_B_OFFSET = 0x00001000U;
constexpr uint32_t SESSION_METADATA_SECTOR_BYTES = 0x00001000U;
constexpr uint16_t SESSION_METADATA_LEGACY_FORMAT_VERSION = 1U;
constexpr uint16_t SESSION_METADATA_FORMAT_VERSION = 2U;
constexpr uint32_t SESSION_METADATA_COMMIT_MARKER = 0x4D43444DU;

constexpr uint32_t SESSION_SEGMENT_HEADER_BYTES = 128U;
constexpr uint32_t SESSION_SEGMENT_HEADER_CRC_OFFSET = 120U;
constexpr uint32_t SESSION_SEGMENT_HEADER_COMMIT_OFFSET = 124U;
constexpr uint16_t SESSION_SEGMENT_HEADER_FORMAT_VERSION = 1U;
constexpr uint32_t SESSION_SEGMENT_HEADER_COMMIT_MARKER = 0x4D434753U;

constexpr uint16_t SESSION_INVALID_PHYSICAL_INDEX = 0xFFFFU;
constexpr uint16_t SESSION_DATA_SEGMENT_COUNT = 191U;
constexpr uint16_t SESSION_RECORDS_PER_SEGMENT = 14U;
constexpr uint32_t SESSION_MAX_RETAINED_RECORDS = 2674U;

// Metadata and segment headers use reflected CRC-32C (Castagnoli):
// polynomial 0x82F63B78, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF.

enum class PersistentSessionState : uint8_t {
  Empty = 0,
  Active,
  Stopping,
  Finalized,
  RecoveredIncomplete,
  ErrorIncomplete
};

enum class SessionSynchronizationState : uint8_t {
  NotSynced = 0,
  Synced = 1,
  Uploading = 2,
  SyncError = 3
};

enum SessionMetadataFlag : uint32_t {
  SessionMetadataHasRetainedRecords = 1U << 0U,
  SessionMetadataStartWallTimeValid = 1U << 1U,
  SessionMetadataEndWallTimeValid = 1U << 2U,
  SessionMetadataStorageTruncated = 1U << 3U,
  SessionMetadataInterruptedRecovered = 1U << 4U,
  SessionMetadataCounterPartial = 1U << 5U,
  SessionMetadataFinalized = 1U << 6U,
  SessionMetadataCorruptionOrGap = 1U << 7U
};

constexpr uint32_t SESSION_METADATA_KNOWN_FLAGS =
    SessionMetadataHasRetainedRecords |
    SessionMetadataStartWallTimeValid |
    SessionMetadataEndWallTimeValid |
    SessionMetadataStorageTruncated |
    SessionMetadataInterruptedRecovered |
    SessionMetadataCounterPartial |
    SessionMetadataFinalized |
    SessionMetadataCorruptionOrGap;

struct PersistentSessionMetadata {
  uint64_t generation;
  // Frozen when synchronization first changes state. This keeps the cloud
  // source generation stable while A/B metadata generations continue to
  // advance for Uploading, SyncError, and Synced transitions.
  uint64_t sourceMetadataGeneration;
  uint64_t sessionId;
  PersistentSessionState state;
  SessionSynchronizationState synchronizationState;
  uint32_t flags;
  uint32_t selectedFlushIntervalSeconds;
  uint32_t bootId;
  uint64_t startUptimeUs;
  uint64_t endUptimeUs;
  uint64_t startWallClockUnixMs;
  uint64_t endWallClockUnixMs;
  uint64_t sessionStartLogicalIndex;
  uint64_t firstRetainedLogicalIndex;
  uint64_t lastRetainedLogicalIndex;
  uint64_t nextGlobalLogicalIndex;
  uint64_t totalStoredRecords;
  uint64_t retainedRecordCount;
  uint64_t overwrittenRecordCount;
  uint64_t droppedRecordCount;
  uint64_t nextSessionId;
  uint64_t nextSegmentSequence;
  uint32_t firstStm32Sequence;
  uint32_t lastStm32Sequence;
  uint16_t oldestPhysicalSegment;
  uint16_t oldestPhysicalSlot;
  uint16_t nextWritePhysicalSegment;
  uint16_t nextWriteSlot;
};

struct SessionSegmentHeader {
  uint64_t segmentSequence;
  uint64_t sessionId;
  uint64_t firstLogicalRecordIndex;
  uint64_t creationUptimeUs;
  uint32_t bootId;
  uint16_t physicalSegmentIndex;
  uint32_t flags;
};

enum class SessionStorageFormatError : uint8_t {
  Ok = 0,
  NullArgument,
  BufferTooShort,
  BadMagic,
  UnsupportedVersion,
  BadLength,
  BadState,
  BadFlags,
  BadGeometry,
  NonZeroReserved,
  MissingCommitMarker,
  BadCrc,
  BadTimeAnchor
};

enum class MetadataSelectionResult : uint8_t {
  NoneValid = 0,
  CopyA,
  CopyB,
  Conflict
};

struct StorageSegmentSummary {
  bool valid;
  uint16_t physicalSegmentIndex;
  uint16_t validRecordMask;
  uint64_t segmentSequence;
  uint64_t sessionId;
  uint64_t firstLogicalRecordIndex;
};

struct StorageRecordLocation {
  uint64_t logicalRecordIndex;
  uint32_t stm32Sequence;
  uint16_t physicalSegmentIndex;
  uint16_t slotIndex;
};

struct ContiguousSuffixResult {
  uint32_t startIndex;
  uint32_t count;
  bool gapOrDuplicateDetected;
};

class SessionStorageFormats {
 public:
  static SessionStorageFormatError encodeMetadata(
      uint8_t* destination,
      uint32_t destinationLength,
      const PersistentSessionMetadata& metadata);
  static SessionStorageFormatError validateMetadata(
      const uint8_t* source,
      uint32_t sourceLength);
  static SessionStorageFormatError decodeMetadata(
      const uint8_t* source,
      uint32_t sourceLength,
      PersistentSessionMetadata& metadata);

  static MetadataSelectionResult selectMetadataCopies(
      const uint8_t* copyA,
      uint32_t copyALength,
      const uint8_t* copyB,
      uint32_t copyBLength,
      PersistentSessionMetadata& selected,
      bool& copyAValid,
      bool& copyBValid);

  static SessionStorageFormatError encodeSegmentHeader(
      uint8_t* destination,
      uint32_t destinationLength,
      const SessionSegmentHeader& header);
  static SessionStorageFormatError validateSegmentHeader(
      const uint8_t* source,
      uint32_t sourceLength,
      uint16_t expectedPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX);
  static SessionStorageFormatError decodeSegmentHeader(
      const uint8_t* source,
      uint32_t sourceLength,
      SessionSegmentHeader& header,
      uint16_t expectedPhysicalSegment = SESSION_INVALID_PHYSICAL_INDEX);

  static void sortSegmentSummaries(StorageSegmentSummary* summaries,
                                   uint16_t count);
  static bool findRecordLocation(const StorageSegmentSummary* summaries,
                                 uint16_t count,
                                 uint64_t sessionId,
                                 uint64_t logicalRecordIndex,
                                 uint16_t& physicalSegmentIndex,
                                 uint16_t& slotIndex);
  static ContiguousSuffixResult newestContiguousSuffix(
      const StorageRecordLocation* records,
      uint32_t count);
  static uint16_t nextPhysicalSegment(uint16_t currentPhysicalSegment);
  static uint32_t crc32c(const uint8_t* data, uint32_t length);
};

static_assert(SESSION_METADATA_COMMIT_OFFSET + 4U ==
                  SESSION_METADATA_ENTRY_BYTES,
              "Metadata commit marker must be the final four bytes");
static_assert(SESSION_SEGMENT_HEADER_COMMIT_OFFSET + 4U ==
                  SESSION_SEGMENT_HEADER_BYTES,
              "Segment commit marker must be the final four bytes");
static_assert(SESSION_DATA_SEGMENT_COUNT * SESSION_RECORDS_PER_SEGMENT ==
                  SESSION_MAX_RETAINED_RECORDS,
              "Stage 3 geometry must retain exactly 2674 records");
static_assert(static_cast<uint8_t>(SessionSynchronizationState::NotSynced) ==
                  0U &&
                  static_cast<uint8_t>(
                      SessionSynchronizationState::Synced) == 1U &&
                  static_cast<uint8_t>(
                      SessionSynchronizationState::Uploading) == 2U &&
                  static_cast<uint8_t>(
                      SessionSynchronizationState::SyncError) == 3U,
              "Persistent synchronization-state values must remain stable");
