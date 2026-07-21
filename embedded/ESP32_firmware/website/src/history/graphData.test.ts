import { describe, expect, it } from 'vitest'
import { decodeBase64, encodeBase64 } from './base64'
import { crc32c, formatCrc32c } from './crc32c'
import {
  chunkIndexForLogicalIndex,
  createHarmonicChartData,
  createTrendChartData,
  findTrendPositionByLogicalIndex,
  formatCaptureLocalDateTime,
  formatCaptureUtcIso,
  formatTrendAxisTick,
  GRAPH_PAGE_SECTIONS,
  loadGraphTrendData,
  loadSelectedPacketHarmonics,
  metricDefinition,
  resolveHistoricalTimestamps,
  TREND_METRICS,
  trendCrossesLocalDateBoundary,
} from './graphData'
import { HistoryPipelineCancelledError } from './chunkPipeline'
import {
  createDeterministicMockHistory,
  MockHistoryRepository,
} from './mockHistory'
import type { HistoryRepository, SessionManifest } from './types'
import { isValidSessionKey, parseSessionManifest } from './validation'

interface RawChunkFixture {
  meta: Record<string, unknown>
  payload: string
}

function rawChunkFixture(value: unknown): RawChunkFixture {
  if (
    typeof value !== 'object' ||
    value === null ||
    !('meta' in value) ||
    typeof value.meta !== 'object' ||
    value.meta === null ||
    !('payload' in value) ||
    typeof value.payload !== 'string'
  ) {
    throw new Error('Mock chunk is malformed')
  }
  return { meta: { ...value.meta }, payload: value.payload }
}

async function mockManifest(): Promise<SessionManifest> {
  const repository = new MockHistoryRepository()
  return parseSessionManifest('s_42', await repository.getManifest('s_42'))
}

describe('historical graph data', () => {
  it('maps all twelve trend metrics to clear units', () => {
    expect(TREND_METRICS).toHaveLength(12)
    expect(metricDefinition('vRms')).toMatchObject({ label: 'Voltage RMS', unit: 'V' })
    expect(metricDefinition('iRms')).toMatchObject({ label: 'Current RMS', unit: 'A' })
    expect(metricDefinition('frequency').unit).toBe('Hz')
    expect(metricDefinition('powerFactor').unit).toBe('ratio')
    expect(metricDefinition('activePower').unit).toBe('W')
    expect(metricDefinition('apparentPower').unit).toBe('VA')
    expect(metricDefinition('reactivePower').unit).toBe('var')
    expect(metricDefinition('thdV').unit).toBe('%')
    expect(metricDefinition('thdI').unit).toBe('%')
    expect(metricDefinition('crestFactorV').unit).toBe('factor')
    expect(metricDefinition('crestFactorI').unit).toBe('factor')
    expect(metricDefinition('swellFactor').unit).toBe('factor')
  })

  it('defines exactly one trend and two simultaneous harmonic graph sections', () => {
    expect(GRAPH_PAGE_SECTIONS).toEqual({
      trend: 'Metric trend',
      packetSelector: 'Selected packet',
      voltageHarmonics: 'Voltage Harmonics H1-H25',
      currentHarmonics: 'Current Harmonics H1-H25',
    })
    expect(Object.values(GRAPH_PAGE_SECTIONS).join(' ')).not.toMatch(/waveform/i)
  })

  it('validates the graph session key format', () => {
    expect(isValidSessionKey('s_42')).toBe(true)
    expect(isValidSessionKey('s_bad')).toBe(false)
  })

  it('extracts compact scalar summaries from deterministic mock chunks', async () => {
    const repository = new MockHistoryRepository()
    const manifest = await mockManifest()
    const progress: number[] = []
    const summaries = await loadGraphTrendData({
      repository,
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
      onProgress: (value) => progress.push(value.recordsValidated),
    })
    expect(summaries).toHaveLength(3)
    expect(summaries[0]).toMatchObject({
      logicalIndex: 100n,
      captureTimestampUs: 5_000_000n,
      stm32Sequence: 900,
    })
    expect(summaries[0].vRms).toBeCloseTo(230.5)
    expect(summaries[2].activePower).toBeCloseTo(937.75)
    expect(progress.at(-1)).toBe(3)
    expect(summaries[0].packetEpochMs).toBeNull()
    expect(summaries.map((summary) => summary.elapsedSeconds)).toEqual([
      0,
      0.1,
      0.2,
    ])
    expect(createTrendChartData(summaries, 'vRms')).toEqual([
      expect.objectContaining({ logicalIndex: '100', metricValue: summaries[0].vRms }),
      expect.objectContaining({ logicalIndex: '101', metricValue: summaries[1].vRms }),
      expect.objectContaining({ logicalIndex: '102', metricValue: summaries[2].vRms }),
    ])
  })

  it('maps valid anchors to actual packet time with bigint-safe irregular intervals', async () => {
    const manifest = await mockManifest()
    const summaries = await loadGraphTrendData({
      repository: new MockHistoryRepository(),
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })
    const startEpochMs = 1_720_000_000_000n
    const anchoredManifest: SessionManifest = {
      ...manifest,
      schemaVersion: 2,
      sessionTimeValid: true,
      sessionStartEpochMs: startEpochMs,
      sessionStartCaptureTimestampUs: 5_000_000n,
      sessionEndTimeValid: false,
      sessionEndEpochMs: null,
      sessionBootId: summaries[0].bootId,
      timeSource: 'ntp',
      // Upload timestamps deliberately differ and must never affect capture time.
      uploadStartedAt: 1,
      uploadCompletedAt: 2,
    }
    const irregular = [
      { ...summaries[0], captureTimestampUs: 5_000_000n },
      { ...summaries[1], captureTimestampUs: 5_123_999n },
      { ...summaries[2], captureTimestampUs: 5_901_001n },
    ]
    const resolved = resolveHistoricalTimestamps(anchoredManifest, irregular)
    expect(resolved[0].packetEpochMs).toBe(startEpochMs)
    expect(resolved[1].packetEpochMs).toBe(startEpochMs + 123n)
    expect(resolved[2].packetEpochMs).toBe(startEpochMs + 901n)
    expect(resolved[0].packetEpochMs).not.toBe(BigInt(anchoredManifest.uploadStartedAt))
  })

  it('rejects non-monotonic, pre-anchor, and wrong-boot capture records', async () => {
    const manifest = await mockManifest()
    const summaries = await loadGraphTrendData({
      repository: new MockHistoryRepository(),
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })
    const anchored: SessionManifest = {
      ...manifest,
      schemaVersion: 2,
      sessionTimeValid: true,
      sessionStartEpochMs: 1_720_000_000_000n,
      sessionStartCaptureTimestampUs: 5_000_000n,
      sessionEndTimeValid: false,
      sessionEndEpochMs: null,
      sessionBootId: summaries[0].bootId,
      timeSource: 'ntp',
    }
    expect(() => resolveHistoricalTimestamps(anchored, [
      summaries[0],
      { ...summaries[1], captureTimestampUs: 4_999_999n },
    ])).toThrow(/not monotonic/i)
    expect(() => resolveHistoricalTimestamps(
      { ...anchored, sessionStartCaptureTimestampUs: 5_000_001n },
      [summaries[0]],
    )).toThrow(/before the session anchor/i)
    expect(() => resolveHistoricalTimestamps(anchored, [
      { ...summaries[0], bootId: summaries[0].bootId + 1 },
    ])).toThrow(/boot ID/i)
  })

  it('formats local/UTC capture time and includes the date across local midnight', async () => {
    const manifest = await mockManifest()
    const summaries = await loadGraphTrendData({
      repository: new MockHistoryRepository(),
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })
    const localStartMs = BigInt(new Date(2026, 0, 1, 23, 59, 59, 900).getTime())
    const anchored: SessionManifest = {
      ...manifest,
      schemaVersion: 2,
      sessionTimeValid: true,
      sessionStartEpochMs: localStartMs,
      sessionStartCaptureTimestampUs: 5_000_000n,
      sessionEndTimeValid: false,
      sessionEndEpochMs: null,
      sessionBootId: summaries[0].bootId,
      timeSource: 'ntp',
    }
    const resolved = resolveHistoricalTimestamps(anchored, [
      { ...summaries[0], captureTimestampUs: 5_000_000n },
      { ...summaries[1], captureTimestampUs: 5_200_000n },
    ])
    expect(trendCrossesLocalDateBoundary(resolved)).toBe(true)
    const epochNumber = Number(resolved[0].packetEpochMs)
    expect(formatCaptureUtcIso(epochNumber)).toBe(new Date(epochNumber).toISOString())
    expect(formatCaptureLocalDateTime(epochNumber)).toBe(
      new Intl.DateTimeFormat(undefined, {
        dateStyle: 'medium',
        timeStyle: 'medium',
      }).format(new Date(epochNumber)),
    )
    expect(formatTrendAxisTick(epochNumber, true, true)).toMatch(/\d/)
    expect(formatTrendAxisTick(1.25, false, false)).toBe('1.3')
  })

  it('finds logical indexes and their deterministic chunks', async () => {
    const manifest = await mockManifest()
    const repository = new MockHistoryRepository()
    const summaries = await loadGraphTrendData({
      repository,
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })
    expect(findTrendPositionByLogicalIndex(summaries, 101n)).toBe(1)
    expect(findTrendPositionByLogicalIndex(summaries, 999n)).toBe(-1)
    expect(chunkIndexForLogicalIndex(manifest, 102n)).toBe(0)
    const extended = {
      ...manifest,
      retainedCount: 16,
      chunkCount: 2,
      lastLogicalIndex: 115n,
    }
    expect(chunkIndexForLogicalIndex(extended, 108n)).toBe(1)
    expect(() => chunkIndexForLogicalIndex(manifest, 99n)).toThrow(RangeError)
  })

  it('extracts voltage/current harmonics without retaining waveform arrays', async () => {
    const repository = new MockHistoryRepository()
    const manifest = await mockManifest()
    const result = await loadSelectedPacketHarmonics(
      repository,
      manifest,
      100n,
      new AbortController().signal,
      null,
    )
    expect(result.packet.voltageHarmonics).toHaveLength(25)
    expect(result.packet.currentHarmonics).toHaveLength(25)
    expect(result.packet.voltageHarmonics[0]).toBeCloseTo(0.11)
    expect(result.packet.voltageHarmonics[24]).toBeCloseTo(2.51)
    expect(result.packet.currentHarmonics[0]).toBeCloseTo(0.16)
    expect(result.packet.currentHarmonics[24]).toBeCloseTo(3.76)
    expect(result.packet).not.toHaveProperty('voltageSamples')
    expect(result.packet).not.toHaveProperty('currentSamples')
    const voltageHarmonics = createHarmonicChartData(result.packet, 'voltage')
    const currentHarmonics = createHarmonicChartData(result.packet, 'current')
    expect(voltageHarmonics[0]).toMatchObject({ harmonic: 'H1' })
    expect(voltageHarmonics[24]).toMatchObject({ harmonic: 'H25' })
    expect(currentHarmonics[0].value).toBeCloseTo(0.16)
    expect(currentHarmonics[24].value).toBeCloseTo(3.76)
  })

  it('reuses one validated decoded chunk for packet navigation', async () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    let fetchCount = 0
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: (_sessionKey, key) => {
        fetchCount += 1
        return Promise.resolve(fixture.chunks.get(key) ?? null)
      },
    }
    const first = await loadSelectedPacketHarmonics(
      repository,
      manifest,
      100n,
      new AbortController().signal,
      null,
    )
    const second = await loadSelectedPacketHarmonics(
      repository,
      manifest,
      101n,
      new AbortController().signal,
      first.cache,
    )
    expect(second.packet.logicalIndex).toBe(101n)
    expect(second.packet.stm32Sequence).toBe(901)
    expect(fetchCount).toBe(1)
  })

  it('cancels graph loading without returning partial summaries', async () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    const controller = new AbortController()
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: (_sessionKey, key) => {
        controller.abort()
        return Promise.resolve(fixture.chunks.get(key) ?? null)
      },
    }
    await expect(loadGraphTrendData({
      repository,
      manifest,
      signal: controller.signal,
      yieldControl: () => Promise.resolve(),
    })).rejects.toBeInstanceOf(HistoryPipelineCancelledError)
  })

  it('rejects malformed chunks and corrupted PQR1 records', async () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    const malformedRepository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: () => Promise.resolve({ broken: true }),
    }
    await expect(loadGraphTrendData({
      repository: malformedRepository,
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })).rejects.toThrow()

    const chunk = rawChunkFixture(fixture.chunks.get('000000'))
    const bytes = decodeBase64(chunk.payload)
    bytes[64] ^= 1
    const corrupted = {
      meta: { ...chunk.meta, crc32c: formatCrc32c(crc32c(bytes)) },
      payload: encodeBase64(bytes),
    }
    const corruptedRepository: HistoryRepository = {
      ...malformedRepository,
      getChunk: () => Promise.resolve(corrupted),
    }
    await expect(loadGraphTrendData({
      repository: corruptedRepository,
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })).rejects.toThrow(/validation failed/)
  })
})
