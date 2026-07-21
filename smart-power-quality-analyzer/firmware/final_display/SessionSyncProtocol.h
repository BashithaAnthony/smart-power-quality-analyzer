#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef SESSION_SYNC_PROTOCOL_HOST_TEST
#define SESSION_SYNC_PROTOCOL_HOST_TEST 0
#endif

constexpr uint32_t SESSION_SYNC_LEGACY_MANIFEST_SCHEMA_VERSION = 1U;
constexpr uint32_t SESSION_SYNC_SCHEMA_VERSION = 2U;
constexpr uint32_t SESSION_SYNC_CHUNK_SCHEMA_VERSION = 1U;
constexpr uint32_t SESSION_SYNC_RECORDS_PER_CHUNK = 8U;
constexpr uint32_t SESSION_SYNC_RECORD_BYTES = 4432U;
constexpr uint32_t SESSION_SYNC_MAX_RAW_CHUNK_BYTES =
    SESSION_SYNC_RECORDS_PER_CHUNK * SESSION_SYNC_RECORD_BYTES;
constexpr uint32_t SESSION_SYNC_MAX_BASE64_CHARS = 47276U;
constexpr uint32_t SESSION_SYNC_BASE64_BUFFER_BYTES =
    SESSION_SYNC_MAX_BASE64_CHARS + 1U;
constexpr uint32_t SESSION_SYNC_JSON_BUFFER_BYTES = 49152U;
constexpr uint32_t SESSION_SYNC_SESSION_KEY_BYTES = 23U;
constexpr uint32_t SESSION_SYNC_CHUNK_KEY_BYTES = 7U;
constexpr uint32_t SESSION_SYNC_LEGACY_MANIFEST_CANONICAL_BYTES = 112U;
constexpr uint32_t SESSION_SYNC_MANIFEST_CANONICAL_BYTES = 144U;
constexpr uint32_t SESSION_SYNC_DEVICE_ID_BYTES = 16U;
constexpr uint32_t SESSION_SYNC_MAX_CHUNK_INDEX = 999999U;
constexpr uint64_t SESSION_SYNC_MIN_UNIX_EPOCH_MS = 1704067200000ULL;

static_assert(SESSION_SYNC_MAX_RAW_CHUNK_BYTES == 35456U,
              "Eight encoded records must occupy 35456 bytes");
static_assert(SESSION_SYNC_MAX_BASE64_CHARS ==
                  ((SESSION_SYNC_MAX_RAW_CHUNK_BYTES + 2U) / 3U) * 4U,
              "Maximum Base64 capacity must include normal padding");
static_assert(SESSION_SYNC_SESSION_KEY_BYTES >= 23U,
              "A session key must hold s_ plus UINT64_MAX and NUL");
static_assert(SESSION_SYNC_CHUNK_KEY_BYTES == 7U,
              "A chunk key must be six digits plus NUL");

enum class SessionSyncProtocolError : uint8_t {
  Ok = 0,
  NullArgument,
  BufferTooShort,
  InvalidDecimal,
  DecimalOverflow,
  ValueOutOfRange,
  ArithmeticOverflow,
  InvalidBase64Input,
  InvalidManifest,
  InvalidProgress
};

enum class SyncManifestPersistentState : uint8_t {
  Finalized = 1,
  RecoveredIncomplete = 2
};

enum class SyncManifestTimeSource : uint8_t {
  None = 0,
  Ntp = 1
};

struct SyncChunkBounds {
  uint32_t chunkIndex;
  uint64_t firstRecordOrdinal;
  uint32_t recordCount;
  uint32_t rawBytes;
};

// Immutable cloud-manifest identity. All fields are serialized explicitly;
// this object is never dumped as raw bytes. deviceId must be NUL-terminated
// with all unused bytes zero.
struct SyncManifestImmutable {
  uint32_t schemaVersion;
  char deviceId[SESSION_SYNC_DEVICE_ID_BYTES];
  uint64_t sessionId;
  SyncManifestPersistentState persistentState;
  bool truncated;
  bool recoveredIncomplete;
  bool countersPartial;
  char recordFormat[5];
  uint32_t recordSize;
  uint32_t recordsPerChunk;
  uint32_t chunkCount;
  uint64_t retainedCount;
  uint64_t totalStored;
  uint64_t overwrittenCount;
  uint64_t firstLogicalIndex;
  uint64_t lastLogicalIndex;
  uint32_t firstStm32Sequence;
  uint32_t lastStm32Sequence;
  uint64_t sourceMetadataGeneration;
  bool sessionTimeValid;
  bool sessionEndTimeValid;
  SyncManifestTimeSource timeSource;
  uint32_t sessionBootId;
  uint64_t sessionStartEpochMs;
  uint64_t sessionStartCaptureTimestampUs;
  uint64_t sessionEndEpochMs;
};

enum class SyncCloudManifestState : uint8_t {
  Absent = 0,
  Uploading,
  Complete,
  Unknown
};

struct SyncParsedCloudManifest {
  SyncCloudManifestState state;
  SyncManifestImmutable immutable;
  uint32_t manifestCrc32c;
  uint32_t nextChunk;
  uint64_t uploadedRecords;
  bool uploadStartedAtValid;
  bool uploadCompletedAtValid;
  uint64_t uploadStartedAtMs;
  uint64_t uploadCompletedAtMs;
};

enum class SyncManifestDecision : uint8_t {
  Create = 0,
  Resume,
  AlreadyComplete,
  CloudConflict,
  UnsupportedCloudManifest
};

enum class SyncCompletion412Decision : uint8_t {
  AcceptMatchingComplete = 0,
  CloudConflict
};

enum class SyncCancellationDecision : uint8_t {
  Continue = 0,
  DeferUntilTransactionCompletes,
  CancelNow
};

enum class SyncRestRequestSite : uint8_t {
  InitialConditionalManifestPut = 0,
  ChunkPut,
  CompletionConditionalManifestPut
};

class SessionSyncProtocol {
 public:
  static SessionSyncProtocolError formatUint64Decimal(
      uint64_t value, char* destination, uint32_t destinationLength,
      uint32_t& writtenLength);
  static SessionSyncProtocolError parseUint64Decimal(
      const char* source, uint32_t sourceLength, uint64_t& value);
  static SessionSyncProtocolError formatSessionKey(
      uint64_t sessionId, char* destination, uint32_t destinationLength);
  static SessionSyncProtocolError formatChunkKey(
      uint32_t chunkIndex, char* destination, uint32_t destinationLength);
  static SessionSyncProtocolError formatCrc32cHex(
      uint32_t crc, char* destination, uint32_t destinationLength);
  static SessionSyncProtocolError parseCrc32cHex(
      const char* source, uint32_t sourceLength, uint32_t& crc);

  static SessionSyncProtocolError base64EncodedLength(
      uint32_t inputLength, uint32_t& encodedLength);
  static SessionSyncProtocolError base64Encode(
      const uint8_t* source, uint32_t sourceLength, char* destination,
      uint32_t destinationLength, uint32_t& writtenLength);
  static uint32_t crc32c(const uint8_t* data, uint32_t length);

  static SessionSyncProtocolError calculateChunkCount(
      uint64_t retainedCount, uint32_t& chunkCount);
  static SessionSyncProtocolError calculateChunkBounds(
      uint64_t retainedCount, uint32_t chunkIndex, SyncChunkBounds& bounds);
  static bool progressIsValid(uint64_t retainedCount, uint32_t nextChunk,
                              uint64_t uploadedRecords);

  // Firebase RTDB rejects conditional ETag requests when they are combined
  // with print=silent, shallow, or query/filter parameters. Authentication is
  // not a query/filter parameter for this policy check.
  static bool restRequestOptionsAreCompatible(
      bool printSilent,
      bool hasIfMatch,
      bool hasIfNoneMatch,
      bool shallow,
      bool hasQueryOrFilterParameters);
  static bool printSilentForRequestSite(SyncRestRequestSite site);

  static bool isRetryableHttpStatus(int32_t httpStatus);
  static uint32_t retryDelayMs(uint32_t retryIndex);
  static SyncCancellationDecision evaluateCancellation(
      bool cancellationRequested, bool transactionInFlight);

  static SessionSyncProtocolError encodeCanonicalManifest(
      const SyncManifestImmutable& manifest, uint8_t* destination,
      uint32_t destinationLength);
  static SessionSyncProtocolError calculateManifestCrc32c(
      const SyncManifestImmutable& manifest, uint32_t& crc);
  static bool immutableManifestMatches(
      const SyncManifestImmutable& expected,
      const SyncManifestImmutable& candidate);
  static SyncManifestDecision evaluateCloudManifest(
      const SyncManifestImmutable& local,
      const SyncParsedCloudManifest& cloud);
  static SyncCompletion412Decision evaluateCompletionAfter412(
      const SyncManifestImmutable& local,
      const SyncParsedCloudManifest& cloud);
};
