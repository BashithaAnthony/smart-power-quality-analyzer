import type {
  ExportIntegrityReport,
  ExportProgress as ExportProgressState,
} from '../history/types'

const PHASE_LABELS: Record<ExportProgressState['phase'], string> = {
  preparing: 'Preparing export',
  'fetching-chunk': 'Downloading encrypted database chunk',
  'validating-chunk': 'Validating chunk integrity',
  'processing-records': 'Validating records and generating CSV',
  'building-file': 'Assembling download file',
  complete: 'Export complete',
  cancelled: 'Export cancelled',
}

function formatBytes(bytes: number): string {
  if (bytes >= 1_048_576) return `${(bytes / 1_048_576).toFixed(1)} MiB`
  if (bytes >= 1_024) return `${(bytes / 1_024).toFixed(1)} KiB`
  return `${bytes} bytes`
}

export function ExportProgress({
  progress,
  onCancel,
}: {
  progress: ExportProgressState
  onCancel: () => void
}) {
  return (
    <section className="export-progress" aria-live="polite" aria-busy="true">
      <div className="export-progress-heading">
        <div>
          <p className="eyebrow">Browser validation pipeline</p>
          <h3>{PHASE_LABELS[progress.phase]}</h3>
        </div>
        <strong>{progress.percentage.toFixed(1)}%</strong>
      </div>
      <progress max="100" value={progress.percentage}>
        {progress.percentage.toFixed(1)}%
      </progress>
      <div className="export-progress-detail">
        <span>
          Chunk {progress.currentChunk} / {progress.totalChunks}
        </span>
        <span>
          {progress.recordsProcessed.toLocaleString()} /{' '}
          {progress.retainedRecords.toLocaleString()} records
        </span>
      </div>
      <button
        type="button"
        className="danger-button"
        onClick={onCancel}
        disabled={progress.cancellationRequested}
      >
        {progress.cancellationRequested ? 'Cancellation requested…' : 'Cancel download'}
      </button>
    </section>
  )
}

export function ExportIntegrity({
  report,
}: {
  report: ExportIntegrityReport
}) {
  return (
    <section className="integrity-report" aria-live="polite">
      <div className="section-heading compact-heading">
        <div>
          <p className="eyebrow">Integrity report</p>
          <h3>Export validated successfully</h3>
        </div>
        <span className="badge badge-complete">Verified</span>
      </div>
      <dl className="integrity-grid">
        <div><dt>Chunks validated</dt><dd>{report.chunksValidated}</dd></div>
        <div><dt>Records validated</dt><dd>{report.recordsValidated}</dd></div>
        <div><dt>Logical range</dt><dd>{report.firstLogicalIndex.toString()}–{report.lastLogicalIndex.toString()}</dd></div>
        <div><dt>Chunk CRC32C</dt><dd>{report.chunkCrcValidationPassed ? 'Passed' : 'Failed'}</dd></div>
        <div><dt>Record CRC32C</dt><dd>{report.recordCrcValidationPassed ? 'Passed' : 'Failed'}</dd></div>
        <div><dt>CSV rows</dt><dd>{report.csvRowCount.toLocaleString()}</dd></div>
        <div><dt>File size</dt><dd>{formatBytes(report.fileSizeBytes)}</dd></div>
        <div><dt>Duration</dt><dd>{(report.durationMs / 1_000).toFixed(1)} s</dd></div>
      </dl>
    </section>
  )
}
