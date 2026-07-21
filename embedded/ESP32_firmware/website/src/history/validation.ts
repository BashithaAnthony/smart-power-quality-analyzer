import { crc32c, formatCrc32c } from './crc32c'
import {
  HISTORY_DEVICE_ID,
  HISTORY_CHUNK_SCHEMA_VERSION,
  HISTORY_LEGACY_MANIFEST_SCHEMA_VERSION,
  HISTORY_RECORD_BYTES,
  HISTORY_RECORD_FORMAT,
  HISTORY_RECORDS_PER_CHUNK,
  HISTORY_SCHEMA_VERSION,
  type HistoryIndexEntry,
  type HistoryListResult,
  type PersistentSessionState,
  type SessionChunk,
  type SessionManifest,
} from './types'

const SESSION_KEY_PATTERN = /^s_[0-9]+$/
const DECIMAL_PATTERN = /^(0|[1-9][0-9]*)$/
const CRC32C_PATTERN = /^[0-9A-F]{8}$/
const UINT64_MAX = 18_446_744_073_709_551_615n
const UINT32_MAX = 0xffff_ffff
const MIN_NTP_EPOCH_MS = 1_704_067_200_000n
const MAX_JAVASCRIPT_DATE_MS = 8_640_000_000_000_000n
const LEGACY_MANIFEST_CANONICAL_BYTES = 112
const MANIFEST_CANONICAL_BYTES = 144

type UnknownRecord = Record<string, unknown>

export class HistoryValidationError extends Error {
  constructor(message: string) {
    super(message)
    this.name = 'HistoryValidationError'
  }
}

function isRecord(value: unknown): value is UnknownRecord {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function objectValue(value: unknown, fieldName: string): UnknownRecord {
  if (!isRecord(value)) {
    throw new HistoryValidationError(`${fieldName} must be an object`)
  }
  return value
}

function stringValue(
  source: UnknownRecord,
  fieldName: string,
): string {
  const value = source[fieldName]
  if (typeof value !== 'string') {
    throw new HistoryValidationError(`${fieldName} must be a string`)
  }
  return value
}

function literalString<T extends string>(
  source: UnknownRecord,
  fieldName: string,
  expected: T,
): T {
  const value = stringValue(source, fieldName)
  if (value !== expected) {
    throw new HistoryValidationError(`${fieldName} must be ${expected}`)
  }
  return expected
}

function booleanValue(source: UnknownRecord, fieldName: string): boolean {
  const value = source[fieldName]
  if (typeof value !== 'boolean') {
    throw new HistoryValidationError(`${fieldName} must be a boolean`)
  }
  return value
}

function integerValue(
  source: UnknownRecord,
  fieldName: string,
  minimum: number,
  maximum: number,
): number {
  const value = source[fieldName]
  if (
    typeof value !== 'number' ||
    !Number.isSafeInteger(value) ||
    value < minimum ||
    value > maximum
  ) {
    throw new HistoryValidationError(
      `${fieldName} must be an integer from ${minimum} to ${maximum}`,
    )
  }
  return value
}

function literalInteger<T extends number>(
  source: UnknownRecord,
  fieldName: string,
  expected: T,
): T {
  integerValue(source, fieldName, expected, expected)
  return expected
}

function timestampValue(source: UnknownRecord, fieldName: string): number {
  const value = integerValue(
    source,
    fieldName,
    1,
    Number.MAX_SAFE_INTEGER,
  )
  if (!Number.isFinite(new Date(value).getTime())) {
    throw new HistoryValidationError(`${fieldName} is not a valid timestamp`)
  }
  return value
}

function decimalValue(source: UnknownRecord, fieldName: string): bigint {
  const text = stringValue(source, fieldName)
  if (!DECIMAL_PATTERN.test(text)) {
    throw new HistoryValidationError(
      `${fieldName} must be a canonical unsigned decimal string`,
    )
  }
  const value = BigInt(text)
  if (value > UINT64_MAX) {
    throw new HistoryValidationError(`${fieldName} exceeds uint64`)
  }
  return value
}

function supportedManifestSchema(source: UnknownRecord): 1 | 2 {
  const value = integerValue(
    source,
    'schemaVersion',
    HISTORY_LEGACY_MANIFEST_SCHEMA_VERSION,
    HISTORY_SCHEMA_VERSION,
  )
  if (
    value !== HISTORY_LEGACY_MANIFEST_SCHEMA_VERSION &&
    value !== HISTORY_SCHEMA_VERSION
  ) {
    throw new HistoryValidationError('schemaVersion is not supported')
  }
  return value
}

function plausibleEpochValue(value: bigint, fieldName: string): bigint {
  if (value < MIN_NTP_EPOCH_MS || value > MAX_JAVASCRIPT_DATE_MS) {
    throw new HistoryValidationError(`${fieldName} is not a plausible NTP epoch`)
  }
  return value
}

function persistentStateValue(
  source: UnknownRecord,
): PersistentSessionState {
  const value = stringValue(source, 'persistentState')
  if (value !== 'Finalized' && value !== 'RecoveredIncomplete') {
    throw new HistoryValidationError(
      'persistentState must be Finalized or RecoveredIncomplete',
    )
  }
  return value
}

function validateSharedSessionFields(
  sessionKey: string,
  source: UnknownRecord,
): HistoryIndexEntry {
  if (!isValidSessionKey(sessionKey)) {
    throw new HistoryValidationError('Session key has an invalid format')
  }

  const sessionId = decimalValue(source, 'sessionId')
  if (sessionId === 0n || `s_${sessionId.toString()}` !== sessionKey) {
    throw new HistoryValidationError('sessionId does not match the session key')
  }

  const retainedCount = integerValue(
    source,
    'retainedCount',
    1,
    UINT32_MAX,
  )
  const totalStored = decimalValue(source, 'totalStored')
  const overwrittenCount = decimalValue(source, 'overwrittenCount')
  const firstLogicalIndex = decimalValue(source, 'firstLogicalIndex')
  const lastLogicalIndex = decimalValue(source, 'lastLogicalIndex')
  const chunkCount = integerValue(source, 'chunkCount', 1, UINT32_MAX)
  const persistentState = persistentStateValue(source)
  const schemaVersion = supportedManifestSchema(source)
  const truncated = booleanValue(source, 'truncated')
  const recoveredIncomplete = booleanValue(source, 'recoveredIncomplete')

  if (totalStored < BigInt(retainedCount)) {
    throw new HistoryValidationError('totalStored is less than retainedCount')
  }
  if (overwrittenCount > totalStored) {
    throw new HistoryValidationError('overwrittenCount exceeds totalStored')
  }
  if (
    lastLogicalIndex < firstLogicalIndex ||
    lastLogicalIndex - firstLogicalIndex !== BigInt(retainedCount - 1)
  ) {
    throw new HistoryValidationError(
      'Logical-index range does not match retainedCount',
    )
  }
  if (chunkCount !== Math.ceil(retainedCount / HISTORY_RECORDS_PER_CHUNK)) {
    throw new HistoryValidationError('chunkCount does not match retainedCount')
  }
  if (
    (persistentState === 'RecoveredIncomplete') !== recoveredIncomplete
  ) {
    throw new HistoryValidationError(
      'persistentState and recoveredIncomplete disagree',
    )
  }

  let sessionTimeValid = false
  let sessionStartEpochMs: bigint | null = null
  if (schemaVersion === HISTORY_SCHEMA_VERSION) {
    sessionTimeValid = booleanValue(source, 'sessionTimeValid')
    if (sessionTimeValid) {
      sessionStartEpochMs = plausibleEpochValue(
        decimalValue(source, 'sessionStartEpochMs'),
        'sessionStartEpochMs',
      )
    } else if (source.sessionStartEpochMs !== undefined) {
      const invalidEpoch = decimalValue(source, 'sessionStartEpochMs')
      if (invalidEpoch !== 0n) {
        throw new HistoryValidationError(
          'sessionStartEpochMs must be zero when sessionTimeValid is false',
        )
      }
    }
  }

  return {
    sessionKey,
    schemaVersion,
    state: literalString(source, 'state', 'complete'),
    deviceId: literalString(source, 'deviceId', HISTORY_DEVICE_ID),
    sessionId,
    persistentState,
    retainedCount,
    totalStored,
    overwrittenCount,
    firstLogicalIndex,
    lastLogicalIndex,
    truncated,
    recoveredIncomplete,
    countersPartial: booleanValue(source, 'countersPartial'),
    chunkCount,
    uploadStartedAt: timestampValue(source, 'uploadStartedAt'),
    uploadCompletedAt: timestampValue(source, 'uploadCompletedAt'),
    sessionTimeValid,
    sessionStartEpochMs,
  }
}

export function isValidSessionKey(sessionKey: string): boolean {
  return SESSION_KEY_PATTERN.test(sessionKey)
}

export function formatChunkKey(chunkIndex: number): string {
  if (!Number.isSafeInteger(chunkIndex) || chunkIndex < 0 || chunkIndex > 999_999) {
    throw new HistoryValidationError('Chunk index is outside the six-digit range')
  }
  return chunkIndex.toString().padStart(6, '0')
}

export function isValidChunkKey(chunkKey: string): boolean {
  return /^[0-9]{6}$/.test(chunkKey)
}

export function parseHistoryIndexEntry(
  sessionKey: string,
  value: unknown,
): HistoryIndexEntry {
  return validateSharedSessionFields(
    sessionKey,
    objectValue(value, `sessionIndex/${sessionKey}`),
  )
}

export function parseHistoryIndex(value: unknown): HistoryListResult {
  if (value === null) return { sessions: [], malformedEntries: [] }
  const root = objectValue(value, 'sessionIndex')
  const sessions: HistoryIndexEntry[] = []
  const malformedEntries: HistoryListResult['malformedEntries'] = []

  for (const [sessionKey, entry] of Object.entries(root)) {
    try {
      sessions.push(parseHistoryIndexEntry(sessionKey, entry))
    } catch (error) {
      malformedEntries.push({
        sessionKey,
        reason: error instanceof Error ? error.message : 'Unknown validation error',
      })
    }
  }

  sessions.sort((left, right) => right.uploadCompletedAt - left.uploadCompletedAt)
  malformedEntries.sort((left, right) => left.sessionKey.localeCompare(right.sessionKey))
  return { sessions, malformedEntries }
}

function writeUint64(view: DataView, offset: number, value: bigint): void {
  view.setBigUint64(offset, value, true)
}

export function calculateManifestCrc32c(manifest: SessionManifest): string {
  const canonicalBytes = manifest.schemaVersion ===
    HISTORY_LEGACY_MANIFEST_SCHEMA_VERSION
    ? LEGACY_MANIFEST_CANONICAL_BYTES
    : MANIFEST_CANONICAL_BYTES
  const bytes = new Uint8Array(canonicalBytes)
  const view = new DataView(bytes.buffer)
  bytes.set([0x50, 0x51, 0x4d, 0x49], 0) // PQMI
  view.setUint16(4, manifest.schemaVersion, true)
  view.setUint16(6, canonicalBytes, true)
  view.setUint32(8, manifest.schemaVersion, true)
  for (let index = 0; index < manifest.deviceId.length; index += 1) {
    bytes[12 + index] = manifest.deviceId.charCodeAt(index)
  }
  writeUint64(view, 28, manifest.sessionId)
  view.setUint8(
    36,
    manifest.persistentState === 'Finalized' ? 1 : 2,
  )
  let flags = 0
  if (manifest.truncated) flags |= 1
  if (manifest.recoveredIncomplete) flags |= 2
  if (manifest.countersPartial) flags |= 4
  view.setUint8(37, flags)
  bytes.set([0x50, 0x51, 0x52, 0x31], 40) // PQR1
  view.setUint32(44, manifest.recordSize, true)
  view.setUint32(48, manifest.recordsPerChunk, true)
  view.setUint32(52, manifest.chunkCount, true)
  writeUint64(view, 56, BigInt(manifest.retainedCount))
  writeUint64(view, 64, manifest.totalStored)
  writeUint64(view, 72, manifest.overwrittenCount)
  writeUint64(view, 80, manifest.firstLogicalIndex)
  writeUint64(view, 88, manifest.lastLogicalIndex)
  view.setUint32(96, manifest.firstStm32Sequence, true)
  view.setUint32(100, manifest.lastStm32Sequence, true)
  writeUint64(view, 104, manifest.sourceMetadataGeneration)
  if (manifest.schemaVersion === HISTORY_SCHEMA_VERSION) {
    view.setUint8(112, manifest.sessionTimeValid ? 1 : 0)
    view.setUint8(113, manifest.sessionEndTimeValid ? 1 : 0)
    view.setUint8(114, manifest.timeSource === 'ntp' ? 1 : 0)
    view.setUint32(116, manifest.sessionBootId ?? 0, true)
    writeUint64(view, 120, manifest.sessionStartEpochMs ?? 0n)
    writeUint64(
      view,
      128,
      manifest.sessionStartCaptureTimestampUs ?? 0n,
    )
    writeUint64(view, 136, manifest.sessionEndEpochMs ?? 0n)
  }
  return formatCrc32c(crc32c(bytes))
}

export function parseSessionManifest(
  sessionKey: string,
  value: unknown,
): SessionManifest {
  const source = objectValue(value, `sessionData/${sessionKey}/manifest`)
  const shared = validateSharedSessionFields(sessionKey, source)
  const manifest: SessionManifest = {
    ...shared,
    recordFormat: literalString(
      source,
      'recordFormat',
      HISTORY_RECORD_FORMAT,
    ),
    recordSize: literalInteger(source, 'recordSize', HISTORY_RECORD_BYTES),
    recordsPerChunk: literalInteger(
      source,
      'recordsPerChunk',
      HISTORY_RECORDS_PER_CHUNK,
    ),
    nextChunk: integerValue(source, 'nextChunk', 0, UINT32_MAX),
    uploadedRecords: integerValue(
      source,
      'uploadedRecords',
      0,
      UINT32_MAX,
    ),
    firstStm32Sequence: integerValue(
      source,
      'firstStm32Sequence',
      0,
      UINT32_MAX,
    ),
    lastStm32Sequence: integerValue(
      source,
      'lastStm32Sequence',
      0,
      UINT32_MAX,
    ),
    sourceMetadataGeneration: decimalValue(
      source,
      'sourceMetadataGeneration',
    ),
    manifestCrc32c: stringValue(source, 'manifestCrc32c'),
    sessionStartCaptureTimestampUs: null,
    sessionEndTimeValid: false,
    sessionEndEpochMs: null,
    sessionBootId: null,
    timeSource: null,
  }

  if (manifest.schemaVersion === HISTORY_SCHEMA_VERSION) {
    manifest.sessionStartCaptureTimestampUs = decimalValue(
      source,
      'sessionStartCaptureTimestampUs',
    )
    manifest.sessionEndTimeValid = booleanValue(source, 'sessionEndTimeValid')
    manifest.sessionBootId = integerValue(
      source,
      'sessionBootId',
      1,
      UINT32_MAX,
    )
    manifest.timeSource = literalString(source, 'timeSource', 'ntp')
    if (manifest.sessionEndTimeValid) {
      manifest.sessionEndEpochMs = plausibleEpochValue(
        decimalValue(source, 'sessionEndEpochMs'),
        'sessionEndEpochMs',
      )
    } else if (source.sessionEndEpochMs !== undefined) {
      throw new HistoryValidationError(
        'sessionEndEpochMs must be omitted when sessionEndTimeValid is false',
      )
    }
    if (
      manifest.sessionTimeValid &&
      manifest.sessionStartEpochMs !== null &&
      manifest.sessionEndEpochMs !== null &&
      manifest.sessionEndEpochMs < manifest.sessionStartEpochMs
    ) {
      throw new HistoryValidationError(
        'sessionEndEpochMs cannot be before sessionStartEpochMs',
      )
    }
  }

  if (
    manifest.nextChunk !== manifest.chunkCount ||
    manifest.uploadedRecords !== manifest.retainedCount
  ) {
    throw new HistoryValidationError('Manifest upload progress is incomplete')
  }
  if (manifest.sourceMetadataGeneration === 0n) {
    throw new HistoryValidationError('sourceMetadataGeneration must be positive')
  }
  // A descending STM32 sequence range is intentionally accepted because the
  // 32-bit packet counter may wrap during a long-running analyzer session.
  if (!CRC32C_PATTERN.test(manifest.manifestCrc32c)) {
    throw new HistoryValidationError('manifestCrc32c must be uppercase hexadecimal')
  }
  if (calculateManifestCrc32c(manifest) !== manifest.manifestCrc32c) {
    throw new HistoryValidationError('Manifest canonical CRC32C is invalid')
  }
  return manifest
}

export function parseSessionChunk(
  value: unknown,
  manifest: SessionManifest,
  expectedChunkIndex: number,
): SessionChunk {
  const root = objectValue(value, `chunks/${formatChunkKey(expectedChunkIndex)}`)
  const meta = objectValue(root.meta, 'chunk meta')
  const finalChunk = expectedChunkIndex === manifest.chunkCount - 1
  const remainder = manifest.retainedCount % HISTORY_RECORDS_PER_CHUNK
  const expectedRecordCount = finalChunk
    ? remainder === 0 ? HISTORY_RECORDS_PER_CHUNK : remainder
    : HISTORY_RECORDS_PER_CHUNK
  const recordCount = integerValue(meta, 'recordCount', 1, HISTORY_RECORDS_PER_CHUNK)
  const chunkIndex = integerValue(meta, 'chunkIndex', 0, UINT32_MAX)
  const firstLogicalIndex = decimalValue(meta, 'firstLogicalIndex')
  const lastLogicalIndex = decimalValue(meta, 'lastLogicalIndex')
  const expectedFirst =
    manifest.firstLogicalIndex +
    BigInt(expectedChunkIndex * HISTORY_RECORDS_PER_CHUNK)
  const expectedLast = expectedFirst + BigInt(expectedRecordCount - 1)
  const rawBytes = integerValue(
    meta,
    'rawBytes',
    HISTORY_RECORD_BYTES,
    HISTORY_RECORD_BYTES * HISTORY_RECORDS_PER_CHUNK,
  )
  const chunkCrc = stringValue(meta, 'crc32c')

  if (chunkIndex !== expectedChunkIndex) {
    throw new HistoryValidationError('Chunk index does not match its key')
  }
  if (recordCount !== expectedRecordCount) {
    throw new HistoryValidationError(
      finalChunk
        ? 'Final chunk record count is inconsistent'
        : 'A non-final chunk must contain eight records',
    )
  }
  if (
    firstLogicalIndex !== expectedFirst ||
    lastLogicalIndex !== expectedLast ||
    lastLogicalIndex - firstLogicalIndex !== BigInt(recordCount - 1)
  ) {
    throw new HistoryValidationError('Chunk logical-index range is invalid')
  }
  if (rawBytes !== recordCount * HISTORY_RECORD_BYTES) {
    throw new HistoryValidationError('Chunk rawBytes does not match recordCount')
  }
  if (!CRC32C_PATTERN.test(chunkCrc)) {
    throw new HistoryValidationError('Chunk CRC32C must be uppercase hexadecimal')
  }
  if (typeof root.payload !== 'string' || root.payload.length === 0) {
    throw new HistoryValidationError('Chunk payload must be non-empty Base64')
  }

  return {
    meta: {
      schemaVersion: literalInteger(
        meta,
        'schemaVersion',
        HISTORY_CHUNK_SCHEMA_VERSION,
      ),
      chunkIndex,
      firstLogicalIndex,
      lastLogicalIndex,
      recordCount,
      recordSize: literalInteger(meta, 'recordSize', HISTORY_RECORD_BYTES),
      rawBytes,
      crc32c: chunkCrc,
      encoding: literalString(meta, 'encoding', 'base64'),
      recordFormat: literalString(
        meta,
        'recordFormat',
        HISTORY_RECORD_FORMAT,
      ),
    },
    payload: root.payload,
  }
}

export function formatLocalTimestamp(timestampMs: number): string {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
    timeStyle: 'medium',
  }).format(new Date(timestampMs))
}

export function formatIsoTimestamp(timestampMs: number): string {
  return new Date(timestampMs).toISOString()
}
