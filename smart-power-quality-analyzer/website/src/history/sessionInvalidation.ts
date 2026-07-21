type SessionInvalidationListener = () => void

const listeners = new Map<string, Set<SessionInvalidationListener>>()

export function subscribeToSessionInvalidation(
  sessionKey: string,
  listener: SessionInvalidationListener,
): () => void {
  const sessionListeners = listeners.get(sessionKey) ?? new Set()
  sessionListeners.add(listener)
  listeners.set(sessionKey, sessionListeners)
  return () => {
    sessionListeners.delete(listener)
    if (sessionListeners.size === 0) listeners.delete(sessionKey)
  }
}

export function invalidateSession(sessionKey: string): void {
  const sessionListeners = listeners.get(sessionKey)
  if (sessionListeners === undefined) return
  for (const listener of [...sessionListeners]) listener()
}
