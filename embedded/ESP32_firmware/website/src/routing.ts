import { useEffect, useState } from 'react'

const NAVIGATION_EVENT = 'power-quality:navigation'

export type AppRoute =
  | { name: 'live' }
  | { name: 'history' }
  | { name: 'history-detail'; sessionKey: string }
  | { name: 'history-graphs'; sessionKey: string }
  | { name: 'not-found' }

export function parseRoute(pathname: string): AppRoute {
  const normalized = pathname.length > 1
    ? pathname.replace(/\/+$/, '')
    : pathname
  if (normalized === '/') return { name: 'live' }
  if (normalized === '/history') return { name: 'history' }
  const graphMatch = /^\/history\/([^/]+)\/graphs$/.exec(normalized)
  if (graphMatch !== null) {
    try {
      return {
        name: 'history-graphs',
        sessionKey: decodeURIComponent(graphMatch[1]),
      }
    } catch {
      return { name: 'not-found' }
    }
  }
  const match = /^\/history\/([^/]+)$/.exec(normalized)
  if (match !== null) {
    try {
      return {
        name: 'history-detail',
        sessionKey: decodeURIComponent(match[1]),
      }
    } catch {
      return { name: 'not-found' }
    }
  }
  return { name: 'not-found' }
}

export function navigateTo(href: string): void {
  if (`${window.location.pathname}${window.location.search}` === href) return
  window.history.pushState(null, '', href)
  window.dispatchEvent(new Event(NAVIGATION_EVENT))
  window.scrollTo({ top: 0, behavior: 'auto' })
}

export function useAppRoute(): AppRoute {
  const [route, setRoute] = useState(() => parseRoute(window.location.pathname))

  useEffect(() => {
    const update = () => setRoute(parseRoute(window.location.pathname))
    window.addEventListener('popstate', update)
    window.addEventListener(NAVIGATION_EVENT, update)
    return () => {
      window.removeEventListener('popstate', update)
      window.removeEventListener(NAVIGATION_EVENT, update)
    }
  }, [])

  return route
}
