import type { SessionManifest } from '../history/types'

export function SessionWarnings({ manifest }: { manifest: SessionManifest }) {
  if (
    !manifest.truncated &&
    !manifest.recoveredIncomplete &&
    !manifest.countersPartial
  ) {
    return null
  }
  return (
    <section className="session-warnings" aria-label="Session integrity warnings">
      {manifest.truncated && (
        <div>
          <strong>Beginning overwritten</strong>
          <span>The circular logger retained only the newest records.</span>
        </div>
      )}
      {manifest.recoveredIncomplete && (
        <div>
          <strong>Recovered incomplete session</strong>
          <span>The session was reconstructed after an interrupted run.</span>
        </div>
      )}
      {manifest.countersPartial && (
        <div>
          <strong>Partial counters</strong>
          <span>Some volatile counters could not be reconstructed.</span>
        </div>
      )}
    </section>
  )
}
