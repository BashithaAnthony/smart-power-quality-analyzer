import { useEffect, useState } from 'react'
import type {
  HistoryListResult,
  HistoryRepository,
} from '../history/types'
import { formatLocalTimestamp } from '../history/validation'
import { AppLink } from '../components/AppLink'
import { HistoryStatePanel } from '../components/HistoryStatePanel'
import { historyErrorMessage } from '../history/historyError'

function SessionBadges({
  truncated,
  recovered,
  partial,
}: {
  truncated: boolean
  recovered: boolean
  partial: boolean
}) {
  return (
    <div className="badge-row" aria-label="Session status">
      <span className="badge badge-complete">Complete</span>
      {truncated && <span className="badge badge-warning">Truncated</span>}
      {recovered && <span className="badge badge-warning">Recovered</span>}
      {partial && <span className="badge badge-neutral">Partial counters</span>}
    </div>
  )
}

export function HistoryListPage({
  repository,
  successNotice,
}: {
  repository: HistoryRepository
  successNotice?: string | null
}) {
  const [result, setResult] = useState<HistoryListResult | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [reloadToken, setReloadToken] = useState(0)

  useEffect(() => {
    let active = true
    setResult(null)
    setError(null)
    repository.listCompletedSessions().then(
      (history) => {
        if (active) setResult(history)
      },
      (loadError) => {
        if (active) setError(historyErrorMessage(loadError))
      },
    )
    return () => {
      active = false
    }
  }, [repository, reloadToken])

  return (
    <main className="dashboard-main history-main">
      <section className="history-hero" aria-labelledby="history-title">
        <div>
          <p className="eyebrow">Validated retained sessions</p>
          <h2 id="history-title">Historical session archive</h2>
          <p>
            Completed uploads are listed without downloading manifests,
            waveform packets, or chunk payloads.
          </p>
        </div>
        {repository.mode === 'mock' && (
          <span className="badge badge-mock">Mock historical data</span>
        )}
      </section>

      {successNotice !== null && successNotice !== undefined && (
        <div className="message message-success history-success" role="status">
          <span className="message-symbol" aria-hidden="true">✓</span>
          <span>{successNotice}</span>
        </div>
      )}

      {result === null && error === null && (
        <HistoryStatePanel
          title="Loading completed sessions"
          detail="Running the authenticated completed-state index query."
        />
      )}

      {error !== null && (
        <HistoryStatePanel
          title="History unavailable"
          detail={error}
          tone="error"
          action={(
            <button
              className="secondary-button state-action"
              type="button"
              onClick={() => setReloadToken((value) => value + 1)}
            >
              Retry history query
            </button>
          )}
        />
      )}

      {result !== null && result.sessions.length === 0 && (
        <HistoryStatePanel
          title="No completed sessions"
          detail="The authenticated completed-state index query returned no sessions."
        />
      )}

      {result !== null && result.sessions.length > 0 && (
        <section className="history-list" aria-label="Completed sessions">
          {result.sessions.map((session) => (
            <article className="session-card" key={session.sessionKey}>
              <div className="session-card-heading">
                <div>
                  <p className="eyebrow">Session {session.sessionId.toString()}</p>
                  <h3>{formatLocalTimestamp(session.uploadCompletedAt)}</h3>
                </div>
                <SessionBadges
                  truncated={session.truncated}
                  recovered={session.recoveredIncomplete}
                  partial={session.countersPartial}
                />
              </div>
              <dl className="session-card-grid">
                <div><dt>Persistent state</dt><dd>{session.persistentState}</dd></div>
                <div><dt>Retained</dt><dd>{session.retainedCount.toLocaleString()}</dd></div>
                <div><dt>Total stored</dt><dd>{session.totalStored.toString()}</dd></div>
                <div><dt>Overwritten</dt><dd>{session.overwrittenCount.toString()}</dd></div>
                <div><dt>Logical range</dt><dd>{session.firstLogicalIndex.toString()}–{session.lastLogicalIndex.toString()}</dd></div>
                <div><dt>Chunks</dt><dd>{session.chunkCount.toLocaleString()}</dd></div>
              </dl>
              <AppLink
                className="session-open-link"
                href={`/history/${encodeURIComponent(session.sessionKey)}`}
              >
                Open session details <span aria-hidden="true">→</span>
              </AppLink>
            </article>
          ))}
        </section>
      )}

      {result !== null && result.malformedEntries.length > 0 && (
        <section className="malformed-panel" aria-labelledby="malformed-title">
          <h3 id="malformed-title">Malformed index entries skipped</h3>
          <p>
            {result.malformedEntries.length} untrusted index{' '}
            {result.malformedEntries.length === 1 ? 'entry was' : 'entries were'}
            {' '}excluded without affecting valid sessions.
          </p>
          <ul>
            {result.malformedEntries.map((entry) => (
              <li key={entry.sessionKey}>
                <code>{entry.sessionKey}</code>: {entry.reason}
              </li>
            ))}
          </ul>
        </section>
      )}
    </main>
  )
}
