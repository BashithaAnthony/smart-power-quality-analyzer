export function HistoryStatePanel({
  title,
  detail,
  tone = 'neutral',
  action,
}: {
  title: string
  detail: string
  tone?: 'neutral' | 'error'
  action?: React.ReactNode
}) {
  return (
    <section
      className={`state-panel history-state-panel ${tone === 'error' ? 'state-connection-error' : ''}`}
      aria-live="polite"
    >
      <span className="state-symbol" aria-hidden="true">
        {tone === 'error' ? '!' : '—'}
      </span>
      <div>
        <h2>{title}</h2>
        <p>{detail}</p>
        {action}
      </div>
    </section>
  )
}
