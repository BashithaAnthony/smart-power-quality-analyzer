import {
  fetchValidatedChunk,
  HistoryPipelineCancelledError,
  validateDecodedChunk,
  type ValidatedChunkData,
} from './chunkPipeline'
import { validatePqr1Record } from './pqr1'
import {
  HISTORY_RECORD_BYTES,
  HISTORY_RECORDS_PER_CHUNK,
  type HistoryRepository,
  type SessionChunkMeta,
  type SessionManifest,
  type ValidatedPqr1Record,
  type WaveformPacketScalars,
} from './types'
import { formatChunkKey } from './validation'

export type TrendMetricKey = keyof WaveformPacketScalars

export interface TrendMetricDefinition {
  key: TrendMetricKey
  label: string
  unit: string
  color: string
}

export const TREND_METRICS: ReadonlyArray<TrendMetricDefinition> = [
  { key: 'vRms', label: 'Voltage RMS', unit: 'V', color: '#53d2e6' },
  { key: 'iRms', label: 'Current RMS', unit: 'A', color: '#f1b85a' },
  { key: 'frequency', label: 'Frequency', unit: 'Hz', color: '#b8a2ff' },
  { key: 'powerFactor', label: 'Power Factor', unit: 'ratio', color: '#5cdd9b' },
  { key: 'activePower', label: 'Active Power', unit: 'W', color: '#5cdd9b' },
  { key: 'apparentPower', label: 'Apparent Power', unit: 'VA', color: '#8beaf7' },
  { key: 'reactivePower', label: 'Reactive Power', unit: 'var', color: '#b8a2ff' },
  { key: 'thdV', label: 'Voltage THD', unit: '%', color: '#53d2e6' },
  { key: 'thdI', label: 'Current THD', unit: '%', color: '#f1b85a' },
  { key: 'crestFactorV', label: 'Voltage Crest Factor', unit: 'factor', color: '#53d2e6' },
  { key: 'crestFactorI', label: 'Current Crest Factor', unit: 'factor', color: '#f1b85a' },
  { key: 'swellFactor', label: 'Swell Factor', unit: 'factor', color: '#b8a2ff' },
]

export const GRAPH_PAGE_SECTIONS = Object.freeze({
  trend: 'Metric trend',
  packetSelector: 'Selected packet',
  voltageHarmonics: 'Voltage Harmonics H1-H25',
  currentHarmonics: 'Current Harmonics H1-H25',
})

export interface ScalarTrendSummary extends WaveformPacketScalars {
  logicalIndex: bigint
  captureTimestampUs: bigint
  stm32Sequence: number
  bootId: number
}

export interface ResolvedScalarTrendSummary extends ScalarTrendSummary {
  timeAxisValue: number
  packetEpochMs: bigint | null
  elapsedSeconds: number
}

export interface TrendChartPoint {
  logicalIndex: string
  stm32Sequence: number
  metricValue: number
  timeAxisValue: number
  packetEpochMs: number | null
  elapsedSeconds: number
}

export interface HarmonicChartPoint {
  harmonic: string
  value: number
}

export interface SelectedPacketHarmonicsData {
  logicalIndex: bigint
  captureTimestampUs: bigint
  stm32Sequence: number
  voltageHarmonics: Float32Array
  currentHarmonics: Float32Array
}

export type GraphLoadPhase =
  | 'fetching-chunk'
  | 'validating-chunk'
  | 'extracting-summaries'
  | 'complete'
  | 'cancelled'

export interface GraphLoadProgress {
  phase: GraphLoadPhase
  currentChunk: number
  totalChunks: number
  recordsValidated: number
  retainedRecords: number
  percentage: number
  cancellationRequested: boolean
}

export interface GraphChunkCache {
  chunkIndex: number
  meta: SessionChunkMeta
  decodedBytes: Uint8Array
}

export interface PacketHarmonicsLoadResult {
  packet: SelectedPacketHarmonicsData
  cache: GraphChunkCache
}

export interface GraphTrendLoadOptions {
  repository: HistoryRepository
  manifest: SessionManifest
  signal: AbortSignal
  onProgress?: (progress: GraphLoadProgress) => void
  yieldControl?: () => Promise<void>
}

export class HistoricalTimestampError extends Error {
  constructor(message: string) {
    super(message)
    this.name = 'HistoricalTimestampError'
  }
}

const MAX_JAVASCRIPT_DATE_MS = 8_640_000_000_000_000n

function defaultYieldControl(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0))
}

function throwIfCancelled(signal: AbortSignal): void {
  if (signal.aborted) throw new HistoryPipelineCancelledError()
}

export function metricDefinition(key: TrendMetricKey): TrendMetricDefinition {
  const definition = TREND_METRICS.find((candidate) => candidate.key === key)
  if (definition === undefined) throw new Error(`Unknown trend metric: ${key}`)
  return definition
}

export function isTrendMetricKey(value: string): value is TrendMetricKey {
  return TREND_METRICS.some((candidate) => candidate.key === value)
}

export function extractScalarTrendSummary(
  record: ValidatedPqr1Record,
): ScalarTrendSummary {
  return {
    logicalIndex: record.metadata.logicalRecordIndex,
    captureTimestampUs: record.metadata.captureTimestampUs,
    stm32Sequence: record.metadata.stm32Sequence,
    bootId: record.metadata.bootId,
    ...record.packet.metrics,
  }
}

export function extractSelectedPacketHarmonics(
  record: ValidatedPqr1Record,
): SelectedPacketHarmonicsData {
  return {
    logicalIndex: record.metadata.logicalRecordIndex,
    captureTimestampUs: record.metadata.captureTimestampUs,
    stm32Sequence: record.metadata.stm32Sequence,
    voltageHarmonics: record.packet.voltageHarmonics.slice(),
    currentHarmonics: record.packet.currentHarmonics.slice(),
  }
}

export function createTrendChartData(
  summaries: ReadonlyArray<ResolvedScalarTrendSummary>,
  metric: TrendMetricKey,
): TrendChartPoint[] {
  return summaries.map((summary) => ({
    logicalIndex: summary.logicalIndex.toString(),
    stm32Sequence: summary.stm32Sequence,
    metricValue: summary[metric],
    timeAxisValue: summary.timeAxisValue,
    packetEpochMs: summary.packetEpochMs === null
      ? null
      : Number(summary.packetEpochMs),
    elapsedSeconds: summary.elapsedSeconds,
  }))
}

export function resolveHistoricalTimestamps(
  manifest: SessionManifest,
  summaries: ReadonlyArray<ScalarTrendSummary>,
): ResolvedScalarTrendSummary[] {
  if (summaries.length === 0) return []
  const anchored = manifest.sessionTimeValid
  const startEpochMs = manifest.sessionStartEpochMs
  const startCaptureUs = manifest.sessionStartCaptureTimestampUs
  const anchorBootId = manifest.sessionBootId
  if (
    anchored &&
    (startEpochMs === null || startCaptureUs === null || anchorBootId === null)
  ) {
    throw new HistoricalTimestampError(
      'The session time anchor is incomplete.',
    )
  }
  const anchorEpochMs = startEpochMs ?? 0n
  const anchorCaptureUs = startCaptureUs ?? 0n
  const requiredBootId = anchorBootId ?? 0

  const firstCaptureUs = summaries[0].captureTimestampUs
  let previousCaptureUs: bigint | null = null
  return summaries.map((summary) => {
    if (
      previousCaptureUs !== null &&
      summary.captureTimestampUs < previousCaptureUs
    ) {
      throw new HistoricalTimestampError(
        `Capture timestamps are not monotonic at logical index ${summary.logicalIndex.toString()}.`,
      )
    }
    previousCaptureUs = summary.captureTimestampUs
    if (!anchored) {
      if (summary.captureTimestampUs < firstCaptureUs) {
        throw new HistoricalTimestampError(
          `Capture timestamp precedes the first retained packet at logical index ${summary.logicalIndex.toString()}.`,
        )
      }
      const elapsedSeconds = Number(
        summary.captureTimestampUs - firstCaptureUs,
      ) / 1_000_000
      return {
        ...summary,
        timeAxisValue: elapsedSeconds,
        packetEpochMs: null,
        elapsedSeconds,
      }
    }

    if (summary.bootId !== requiredBootId) {
      throw new HistoricalTimestampError(
        `Record boot ID does not match the session time anchor at logical index ${summary.logicalIndex.toString()}.`,
      )
    }
    if (summary.captureTimestampUs < anchorCaptureUs) {
      throw new HistoricalTimestampError(
        `Capture timestamp is before the session anchor at logical index ${summary.logicalIndex.toString()}.`,
      )
    }
    const elapsedUs = summary.captureTimestampUs - anchorCaptureUs
    const packetEpochMs = anchorEpochMs + elapsedUs / 1_000n
    if (packetEpochMs > MAX_JAVASCRIPT_DATE_MS) {
      throw new HistoricalTimestampError(
        `Calculated capture time exceeds the JavaScript Date range at logical index ${summary.logicalIndex.toString()}.`,
      )
    }
    const timeAxisValue = Number(packetEpochMs)
    if (!Number.isFinite(new Date(timeAxisValue).getTime())) {
      throw new HistoricalTimestampError(
        `Calculated capture time is invalid at logical index ${summary.logicalIndex.toString()}.`,
      )
    }
    return {
      ...summary,
      timeAxisValue,
      packetEpochMs,
      elapsedSeconds: Number(elapsedUs) / 1_000_000,
    }
  })
}

export function trendCrossesLocalDateBoundary(
  summaries: ReadonlyArray<ResolvedScalarTrendSummary>,
): boolean {
  const firstEpoch = summaries[0]?.packetEpochMs
  const lastEpoch = summaries[summaries.length - 1]?.packetEpochMs
  if (firstEpoch === null || firstEpoch === undefined ||
      lastEpoch === null || lastEpoch === undefined) return false
  const first = new Date(Number(firstEpoch))
  const last = new Date(Number(lastEpoch))
  return first.getFullYear() !== last.getFullYear() ||
    first.getMonth() !== last.getMonth() ||
    first.getDate() !== last.getDate()
}

export function formatTrendAxisTick(
  value: number,
  actualTime: boolean,
  includeDate: boolean,
): string {
  if (!actualTime) return `${value.toFixed(value < 10 ? 1 : 0)}`
  return new Intl.DateTimeFormat(undefined, includeDate
    ? {
        month: 'short',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
      }
    : {
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
      }).format(new Date(value))
}

export function formatCaptureLocalDateTime(epochMs: number): string {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
    timeStyle: 'medium',
  }).format(new Date(epochMs))
}

export function formatCaptureUtcIso(epochMs: number): string {
  return new Date(epochMs).toISOString()
}

export function createHarmonicChartData(
  packet: SelectedPacketHarmonicsData,
  source: 'voltage' | 'current',
): HarmonicChartPoint[] {
  const values = source === 'voltage'
    ? packet.voltageHarmonics
    : packet.currentHarmonics
  return Array.from(values, (value, index) => ({
    harmonic: `H${index + 1}`,
    value,
  }))
}

export function chunkIndexForLogicalIndex(
  manifest: SessionManifest,
  logicalIndex: bigint,
): number {
  if (
    logicalIndex < manifest.firstLogicalIndex ||
    logicalIndex > manifest.lastLogicalIndex
  ) {
    throw new RangeError('Logical index is outside the retained session')
  }
  return Number(
    (logicalIndex - manifest.firstLogicalIndex) /
      BigInt(HISTORY_RECORDS_PER_CHUNK),
  )
}

export function findTrendPositionByLogicalIndex(
  summaries: ReadonlyArray<ScalarTrendSummary>,
  logicalIndex: bigint,
): number {
  let low = 0
  let high = summaries.length - 1
  while (low <= high) {
    const middle = low + Math.floor((high - low) / 2)
    const candidate = summaries[middle].logicalIndex
    if (candidate === logicalIndex) return middle
    if (candidate < logicalIndex) low = middle + 1
    else high = middle - 1
  }
  return -1
}

export async function loadGraphTrendData(
  options: GraphTrendLoadOptions,
): Promise<ResolvedScalarTrendSummary[]> {
  const { repository, manifest, signal, onProgress } = options
  const yieldControl = options.yieldControl ?? defaultYieldControl
  const summaries: ScalarTrendSummary[] = []
  let currentChunk = 0

  const report = (phase: GraphLoadPhase, currentChunk: number): void => {
    onProgress?.({
      phase,
      currentChunk,
      totalChunks: manifest.chunkCount,
      recordsValidated: summaries.length,
      retainedRecords: manifest.retainedCount,
      percentage: Math.min(
        100,
        (summaries.length / manifest.retainedCount) * 100,
      ),
      cancellationRequested: signal.aborted,
    })
  }

  try {
    for (let chunkIndex = 0; chunkIndex < manifest.chunkCount; chunkIndex += 1) {
      currentChunk = chunkIndex + 1
      throwIfCancelled(signal)
      report('fetching-chunk', chunkIndex + 1)
      const rawChunk = await repository.getChunk(
        manifest.sessionKey,
        formatChunkKey(chunkIndex),
      )
      throwIfCancelled(signal)
      report('validating-chunk', chunkIndex + 1)
      const chunk = validateDecodedChunk(
        rawChunk,
        manifest,
        chunkIndex,
        signal,
      )
      throwIfCancelled(signal)
      summaries.push(...chunk.records.map(extractScalarTrendSummary))
      report('extracting-summaries', chunkIndex + 1)
      await yieldControl()
    }
    if (
      summaries.length !== manifest.retainedCount ||
      summaries[0]?.logicalIndex !== manifest.firstLogicalIndex ||
      summaries[summaries.length - 1]?.logicalIndex !== manifest.lastLogicalIndex
    ) {
      throw new Error('Validated trend range differs from the manifest')
    }
    const resolved = resolveHistoricalTimestamps(manifest, summaries)
    report('complete', manifest.chunkCount)
    return resolved
  } catch (error) {
    if (
      error instanceof HistoryPipelineCancelledError ||
      signal.aborted
    ) {
      summaries.length = 0
      report('cancelled', currentChunk)
      throw new HistoryPipelineCancelledError()
    }
    summaries.length = 0
    throw error
  }
}

function recordFromCache(
  cache: GraphChunkCache,
  manifest: SessionManifest,
  logicalIndex: bigint,
): ValidatedPqr1Record {
  const recordIndex = Number(logicalIndex - cache.meta.firstLogicalIndex)
  if (recordIndex < 0 || recordIndex >= cache.meta.recordCount) {
    throw new RangeError('Selected logical index is not in the cached chunk')
  }
  const recordOffset = recordIndex * HISTORY_RECORD_BYTES
  return validatePqr1Record(
    cache.decodedBytes.subarray(
      recordOffset,
      recordOffset + HISTORY_RECORD_BYTES,
    ),
    {
      chunkIndex: cache.chunkIndex,
      recordIndex,
      expectedSessionId: manifest.sessionId,
      expectedLogicalIndex: logicalIndex,
    },
  )
}

export async function loadSelectedPacketHarmonics(
  repository: HistoryRepository,
  manifest: SessionManifest,
  logicalIndex: bigint,
  signal: AbortSignal,
  currentCache: GraphChunkCache | null,
): Promise<PacketHarmonicsLoadResult> {
  throwIfCancelled(signal)
  const chunkIndex = chunkIndexForLogicalIndex(manifest, logicalIndex)
  let cache = currentCache
  let validatedChunk: ValidatedChunkData | null = null

  if (cache === null || cache.chunkIndex !== chunkIndex) {
    validatedChunk = await fetchValidatedChunk(
      repository,
      manifest,
      chunkIndex,
      signal,
    )
    cache = {
      chunkIndex,
      meta: validatedChunk.meta,
      decodedBytes: validatedChunk.decodedBytes,
    }
  }
  throwIfCancelled(signal)
  const record = validatedChunk === null
    ? recordFromCache(cache, manifest, logicalIndex)
    : validatedChunk.records[
        Number(logicalIndex - validatedChunk.meta.firstLogicalIndex)
      ]
  if (record === undefined) {
    throw new RangeError('Selected record is absent from the validated chunk')
  }
  return {
    packet: extractSelectedPacketHarmonics(record),
    cache,
  }
}
