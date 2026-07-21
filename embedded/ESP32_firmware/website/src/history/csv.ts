import type { SessionManifest, ValidatedPqr1Record } from './types'

export const UTF8_BOM = '\uFEFF'
export const CSV_LINE_ENDING = '\r\n'

export const WAVEFORM_CSV_COLUMNS = [
  'device_id',
  'cloud_session_key',
  'session_id',
  'logical_index',
  'capture_timestamp',
  'stm32_sequence',
  'session_upload_started_at_iso',
  'session_upload_completed_at_iso',
  'sample_index',
  'voltage_sample',
  'current_sample',
] as const

export function escapeCsvField(value: string | number | bigint | boolean): string {
  const text = String(value)
  return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text
}

function csvLine(values: ReadonlyArray<string | number | bigint | boolean>): string {
  return `${values.map(escapeCsvField).join(',')}${CSV_LINE_ENDING}`
}

export function waveformCsvHeader(): string {
  return `${UTF8_BOM}${csvLine(WAVEFORM_CSV_COLUMNS)}`
}

export function waveformCsvRow(
  manifest: SessionManifest,
  record: ValidatedPqr1Record,
  sampleIndex: number,
): string {
  const { metadata, packet } = record
  return csvLine([
    manifest.deviceId,
    manifest.sessionKey,
    manifest.sessionId,
    metadata.logicalRecordIndex,
    metadata.captureTimestampUs,
    metadata.stm32Sequence,
    new Date(manifest.uploadStartedAt).toISOString(),
    new Date(manifest.uploadCompletedAt).toISOString(),
    sampleIndex,
    packet.voltageSamples[sampleIndex],
    packet.currentSamples[sampleIndex],
  ])
}

/** Keeps each retained string part bounded before it becomes a Blob part. */
export class BoundedCsvParts {
  private readonly blobParts: BlobPart[] = []
  private readonly pending: string[] = []
  private pendingCharacters = 0
  private readonly maximumPartCharacters: number

  constructor(maximumPartCharacters = 512 * 1_024) {
    this.maximumPartCharacters = maximumPartCharacters
  }

  append(text: string): void {
    this.pending.push(text)
    this.pendingCharacters += text.length
    if (this.pendingCharacters >= this.maximumPartCharacters) this.flush()
  }

  finish(): BlobPart[] {
    this.flush()
    return this.blobParts
  }

  flushPart(): void {
    this.flush()
  }

  discard(): void {
    this.pending.length = 0
    this.blobParts.length = 0
    this.pendingCharacters = 0
  }

  private flush(): void {
    if (this.pending.length === 0) return
    this.blobParts.push(this.pending.join(''))
    this.pending.length = 0
    this.pendingCharacters = 0
  }
}
