import { encodeBase64 } from './base64'
import { crc32c, formatCrc32c } from './crc32c'
import {
  PQR1_COMMIT_MARKER,
  PQR1_COMMIT_OFFSET,
  PQR1_PADDING_OFFSET,
  PQR1_PAYLOAD_CRC_OFFSET,
  PQR1_PAYLOAD_OFFSET,
  PQR1_WHOLE_RECORD_CRC_OFFSET,
} from './pqr1'
import {
  HISTORY_DEVICE_ID,
  HISTORY_HARMONICS,
  HISTORY_PACKET_BYTES,
  HISTORY_RECORD_BYTES,
  HISTORY_RECORD_FORMAT,
  HISTORY_RECORDS_PER_CHUNK,
  HISTORY_WAVEFORM_SAMPLES,
  type HistoryIndexEntry,
  type HistoryListResult,
  type HistoryRepository,
  type SessionManifest,
} from './types'
import { calculateManifestCrc32c, formatChunkKey } from './validation'
import { invalidateSession } from './sessionInvalidation'

const MOCK_SESSION_ID = 42n
const MOCK_SESSION_KEY = 's_42'
const MOCK_FIRST_LOGICAL_INDEX = 100n
const MOCK_RECORD_COUNT = 3
const MOCK_UPLOAD_STARTED_AT = Date.UTC(2026, 6, 18, 4, 30, 0)
const MOCK_UPLOAD_COMPLETED_AT = Date.UTC(2026, 6, 18, 4, 30, 8)

export interface Pqr1FixtureOptions {
  sessionId: bigint
  logicalIndex: bigint
  captureTimestampUs: bigint
  stm32Sequence: number
  bootId: number
  seed: number
}

export function createDeterministicWaveformPacket(
  stm32Sequence: number,
  seed: number,
): Uint8Array {
  const packet = new Uint8Array(HISTORY_PACKET_BYTES)
  const view = new DataView(packet.buffer)
  view.setUint32(0, 0xaa55aa55, true)
  view.setUint32(4, stm32Sequence, true)

  for (let index = 0; index < HISTORY_WAVEFORM_SAMPLES; index += 1) {
    const voltage = ((index * 31 + seed * 17) % 2_001) - 1_000
    const current = ((index * 13 + seed * 29) % 801) - 400
    view.setInt16(8 + index * 2, voltage, true)
    view.setInt16(2_056 + index * 2, current, true)
  }

  const scalars = [
    229.5 + seed,
    4.25 + seed / 10,
    49.98,
    0.957,
    934.75 + seed,
    976.8 + seed,
    282.4 + seed,
    1.414,
    1.732,
    1.02,
    2.45,
    3.15,
  ]
  scalars.forEach((value, index) => {
    view.setFloat32(4_104 + index * 4, value, true)
  })
  for (let index = 0; index < HISTORY_HARMONICS; index += 1) {
    view.setFloat32(4_152 + index * 4, (index + 1) * 0.1 + seed * 0.01, true)
    view.setFloat32(4_252 + index * 4, (index + 1) * 0.15 + seed * 0.01, true)
  }

  let checksum = 0
  for (let offset = 0; offset < 4_352; offset += 1) {
    checksum = (checksum + packet[offset]) & 0xffff
  }
  view.setUint16(4_352, checksum, true)
  return packet
}

export function createPqr1Fixture(options: Pqr1FixtureOptions): Uint8Array {
  const packet = createDeterministicWaveformPacket(
    options.stm32Sequence,
    options.seed,
  )
  const record = new Uint8Array(HISTORY_RECORD_BYTES)
  const view = new DataView(record.buffer)
  record.set([0x50, 0x51, 0x52, 0x31], 0)
  view.setUint16(4, 1, true)
  view.setUint16(6, 64, true)
  view.setUint32(8, HISTORY_RECORD_BYTES, true)
  view.setUint32(12, HISTORY_PACKET_BYTES, true)
  view.setBigUint64(16, options.sessionId, true)
  view.setBigUint64(24, options.logicalIndex, true)
  view.setBigUint64(32, options.captureTimestampUs, true)
  view.setUint32(40, options.stm32Sequence, true)
  view.setUint16(44, 1, true)
  view.setUint16(46, 0, true)
  view.setUint32(48, options.bootId, true)
  view.setUint32(60, crc32c(record.subarray(0, 60)), true)
  record.set(packet, PQR1_PAYLOAD_OFFSET)
  record[PQR1_PADDING_OFFSET] = 0
  record[PQR1_PADDING_OFFSET + 1] = 0
  view.setUint32(
    PQR1_PAYLOAD_CRC_OFFSET,
    crc32c(record.subarray(PQR1_PAYLOAD_OFFSET, PQR1_PADDING_OFFSET)),
    true,
  )
  view.setUint32(
    PQR1_WHOLE_RECORD_CRC_OFFSET,
    crc32c(record.subarray(0, PQR1_WHOLE_RECORD_CRC_OFFSET)),
    true,
  )
  view.setUint32(PQR1_COMMIT_OFFSET, PQR1_COMMIT_MARKER, true)
  return record
}

function rawManifest(manifest: SessionManifest): Record<string, unknown> {
  const result: Record<string, unknown> = {
    schemaVersion: manifest.schemaVersion,
    state: manifest.state,
    deviceId: manifest.deviceId,
    sessionId: manifest.sessionId.toString(),
    persistentState: manifest.persistentState,
    recordFormat: manifest.recordFormat,
    recordSize: manifest.recordSize,
    recordsPerChunk: manifest.recordsPerChunk,
    chunkCount: manifest.chunkCount,
    nextChunk: manifest.nextChunk,
    uploadedRecords: manifest.uploadedRecords,
    retainedCount: manifest.retainedCount,
    totalStored: manifest.totalStored.toString(),
    overwrittenCount: manifest.overwrittenCount.toString(),
    firstLogicalIndex: manifest.firstLogicalIndex.toString(),
    lastLogicalIndex: manifest.lastLogicalIndex.toString(),
    firstStm32Sequence: manifest.firstStm32Sequence,
    lastStm32Sequence: manifest.lastStm32Sequence,
    truncated: manifest.truncated,
    recoveredIncomplete: manifest.recoveredIncomplete,
    countersPartial: manifest.countersPartial,
    sourceMetadataGeneration: manifest.sourceMetadataGeneration.toString(),
    uploadStartedAt: manifest.uploadStartedAt,
    uploadCompletedAt: manifest.uploadCompletedAt,
    manifestCrc32c: manifest.manifestCrc32c,
  }
  if (manifest.schemaVersion === 2) {
    result.sessionTimeValid = manifest.sessionTimeValid
    result.sessionStartEpochMs = (manifest.sessionStartEpochMs ?? 0n).toString()
    result.sessionStartCaptureTimestampUs =
      manifest.sessionStartCaptureTimestampUs?.toString()
    result.sessionEndTimeValid = manifest.sessionEndTimeValid
    result.sessionBootId = manifest.sessionBootId
    result.timeSource = manifest.timeSource
    if (manifest.sessionEndEpochMs !== null) {
      result.sessionEndEpochMs = manifest.sessionEndEpochMs.toString()
    }
  }
  return result
}

export interface DeterministicMockHistory {
  list: HistoryListResult
  manifest: Record<string, unknown>
  chunks: ReadonlyMap<string, unknown>
}

export function createDeterministicMockHistory(): DeterministicMockHistory {
  const lastLogicalIndex =
    MOCK_FIRST_LOGICAL_INDEX + BigInt(MOCK_RECORD_COUNT - 1)
  const manifest: SessionManifest = {
    sessionKey: MOCK_SESSION_KEY,
    schemaVersion: 1,
    state: 'complete',
    deviceId: HISTORY_DEVICE_ID,
    sessionId: MOCK_SESSION_ID,
    persistentState: 'Finalized',
    retainedCount: MOCK_RECORD_COUNT,
    totalStored: BigInt(MOCK_RECORD_COUNT),
    overwrittenCount: 0n,
    firstLogicalIndex: MOCK_FIRST_LOGICAL_INDEX,
    lastLogicalIndex,
    truncated: false,
    recoveredIncomplete: false,
    countersPartial: false,
    chunkCount: 1,
    uploadStartedAt: MOCK_UPLOAD_STARTED_AT,
    uploadCompletedAt: MOCK_UPLOAD_COMPLETED_AT,
    recordFormat: HISTORY_RECORD_FORMAT,
    recordSize: HISTORY_RECORD_BYTES,
    recordsPerChunk: HISTORY_RECORDS_PER_CHUNK,
    nextChunk: 1,
    uploadedRecords: MOCK_RECORD_COUNT,
    firstStm32Sequence: 900,
    lastStm32Sequence: 902,
    sourceMetadataGeneration: 7n,
    manifestCrc32c: '00000000',
    sessionTimeValid: false,
    sessionStartEpochMs: null,
    sessionStartCaptureTimestampUs: null,
    sessionEndTimeValid: false,
    sessionEndEpochMs: null,
    sessionBootId: null,
    timeSource: null,
  }
  manifest.manifestCrc32c = calculateManifestCrc32c(manifest)

  const indexEntry: HistoryIndexEntry = {
    sessionKey: manifest.sessionKey,
    schemaVersion: manifest.schemaVersion,
    state: manifest.state,
    deviceId: manifest.deviceId,
    sessionId: manifest.sessionId,
    persistentState: manifest.persistentState,
    retainedCount: manifest.retainedCount,
    totalStored: manifest.totalStored,
    overwrittenCount: manifest.overwrittenCount,
    firstLogicalIndex: manifest.firstLogicalIndex,
    lastLogicalIndex: manifest.lastLogicalIndex,
    truncated: manifest.truncated,
    recoveredIncomplete: manifest.recoveredIncomplete,
    countersPartial: manifest.countersPartial,
    chunkCount: manifest.chunkCount,
    uploadStartedAt: manifest.uploadStartedAt,
    uploadCompletedAt: manifest.uploadCompletedAt,
    sessionTimeValid: manifest.sessionTimeValid,
    sessionStartEpochMs: manifest.sessionStartEpochMs,
  }

  const rawRecords = new Uint8Array(MOCK_RECORD_COUNT * HISTORY_RECORD_BYTES)
  for (let index = 0; index < MOCK_RECORD_COUNT; index += 1) {
    rawRecords.set(
      createPqr1Fixture({
        sessionId: MOCK_SESSION_ID,
        logicalIndex: MOCK_FIRST_LOGICAL_INDEX + BigInt(index),
        captureTimestampUs: 5_000_000n + BigInt(index * 100_000),
        stm32Sequence: 900 + index,
        bootId: 0x1234abcd,
        seed: index + 1,
      }),
      index * HISTORY_RECORD_BYTES,
    )
  }
  const chunkKey = formatChunkKey(0)
  const chunk = {
    meta: {
      schemaVersion: 1,
      chunkIndex: 0,
      firstLogicalIndex: MOCK_FIRST_LOGICAL_INDEX.toString(),
      lastLogicalIndex: lastLogicalIndex.toString(),
      recordCount: MOCK_RECORD_COUNT,
      recordSize: HISTORY_RECORD_BYTES,
      rawBytes: rawRecords.byteLength,
      crc32c: formatCrc32c(crc32c(rawRecords)),
      encoding: 'base64',
      recordFormat: HISTORY_RECORD_FORMAT,
    },
    payload: encodeBase64(rawRecords),
  }

  return {
    list: { sessions: [indexEntry], malformedEntries: [] },
    manifest: rawManifest(manifest),
    chunks: new Map([[chunkKey, chunk]]),
  }
}

export class MockHistoryRepository implements HistoryRepository {
  readonly mode = 'mock' as const
  private readonly fixture = createDeterministicMockHistory()
  private deleted = false

  async listCompletedSessions(): Promise<HistoryListResult> {
    return Promise.resolve(this.deleted
      ? { sessions: [], malformedEntries: [] }
      : this.fixture.list)
  }

  async getManifest(sessionKey: string): Promise<unknown> {
    if (this.deleted || sessionKey !== MOCK_SESSION_KEY) return Promise.resolve(null)
    return Promise.resolve(this.fixture.manifest)
  }

  async getChunk(sessionKey: string, chunkKey: string): Promise<unknown> {
    if (this.deleted || sessionKey !== MOCK_SESSION_KEY) return Promise.resolve(null)
    return Promise.resolve(this.fixture.chunks.get(chunkKey) ?? null)
  }

  async deleteSession(deviceId: string, sessionKey: string): Promise<void> {
    if (deviceId !== HISTORY_DEVICE_ID || sessionKey !== MOCK_SESSION_KEY) {
      throw new Error('The mock session identifier is invalid.')
    }
    this.deleted = true
    invalidateSession(sessionKey)
  }
}
