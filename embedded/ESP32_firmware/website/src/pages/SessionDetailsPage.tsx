import { useEffect, useRef, useState } from 'react'
import { ExportIntegrity, ExportProgress } from '../components/ExportProgress'
import {
  HistoryStatePanel,
} from '../components/HistoryStatePanel'
import { AppLink } from '../components/AppLink'
import { SessionWarnings } from '../components/SessionWarnings'
import {
  ExportCancelledError,
  exportSessionToCsv,
} from '../history/exportSession'
import type {
  ExportIntegrityReport,
  ExportProgress as ExportProgressState,
  HistoryRepository,
  SessionManifest,
} from '../history/types'
import { HISTORY_DEVICE_ID } from '../history/types'
import {
  formatIsoTimestamp,
  formatLocalTimestamp,
  isValidSessionKey,
  parseSessionManifest,
} from '../history/validation'
import { historyErrorMessage } from '../history/historyError'
import {
  SESSION_DETAIL_ACTIONS,
  sessionGraphsPath,
} from '../history/sessionActions'
import { subscribeToSessionInvalidation } from '../history/sessionInvalidation'

function formatBytes(bytes: number): string {
  if (bytes >= 1_073_741_824) return `${(bytes / 1_073_741_824).toFixed(1)} GiB`
  if (bytes >= 1_048_576) return `${(bytes / 1_048_576).toFixed(1)} MiB`
  return `${(bytes / 1_024).toFixed(1)} KiB`
}

function triggerDownload(blob: Blob, fileName: string): void {
  const url = URL.createObjectURL(blob)
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = fileName
  anchor.style.display = 'none'
  document.body.append(anchor)
  anchor.click()
  anchor.remove()
  window.setTimeout(() => URL.revokeObjectURL(url), 1_000)
}

export function SessionDetailsPage({
  repository,
  sessionKey,
  onDeleted,
}: {
  repository: HistoryRepository
  sessionKey: string
  onDeleted?: (sessionKey: string) => void
}) {
  const [manifest, setManifest] = useState<SessionManifest | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [isExporting, setIsExporting] = useState(false)
  const [progress, setProgress] = useState<ExportProgressState | null>(null)
  const [exportError, setExportError] = useState<string | null>(null)
  const [exportNotice, setExportNotice] = useState<string | null>(null)
  const [integrity, setIntegrity] = useState<ExportIntegrityReport | null>(null)
  const [confirmWaveforms, setConfirmWaveforms] = useState(false)
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [isDeleting, setIsDeleting] = useState(false)
  const [deleteError, setDeleteError] = useState<string | null>(null)
  const abortController = useRef<AbortController | null>(null)

  useEffect(() => {
    let active = true
    setManifest(null)
    setLoadError(null)
    if (!isValidSessionKey(sessionKey)) {
      setLoadError('The session URL does not contain a valid session key.')
      return () => {
        active = false
      }
    }
    repository.getManifest(sessionKey).then(
      (value) => {
        if (!active) return
        try {
          setManifest(parseSessionManifest(sessionKey, value))
        } catch (error) {
          setLoadError(historyErrorMessage(error))
        }
      },
      (error) => {
        if (active) setLoadError(historyErrorMessage(error))
      },
    )
    return () => {
      active = false
    }
  }, [repository, sessionKey])

  useEffect(() => () => abortController.current?.abort(), [])

  useEffect(() => subscribeToSessionInvalidation(sessionKey, () => {
    abortController.current?.abort()
    abortController.current = null
    setManifest(null)
    setProgress(null)
    setIntegrity(null)
    setExportNotice(null)
    setConfirmWaveforms(false)
    setConfirmDelete(false)
    setLoadError('This uploaded session no longer exists in cloud history.')
  }), [sessionKey])

  async function startExport() {
    if (manifest === null || isExporting) return
    const controller = new AbortController()
    abortController.current = controller
    setIsExporting(true)
    setProgress(null)
    setExportError(null)
    setExportNotice(null)
    setIntegrity(null)
    setConfirmWaveforms(false)

    try {
      const result = await exportSessionToCsv({
        repository,
        manifest,
        signal: controller.signal,
        onProgress: setProgress,
      })
      if (!controller.signal.aborted) {
        triggerDownload(result.blob, result.fileName)
        setIntegrity(result.report)
        setExportNotice('The validated CSV download has started.')
      }
    } catch (error) {
      if (error instanceof ExportCancelledError) {
        setExportNotice('Export cancelled. No partial file was downloaded.')
      } else {
        setExportError(historyErrorMessage(error))
      }
    } finally {
      if (abortController.current === controller) {
        abortController.current = null
      }
      setIsExporting(false)
    }
  }

  function cancelExport() {
    abortController.current?.abort()
    setProgress((current) => current === null
      ? current
      : { ...current, cancellationRequested: true })
  }

  async function deleteUploadedSession(): Promise<void> {
    if (manifest === null || isDeleting) return
    if (repository.deleteSession === undefined) {
      setDeleteError('Session deletion is unavailable in this data mode.')
      return
    }
    setIsDeleting(true)
    setDeleteError(null)
    abortController.current?.abort()
    try {
      await repository.deleteSession(HISTORY_DEVICE_ID, sessionKey)
      setProgress(null)
      setIntegrity(null)
      setExportNotice(null)
      setExportError(null)
      setIsDeleting(false)
      onDeleted?.(sessionKey)
    } catch (error) {
      setDeleteError(historyErrorMessage(error))
      setIsDeleting(false)
    }
  }

  return (
    <main className="dashboard-main history-main">
      <nav className="breadcrumb" aria-label="Breadcrumb">
        <AppLink href="/history">History</AppLink>
        <span aria-hidden="true">/</span>
        <span>{sessionKey}</span>
      </nav>

      {manifest === null && loadError === null && (
        <HistoryStatePanel
          title="Validating session manifest"
          detail="Only the selected manifest is being fetched. Chunks remain untouched."
        />
      )}

      {loadError !== null && (
        <HistoryStatePanel
          title="Session cannot be opened"
          detail={loadError}
          tone="error"
          action={(
            <AppLink className="state-link" href="/history">
              Return to completed sessions
            </AppLink>
          )}
        />
      )}

      {manifest !== null && (
        <>
          <section className="detail-hero" aria-labelledby="session-title">
            <div>
              <p className="eyebrow">Historical session</p>
              <h2 id="session-title">Session {manifest.sessionId.toString()}</h2>
              <p>
                Uploaded {formatLocalTimestamp(manifest.uploadCompletedAt)}.
                Graphs and waveform export begin only after you choose an action.
              </p>
            </div>
            <div className="badge-row">
              <span className="badge badge-complete">Complete</span>
              {repository.mode === 'mock' && (
                <span className="badge badge-mock">Mock data</span>
              )}
            </div>
          </section>

          <SessionWarnings manifest={manifest} />

          <section className="session-detail-panel" aria-labelledby="summary-title">
            <div className="section-heading">
              <div>
                <p className="eyebrow">Verified manifest</p>
                <h3 id="summary-title">Session summary</h3>
              </div>
              <span className="format-chip">{manifest.recordFormat} · {manifest.recordSize} bytes</span>
            </div>
            <dl className="detail-grid">
              <div><dt>Persistent state</dt><dd>{manifest.persistentState}</dd></div>
              <div><dt>Retained records</dt><dd>{manifest.retainedCount.toLocaleString()}</dd></div>
              <div><dt>Total stored</dt><dd>{manifest.totalStored.toString()}</dd></div>
              <div><dt>Overwritten</dt><dd>{manifest.overwrittenCount.toString()}</dd></div>
              <div><dt>Logical range</dt><dd>{manifest.firstLogicalIndex.toString()}–{manifest.lastLogicalIndex.toString()}</dd></div>
              <div><dt>STM32 sequence range</dt><dd>{manifest.firstStm32Sequence.toLocaleString()}–{manifest.lastStm32Sequence.toLocaleString()}</dd></div>
              <div><dt>Chunks</dt><dd>{manifest.chunkCount.toLocaleString()}</dd></div>
              <div><dt>Records per chunk</dt><dd>{manifest.recordsPerChunk}</dd></div>
              <div><dt>Source metadata generation</dt><dd>{manifest.sourceMetadataGeneration.toString()}</dd></div>
              <div><dt>Upload started</dt><dd title={formatIsoTimestamp(manifest.uploadStartedAt)}>{formatLocalTimestamp(manifest.uploadStartedAt)}</dd></div>
              <div><dt>Upload completed</dt><dd title={formatIsoTimestamp(manifest.uploadCompletedAt)}>{formatLocalTimestamp(manifest.uploadCompletedAt)}</dd></div>
              <div><dt>Manifest CRC32C</dt><dd>{manifest.manifestCrc32c}</dd></div>
            </dl>
            <p className="timestamp-note">
              Upload timestamps describe the cloud transfer. Per-record capture
              timestamps are ESP32 uptime microseconds and are not treated as wall-clock time.
            </p>
          </section>

          <section className="export-panel" aria-labelledby="export-title">
            <div className="section-heading">
              <div>
                <p className="eyebrow">On-demand historical analysis</p>
                <h3 id="export-title">View or export validated data</h3>
              </div>
            </div>
            <div className="export-options">
              <article>
                <h4>Historical graphs</h4>
                <p>
                  Explore session-wide scalar trends and the selected packet's
                  voltage and current harmonic spectra.
                </p>
                <strong>Data loads only after the graph page opens</strong>
                <AppLink
                  className="primary-button action-link"
                  href={sessionGraphsPath(sessionKey)}
                >
                  {SESSION_DETAIL_ACTIONS.graphs}
                </AppLink>
              </article>
              <article>
                <h4>Full waveforms CSV</h4>
                <p>
                  Long-format voltage and current samples. This may generate a
                  very large file and can take several minutes.
                </p>
                <strong>
                  {(manifest.retainedCount * 1_024).toLocaleString()} rows · approximately{' '}
                  {formatBytes(manifest.retainedCount * 1_024 * 90)}
                </strong>
                <button
                  className="secondary-button"
                  type="button"
                  disabled={isExporting}
                  onClick={() => setConfirmWaveforms(true)}
                >
                  {SESSION_DETAIL_ACTIONS.waveforms}
                </button>
              </article>
            </div>

            {confirmWaveforms && !isExporting && (
              <div className="waveform-confirmation" role="alertdialog" aria-labelledby="waveform-confirm-title">
                <h4 id="waveform-confirm-title">Start a potentially large export?</h4>
                <p>
                  The browser will validate every chunk and generate{' '}
                  {(manifest.retainedCount * 1_024).toLocaleString()} waveform rows.
                  Keep this page open until the download starts.
                </p>
                <div className="confirmation-actions">
                  <button
                    className="primary-button"
                    type="button"
                    onClick={() => void startExport()}
                  >
                    Confirm waveform export
                  </button>
                  <button
                    className="secondary-button"
                    type="button"
                    onClick={() => setConfirmWaveforms(false)}
                  >
                    Keep browsing
                  </button>
                </div>
              </div>
            )}

            {isExporting && progress !== null && (
              <ExportProgress progress={progress} onCancel={cancelExport} />
            )}
            {exportError !== null && (
              <div className="message message-error export-message" role="alert">
                <span className="message-symbol" aria-hidden="true">!</span>
                <span>{exportError}. No partial file was downloaded.</span>
              </div>
            )}
            {exportNotice !== null && (
              <p className="export-notice" aria-live="polite">{exportNotice}</p>
            )}
          </section>

          {integrity !== null && <ExportIntegrity report={integrity} />}

          <section className="danger-zone" aria-labelledby="delete-session-title">
            <div>
              <p className="eyebrow">Cloud history management</p>
              <h3 id="delete-session-title">Delete uploaded session</h3>
              <p>
                Permanently remove this session's manifest, index entry, and
                uploaded chunks from Firebase. Physical-device data is not erased.
              </p>
            </div>
            <button
              className="danger-button"
              type="button"
              disabled={isDeleting}
              onClick={() => {
                setDeleteError(null)
                setConfirmDelete(true)
              }}
            >
              Delete Session
            </button>

            {confirmDelete && (
              <div
                className="delete-confirmation"
                role="alertdialog"
                aria-labelledby="delete-confirm-title"
                aria-describedby="delete-confirm-description"
              >
                <h4 id="delete-confirm-title">Delete this uploaded session permanently?</h4>
                <p id="delete-confirm-description">
                  This removes the cloud copy only. Data retained on the physical
                  device is not erased.
                </p>
                <dl className="delete-session-identity">
                  <div><dt>Session ID</dt><dd>{manifest.sessionId.toString()}</dd></div>
                  <div>
                    <dt>Session start</dt>
                    <dd>{manifest.sessionTimeValid && manifest.sessionStartEpochMs !== null
                      ? formatLocalTimestamp(Number(manifest.sessionStartEpochMs))
                      : 'Unavailable'}</dd>
                  </div>
                  <div>
                    <dt>Session end</dt>
                    <dd>{manifest.sessionEndTimeValid && manifest.sessionEndEpochMs !== null
                      ? formatLocalTimestamp(Number(manifest.sessionEndEpochMs))
                      : 'Unavailable'}</dd>
                  </div>
                  <div><dt>Retained records</dt><dd>{manifest.retainedCount.toLocaleString()}</dd></div>
                </dl>
                <div className="confirmation-actions">
                  <button
                    className="danger-button"
                    type="button"
                    disabled={isDeleting}
                    onClick={() => void deleteUploadedSession()}
                  >
                    {isDeleting ? 'Deleting…' : 'Delete session'}
                  </button>
                  <button
                    className="secondary-button"
                    type="button"
                    disabled={isDeleting}
                    onClick={() => setConfirmDelete(false)}
                  >
                    Cancel
                  </button>
                </div>
              </div>
            )}

            {deleteError !== null && (
              <div className="message message-error" role="alert">
                <span className="message-symbol" aria-hidden="true">!</span>
                <span>{deleteError}</span>
              </div>
            )}
          </section>
        </>
      )}
    </main>
  )
}
