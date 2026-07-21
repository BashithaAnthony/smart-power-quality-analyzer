import { describe, expect, it } from 'vitest'
import {
  ExportCancelledError,
  expectedFinalChunkRecordCount,
  exportSessionToCsv,
  SessionExportError,
} from './exportSession'
import {
  createDeterministicMockHistory,
  MockHistoryRepository,
} from './mockHistory'
import type { HistoryRepository } from './types'
import { parseSessionManifest } from './validation'

describe('incremental session export', () => {
  it('calculates final partial chunk sizes', () => {
    expect(expectedFinalChunkRecordCount(1)).toBe(1)
    expect(expectedFinalChunkRecordCount(8)).toBe(8)
    expect(expectedFinalChunkRecordCount(9)).toBe(1)
    expect(expectedFinalChunkRecordCount(2_674)).toBe(2)
  })

  it('generates the full waveform export through the shared validation pipeline', async () => {
    const repository = new MockHistoryRepository()
    const manifest = parseSessionManifest(
      's_42',
      await repository.getManifest('s_42'),
    )
    const waveforms = await exportSessionToCsv({
      repository,
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })
    expect(waveforms.report.csvRowCount).toBe(3 * 1_024)
    expect(waveforms.fileName).toBe('PQ-3PH-001_session_42_waveforms.csv')
  })

  it('cancels after a chunk request without downloading a partial result', async () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    const controller = new AbortController()
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: (_sessionKey, chunkKey) => {
        controller.abort()
        return Promise.resolve(fixture.chunks.get(chunkKey) ?? null)
      },
    }
    await expect(exportSessionToCsv({
      repository,
      manifest,
      signal: controller.signal,
      yieldControl: () => Promise.resolve(),
    })).rejects.toBeInstanceOf(ExportCancelledError)
  })

  it('stops on a corrupted chunk CRC', async () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    const sourceChunk = fixture.chunks.get('000000')
    if (
      typeof sourceChunk !== 'object' ||
      sourceChunk === null ||
      !('meta' in sourceChunk)
    ) {
      throw new Error('Mock fixture chunk is malformed')
    }
    const meta = sourceChunk.meta
    if (typeof meta !== 'object' || meta === null) {
      throw new Error('Mock fixture metadata is malformed')
    }
    const corruptedChunk = {
      ...sourceChunk,
      meta: { ...meta, crc32c: '00000000' },
    }
    const repository: HistoryRepository = {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: () => Promise.resolve(corruptedChunk),
    }
    await expect(exportSessionToCsv({
      repository,
      manifest,
      signal: new AbortController().signal,
      yieldControl: () => Promise.resolve(),
    })).rejects.toBeInstanceOf(SessionExportError)
  })
})
