/** @vitest-environment jsdom */

import type { ReactNode } from 'react'
import {
  act,
  cleanup,
  fireEvent,
  render,
  screen,
  waitFor,
} from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { encodeBase64 } from '../history/base64'
import { crc32c, formatCrc32c } from '../history/crc32c'
import {
  createDeterministicMockHistory,
  createPqr1Fixture,
} from '../history/mockHistory'
import {
  HISTORY_DEVICE_ID,
  HISTORY_RECORD_BYTES,
  HISTORY_RECORD_FORMAT,
  HISTORY_RECORDS_PER_CHUNK,
  type HistoryRepository,
  type SessionManifest,
} from '../history/types'
import {
  calculateManifestCrc32c,
  formatChunkKey,
} from '../history/validation'
import { SessionGraphsPage } from './SessionGraphsPage'
import { invalidateSession } from '../history/sessionInvalidation'

vi.mock('recharts', () => {
  function ChartContainer({ children }: { children?: ReactNode }) {
    return <div>{children}</div>
  }

  return {
    Bar: () => null,
    BarChart: ChartContainer,
    CartesianGrid: () => null,
    Line: () => null,
    LineChart: ChartContainer,
    ResponsiveContainer: ChartContainer,
    Tooltip: () => null,
    XAxis: () => null,
    YAxis: () => null,
  }
})

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
})

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

function createChunk(
  manifest: SessionManifest,
  chunkIndex: number,
  recordCount: number,
): unknown {
  const firstOrdinal = chunkIndex * HISTORY_RECORDS_PER_CHUNK
  const firstLogicalIndex = manifest.firstLogicalIndex + BigInt(firstOrdinal)
  const bytes = new Uint8Array(recordCount * HISTORY_RECORD_BYTES)
  for (let recordIndex = 0; recordIndex < recordCount; recordIndex += 1) {
    const ordinal = firstOrdinal + recordIndex
    bytes.set(
      createPqr1Fixture({
        sessionId: manifest.sessionId,
        logicalIndex: manifest.firstLogicalIndex + BigInt(ordinal),
        captureTimestampUs: 5_000_000n + BigInt(ordinal * 100_000),
        stm32Sequence: manifest.firstStm32Sequence + ordinal,
        bootId: 0x1234abcd,
        seed: ordinal + 1,
      }),
      recordIndex * HISTORY_RECORD_BYTES,
    )
  }
  return {
    meta: {
      schemaVersion: 1,
      chunkIndex,
      firstLogicalIndex: firstLogicalIndex.toString(),
      lastLogicalIndex: (firstLogicalIndex + BigInt(recordCount - 1)).toString(),
      recordCount,
      recordSize: HISTORY_RECORD_BYTES,
      rawBytes: bytes.byteLength,
      crc32c: formatCrc32c(crc32c(bytes)),
      encoding: 'base64',
      recordFormat: HISTORY_RECORD_FORMAT,
    },
    payload: encodeBase64(bytes),
  }
}

function createMultiChunkFixture(): {
  manifest: SessionManifest
  rawManifest: Record<string, unknown>
  chunks: ReadonlyMap<string, unknown>
} {
  const retainedCount = 9
  const manifest: SessionManifest = {
    sessionKey: 's_84',
    schemaVersion: 1,
    state: 'complete',
    deviceId: HISTORY_DEVICE_ID,
    sessionId: 84n,
    persistentState: 'Finalized',
    retainedCount,
    totalStored: BigInt(retainedCount),
    overwrittenCount: 0n,
    firstLogicalIndex: 100n,
    lastLogicalIndex: 108n,
    truncated: false,
    recoveredIncomplete: false,
    countersPartial: false,
    chunkCount: 2,
    uploadStartedAt: Date.UTC(2026, 6, 18, 4, 30, 0),
    uploadCompletedAt: Date.UTC(2026, 6, 18, 4, 30, 8),
    recordFormat: HISTORY_RECORD_FORMAT,
    recordSize: HISTORY_RECORD_BYTES,
    recordsPerChunk: HISTORY_RECORDS_PER_CHUNK,
    nextChunk: 2,
    uploadedRecords: retainedCount,
    firstStm32Sequence: 900,
    lastStm32Sequence: 908,
    sourceMetadataGeneration: 11n,
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
  return {
    manifest,
    rawManifest: rawManifest(manifest),
    chunks: new Map([
      [formatChunkKey(0), createChunk(manifest, 0, 8)],
      [formatChunkKey(1), createChunk(manifest, 1, 1)],
    ]),
  }
}

function harmonicPanels(container: HTMLElement): HTMLElement[] {
  return Array.from(container.querySelectorAll<HTMLElement>('.harmonic-panel'))
}

async function waitForSelectedLogicalIndex(
  container: HTMLElement,
  logicalIndex: string,
): Promise<void> {
  await waitFor(() => {
    const panels = harmonicPanels(container)
    expect(panels).toHaveLength(2)
    expect(panels.every((panel) =>
      panel.dataset.logicalIndex === logicalIndex &&
      panel.getAttribute('aria-busy') === 'false')).toBe(true)
  })
}

describe('historical session graph page', () => {
  it('aborts in-flight graph work and releases graph state after session deletion', async () => {
    const fixture = createDeterministicMockHistory()
    let resolveChunk: ((value: unknown) => void) | null = null
    const pendingChunk = new Promise<unknown>((resolve) => {
      resolveChunk = resolve
    })
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: () => pendingChunk,
    }
    const rendered = render(
      <SessionGraphsPage repository={repository} sessionKey="s_42" />,
    )
    await screen.findByRole('heading', { name: 'Session 42' })
    fireEvent.click(screen.getByRole('button', { name: 'Load Graph Data' }))
    await screen.findByRole('button', { name: 'Cancel loading' })

    act(() => invalidateSession('s_42'))
    expect(await screen.findByText(
      'This uploaded session no longer exists in cloud history.',
    )).toBeTruthy()
    expect(rendered.container.querySelector('[aria-labelledby="trend-title"]')).toBeNull()
    expect(rendered.container.querySelector('.harmonic-panel')).toBeNull()

    await act(async () => {
      resolveChunk?.(fixture.chunks.get('000000'))
      await pendingChunk
    })
    expect(rendered.container.querySelector('[aria-labelledby="trend-title"]')).toBeNull()
  })

  it('keeps the graph page mounted and preserves scroll, focus, route, trend, and cache during packet selection', async () => {
    const fixture = createDeterministicMockHistory()
    let manifestReads = 0
    let chunkReads = 0
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => {
        manifestReads += 1
        return Promise.resolve(fixture.manifest)
      },
      getChunk: (_sessionKey, chunkKey) => {
        chunkReads += 1
        return Promise.resolve(fixture.chunks.get(chunkKey) ?? null)
      },
    }
    const scrollTo = vi.spyOn(window, 'scrollTo').mockImplementation(() => undefined)
    const pushState = vi.spyOn(window.history, 'pushState')
    Object.defineProperty(window, 'scrollY', {
      configurable: true,
      value: 720,
    })

    const rendered = render(
      <SessionGraphsPage repository={repository} sessionKey="s_42" />,
    )
    await screen.findByRole('heading', { name: 'Session 42' })
    fireEvent.click(screen.getByRole('button', { name: 'Load Graph Data' }))
    await screen.findByRole('heading', { name: 'Metric trend' })
    await waitForSelectedLogicalIndex(rendered.container, '100')
    expect(screen.getByText(
      'Actual capture time is unavailable for this session.',
    )).toBeTruthy()
    expect(screen.getByText('X-axis: Elapsed time (s)')).toBeTruthy()

    expect(screen.getByRole('heading', { name: 'Voltage Harmonics H1-H25' })).toBeTruthy()
    expect(screen.getByRole('heading', { name: 'Current Harmonics H1-H25' })).toBeTruthy()
    expect(screen.queryByLabelText('Harmonic source')).toBeNull()
    expect(rendered.container.textContent).not.toMatch(/voltage waveform|current waveform/i)
    expect(rendered.container.querySelector('form')).toBeNull()

    const page = rendered.container.querySelector('main.graph-page')
    const trend = rendered.container.querySelector('[aria-labelledby="trend-title"]')
    const slider = screen.getByLabelText('Packet position')
    slider.focus()
    const readsAfterInitialPacket = chunkReads
    fireEvent.change(slider, { target: { value: '1' } })
    await waitForSelectedLogicalIndex(rendered.container, '101')

    expect(document.activeElement).toBe(slider)
    expect(rendered.container.querySelector('main.graph-page')).toBe(page)
    expect(rendered.container.querySelector('[aria-labelledby="trend-title"]')).toBe(trend)
    expect(window.scrollY).toBe(720)
    expect(scrollTo).not.toHaveBeenCalled()
    expect(pushState).not.toHaveBeenCalled()
    expect(manifestReads).toBe(1)
    expect(chunkReads).toBe(readsAfterInitialPacket)

    const next = screen.getByRole('button', { name: 'Next packet' })
    const previous = screen.getByRole('button', { name: 'Previous packet' })
    expect(next.getAttribute('type')).toBe('button')
    expect(previous.getAttribute('type')).toBe('button')
    fireEvent.click(next)
    await waitForSelectedLogicalIndex(rendered.container, '102')
    fireEvent.click(previous)
    await waitForSelectedLogicalIndex(rendered.container, '101')
    fireEvent.change(screen.getByLabelText('Logical index'), {
      target: { value: '0' },
    })
    await waitForSelectedLogicalIndex(rendered.container, '100')

    expect(rendered.container.querySelector('main.graph-page')).toBe(page)
    expect(chunkReads).toBe(readsAfterInitialPacket)
    expect(scrollTo).not.toHaveBeenCalled()
    expect(pushState).not.toHaveBeenCalled()
  })

  it('ignores a stale cross-chunk response after a newer cached selection wins', async () => {
    const fixture = createMultiChunkFixture()
    let chunkReads = 0
    let resolveStaleRequest: ((value: unknown) => void) | null = null
    const staleRequest = new Promise<unknown>((resolve) => {
      resolveStaleRequest = resolve
    })
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve({ sessions: [], malformedEntries: [] }),
      getManifest: () => Promise.resolve(fixture.rawManifest),
      getChunk: (_sessionKey, chunkKey) => {
        chunkReads += 1
        if (chunkReads === 4) return staleRequest
        return Promise.resolve(fixture.chunks.get(chunkKey) ?? null)
      },
    }

    const rendered = render(
      <SessionGraphsPage repository={repository} sessionKey="s_84" />,
    )
    await screen.findByRole('heading', { name: 'Session 84' })
    fireEvent.click(screen.getByRole('button', { name: 'Load Graph Data' }))
    await waitForSelectedLogicalIndex(rendered.container, '100')

    const slider = screen.getByLabelText('Packet position')
    fireEvent.change(slider, { target: { value: '8' } })
    await waitFor(() => expect(chunkReads).toBe(4))
    fireEvent.change(slider, { target: { value: '1' } })
    await waitForSelectedLogicalIndex(rendered.container, '101')

    const lateChunk = fixture.chunks.get(formatChunkKey(1))
    await act(async () => {
      resolveStaleRequest?.(lateChunk)
      await staleRequest
    })
    await new Promise((resolve) => window.setTimeout(resolve, 0))

    expect(harmonicPanels(rendered.container).every((panel) =>
      panel.dataset.logicalIndex === '101')).toBe(true)
    expect(chunkReads).toBe(4)
  })
})
