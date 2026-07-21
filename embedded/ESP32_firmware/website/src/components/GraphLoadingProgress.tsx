import type { GraphLoadProgress } from '../history/graphData'

const PHASE_LABELS: Record<GraphLoadProgress['phase'], string> = {
  'fetching-chunk': 'Downloading graph data chunk',
  'validating-chunk': 'Validating chunk and records',
  'extracting-summaries': 'Extracting compact trend summaries',
  complete: 'Graph data ready',
  cancelled: 'Graph loading cancelled',
}

export function GraphLoadingProgress({
  progress,
  onCancel,
}: {
  progress: GraphLoadProgress
  onCancel: () => void
}) {
  return (
    <section className="export-progress graph-progress" aria-live="polite" aria-busy="true">
      <div className="export-progress-heading">
        <div>
          <p className="eyebrow">Validated graph pipeline</p>
          <h3>{PHASE_LABELS[progress.phase]}</h3>
        </div>
        <strong>{progress.percentage.toFixed(1)}%</strong>
      </div>
      <progress max="100" value={progress.percentage}>
        {progress.percentage.toFixed(1)}%
      </progress>
      <div className="export-progress-detail">
        <span>Chunk {progress.currentChunk} / {progress.totalChunks}</span>
        <span>
          {progress.recordsValidated.toLocaleString()} /{' '}
          {progress.retainedRecords.toLocaleString()} records validated
        </span>
      </div>
      <button
        type="button"
        className="danger-button"
        onClick={onCancel}
        disabled={progress.cancellationRequested}
      >
        {progress.cancellationRequested ? 'Cancellation requested…' : 'Cancel loading'}
      </button>
    </section>
  )
}
