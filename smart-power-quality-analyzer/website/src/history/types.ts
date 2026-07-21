export const HISTORY_DEVICE_ID = 'PQ-3PH-001'
export const HISTORY_LEGACY_MANIFEST_SCHEMA_VERSION = 1
export const HISTORY_SCHEMA_VERSION = 2
export const HISTORY_CHUNK_SCHEMA_VERSION = 1
export const HISTORY_RECORD_FORMAT = 'PQR1'
export const HISTORY_RECORD_BYTES = 4_432
export const HISTORY_RECORDS_PER_CHUNK = 8
export const HISTORY_PACKET_BYTES = 4_354
export const HISTORY_WAVEFORM_SAMPLES = 1_024
export const HISTORY_HARMONICS = 25

export type PersistentSessionState = 'Finalized' | 'RecoveredIncomplete'

export interface HistoryIndexEntry {
  sessionKey: string
  schemaVersion: 1 | 2
  state: 'complete'
  deviceId: typeof HISTORY_DEVICE_ID
  sessionId: bigint
  persistentState: PersistentSessionState
  retainedCount: number
  totalStored: bigint
  overwrittenCount: bigint
  firstLogicalIndex: bigint
  lastLogicalIndex: bigint
  truncated: boolean
  recoveredIncomplete: boolean
  countersPartial: boolean
  chunkCount: number
  uploadStartedAt: number
  uploadCompletedAt: number
  sessionTimeValid: boolean
  sessionStartEpochMs: bigint | null
}

export interface MalformedHistoryEntry {
  sessionKey: string
  reason: string
}

export interface HistoryListResult {
  sessions: HistoryIndexEntry[]
  malformedEntries: MalformedHistoryEntry[]
}

export interface SessionManifest extends HistoryIndexEntry {
  recordFormat: typeof HISTORY_RECORD_FORMAT
  recordSize: typeof HISTORY_RECORD_BYTES
  recordsPerChunk: typeof HISTORY_RECORDS_PER_CHUNK
  nextChunk: number
  uploadedRecords: number
  firstStm32Sequence: number
  lastStm32Sequence: number
  sourceMetadataGeneration: bigint
  manifestCrc32c: string
  sessionStartCaptureTimestampUs: bigint | null
  sessionEndTimeValid: boolean
  sessionEndEpochMs: bigint | null
  sessionBootId: number | null
  timeSource: 'ntp' | null
}

export interface SessionChunkMeta {
  schemaVersion: 1
  chunkIndex: number
  firstLogicalIndex: bigint
  lastLogicalIndex: bigint
  recordCount: number
  recordSize: typeof HISTORY_RECORD_BYTES
  rawBytes: number
  crc32c: string
  encoding: 'base64'
  recordFormat: typeof HISTORY_RECORD_FORMAT
}

export interface SessionChunk {
  meta: SessionChunkMeta
  payload: string
}

export interface Pqr1RecordMetadata {
  recordFormatVersion: number
  sessionId: bigint
  logicalRecordIndex: bigint
  captureTimestampUs: bigint
  stm32Sequence: number
  packetFormatVersion: number
  flags: number
  bootId: number
}

export interface WaveformPacketScalars {
  vRms: number
  iRms: number
  frequency: number
  powerFactor: number
  activePower: number
  apparentPower: number
  reactivePower: number
  crestFactorV: number
  crestFactorI: number
  swellFactor: number
  thdV: number
  thdI: number
}

export interface ParsedWaveformPacket {
  startMarker: number
  sequence: number
  voltageSamples: Int16Array
  currentSamples: Int16Array
  metrics: WaveformPacketScalars
  voltageHarmonics: Float32Array
  currentHarmonics: Float32Array
  checksum: number
}

export interface ValidatedPqr1Record {
  metadata: Pqr1RecordMetadata
  packet: ParsedWaveformPacket
}

export type ExportKind = 'waveforms'

export type ExportPhase =
  | 'preparing'
  | 'fetching-chunk'
  | 'validating-chunk'
  | 'processing-records'
  | 'building-file'
  | 'complete'
  | 'cancelled'

export interface ExportProgress {
  phase: ExportPhase
  currentChunk: number
  totalChunks: number
  recordsProcessed: number
  retainedRecords: number
  percentage: number
  cancellationRequested: boolean
}

export interface ExportIntegrityReport {
  kind: ExportKind
  chunksValidated: number
  recordsValidated: number
  firstLogicalIndex: bigint
  lastLogicalIndex: bigint
  chunkCrcValidationPassed: boolean
  recordCrcValidationPassed: boolean
  csvRowCount: number
  fileSizeBytes: number
  durationMs: number
}

export interface SessionExportResult {
  blob: Blob
  fileName: string
  report: ExportIntegrityReport
}

export interface HistoryRepository {
  readonly mode: 'firebase' | 'mock'
  listCompletedSessions(): Promise<HistoryListResult>
  getManifest(sessionKey: string): Promise<unknown>
  getChunk(sessionKey: string, chunkKey: string): Promise<unknown>
  deleteSession?(deviceId: string, sessionKey: string): Promise<void>
}
