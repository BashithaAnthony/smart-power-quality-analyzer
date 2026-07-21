import { decodeBase64 } from './base64'
import { crc32c, formatCrc32c } from './crc32c'
import { validatePqr1Record } from './pqr1'
import {
  HISTORY_RECORD_BYTES,
  type HistoryRepository,
  type SessionChunkMeta,
  type SessionManifest,
  type ValidatedPqr1Record,
} from './types'
import { formatChunkKey, parseSessionChunk } from './validation'

export class HistoryPipelineCancelledError extends Error {
  constructor() {
    super('History operation cancelled')
    this.name = 'HistoryPipelineCancelledError'
  }
}

export class HistoryChunkPipelineError extends Error {
  readonly chunkIndex: number

  constructor(chunkIndex: number, message: string) {
    super(`Chunk ${chunkIndex}: ${message}`)
    this.name = 'HistoryChunkPipelineError'
    this.chunkIndex = chunkIndex
  }
}

export interface ValidatedChunkData {
  meta: SessionChunkMeta
  decodedBytes: Uint8Array
  records: ValidatedPqr1Record[]
}

function throwIfCancelled(signal: AbortSignal): void {
  if (signal.aborted) throw new HistoryPipelineCancelledError()
}

export function validateDecodedChunk(
  value: unknown,
  manifest: SessionManifest,
  chunkIndex: number,
  signal: AbortSignal,
): ValidatedChunkData {
  throwIfCancelled(signal)
  const chunk = parseSessionChunk(value, manifest, chunkIndex)
  const decodedBytes = decodeBase64(chunk.payload)
  if (decodedBytes.byteLength !== chunk.meta.rawBytes) {
    throw new HistoryChunkPipelineError(
      chunkIndex,
      'decoded byte length differs from rawBytes',
    )
  }
  if (formatCrc32c(crc32c(decodedBytes)) !== chunk.meta.crc32c) {
    throw new HistoryChunkPipelineError(
      chunkIndex,
      'chunk CRC32C validation failed',
    )
  }

  const records: ValidatedPqr1Record[] = []
  for (let recordIndex = 0; recordIndex < chunk.meta.recordCount; recordIndex += 1) {
    throwIfCancelled(signal)
    const recordOffset = recordIndex * HISTORY_RECORD_BYTES
    const expectedLogicalIndex =
      chunk.meta.firstLogicalIndex + BigInt(recordIndex)
    const record = validatePqr1Record(
      decodedBytes.subarray(recordOffset, recordOffset + HISTORY_RECORD_BYTES),
      {
        chunkIndex,
        recordIndex,
        expectedSessionId: manifest.sessionId,
        expectedLogicalIndex,
      },
    )
    records.push(record)
  }

  const firstRecord = records[0]
  const lastRecord = records[records.length - 1]
  if (
    chunkIndex === 0 &&
    firstRecord.metadata.stm32Sequence !== manifest.firstStm32Sequence
  ) {
    throw new HistoryChunkPipelineError(
      chunkIndex,
      'first STM32 sequence differs from the manifest',
    )
  }
  if (
    chunkIndex === manifest.chunkCount - 1 &&
    lastRecord.metadata.stm32Sequence !== manifest.lastStm32Sequence
  ) {
    throw new HistoryChunkPipelineError(
      chunkIndex,
      'last STM32 sequence differs from the manifest',
    )
  }

  return { meta: chunk.meta, decodedBytes, records }
}

export async function fetchValidatedChunk(
  repository: HistoryRepository,
  manifest: SessionManifest,
  chunkIndex: number,
  signal: AbortSignal,
  onFetched?: () => void,
): Promise<ValidatedChunkData> {
  throwIfCancelled(signal)
  const value = await repository.getChunk(
    manifest.sessionKey,
    formatChunkKey(chunkIndex),
  )
  throwIfCancelled(signal)
  onFetched?.()
  return validateDecodedChunk(value, manifest, chunkIndex, signal)
}
