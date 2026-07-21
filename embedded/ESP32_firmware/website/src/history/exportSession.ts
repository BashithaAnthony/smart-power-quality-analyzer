import {
  BoundedCsvParts,
  waveformCsvHeader,
  waveformCsvRow,
} from './csv'
import {
  fetchValidatedChunk,
  HistoryChunkPipelineError,
  HistoryPipelineCancelledError,
} from './chunkPipeline'
import {
  HISTORY_RECORDS_PER_CHUNK,
  HISTORY_WAVEFORM_SAMPLES,
  type ExportProgress,
  type HistoryRepository,
  type SessionExportResult,
  type SessionManifest,
} from './types'

export class ExportCancelledError extends Error {
  constructor() {
    super('Export cancelled')
    this.name = 'ExportCancelledError'
  }
}

export class SessionExportError extends Error {
  readonly chunkIndex: number | null

  constructor(message: string, chunkIndex: number | null = null) {
    super(message)
    this.name = 'SessionExportError'
    this.chunkIndex = chunkIndex
  }
}

export interface ExportSessionOptions {
  repository: HistoryRepository
  manifest: SessionManifest
  signal: AbortSignal
  onProgress?: (progress: ExportProgress) => void
  yieldControl?: () => Promise<void>
  now?: () => number
}

function defaultYieldControl(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0))
}

function throwIfCancelled(signal: AbortSignal): void {
  if (signal.aborted) throw new ExportCancelledError()
}

function percentage(recordsProcessed: number, retainedRecords: number): number {
  if (retainedRecords === 0) return 0
  return Math.min(100, (recordsProcessed / retainedRecords) * 100)
}

export async function exportSessionToCsv(
  options: ExportSessionOptions,
): Promise<SessionExportResult> {
  const {
    repository,
    manifest,
    signal,
    onProgress,
  } = options
  const yieldControl = options.yieldControl ?? defaultYieldControl
  const now = options.now ?? Date.now
  const startedAt = now()
  const parts = new BoundedCsvParts()
  let recordsProcessed = 0
  let chunksValidated = 0
  let csvRowCount = 0
  let lastLogicalIndex: bigint | null = null

  const reportProgress = (
    phase: ExportProgress['phase'],
    currentChunk: number,
  ): void => {
    onProgress?.({
      phase,
      currentChunk,
      totalChunks: manifest.chunkCount,
      recordsProcessed,
      retainedRecords: manifest.retainedCount,
      percentage: percentage(recordsProcessed, manifest.retainedCount),
      cancellationRequested: signal.aborted,
    })
  }

  try {
    throwIfCancelled(signal)
    reportProgress('preparing', 0)
    parts.append(waveformCsvHeader())

    for (let chunkIndex = 0; chunkIndex < manifest.chunkCount; chunkIndex += 1) {
      throwIfCancelled(signal)
      reportProgress('fetching-chunk', chunkIndex + 1)

      try {
        const chunk = await fetchValidatedChunk(
          repository,
          manifest,
          chunkIndex,
          signal,
          () => reportProgress('validating-chunk', chunkIndex + 1),
        )

        for (let recordIndex = 0; recordIndex < chunk.records.length; recordIndex += 1) {
          throwIfCancelled(signal)
          reportProgress('processing-records', chunkIndex + 1)
          const record = chunk.records[recordIndex]
          if (
            lastLogicalIndex !== null &&
            record.metadata.logicalRecordIndex !== lastLogicalIndex + 1n
          ) {
            throw new SessionExportError(
              `Chunk ${chunkIndex}, record ${recordIndex}: logical indexes are not consecutive`,
              chunkIndex,
            )
          }
          lastLogicalIndex = record.metadata.logicalRecordIndex

          for (
            let sampleIndex = 0;
            sampleIndex < HISTORY_WAVEFORM_SAMPLES;
            sampleIndex += 1
          ) {
            if (sampleIndex > 0 && (sampleIndex & 511) === 0) {
              throwIfCancelled(signal)
              await yieldControl()
            }
            parts.append(waveformCsvRow(manifest, record, sampleIndex))
            csvRowCount += 1
          }
          recordsProcessed += 1
          await yieldControl()
        }

        chunksValidated += 1
        parts.flushPart()
      } catch (error) {
        if (
          error instanceof ExportCancelledError ||
          error instanceof HistoryPipelineCancelledError
        ) {
          throw new ExportCancelledError()
        }
        if (error instanceof SessionExportError) throw error
        if (error instanceof HistoryChunkPipelineError) {
          throw new SessionExportError(error.message, error.chunkIndex)
        }
        const detail = error instanceof Error ? error.message : 'unknown error'
        throw new SessionExportError(
          `Chunk ${chunkIndex}: ${detail}`,
          chunkIndex,
        )
      }

      // The Base64 payload, decoded bytes, and parsed records have
      // left scope before the next one-time Firebase request begins.
      await yieldControl()
    }

    if (
      recordsProcessed !== manifest.retainedCount ||
      lastLogicalIndex !== manifest.lastLogicalIndex
    ) {
      throw new SessionExportError(
        'Validated record count or final logical index differs from the manifest',
      )
    }
    reportProgress('building-file', manifest.chunkCount)
    throwIfCancelled(signal)
    const blob = new Blob(parts.finish(), { type: 'text/csv;charset=utf-8' })
    const fileName = `${manifest.deviceId}_session_${manifest.sessionId.toString()}_waveforms.csv`
    reportProgress('complete', manifest.chunkCount)
    return {
      blob,
      fileName,
      report: {
        kind: 'waveforms',
        chunksValidated,
        recordsValidated: recordsProcessed,
        firstLogicalIndex: manifest.firstLogicalIndex,
        lastLogicalIndex: manifest.lastLogicalIndex,
        chunkCrcValidationPassed: true,
        recordCrcValidationPassed: true,
        csvRowCount,
        fileSizeBytes: blob.size,
        durationMs: Math.max(0, now() - startedAt),
      },
    }
  } catch (error) {
    parts.discard()
    if (error instanceof ExportCancelledError) {
      reportProgress('cancelled', Math.min(manifest.chunkCount, chunksValidated + 1))
    }
    throw error
  }
}

export function expectedFinalChunkRecordCount(retainedCount: number): number {
  const remainder = retainedCount % HISTORY_RECORDS_PER_CHUNK
  return remainder === 0 ? HISTORY_RECORDS_PER_CHUNK : remainder
}
