export const SESSION_DETAIL_ACTIONS = Object.freeze({
  graphs: 'View Data as Graphs',
  waveforms: 'Download Full Waveforms CSV',
})

export function sessionGraphsPath(sessionKey: string): string {
  return `/history/${encodeURIComponent(sessionKey)}/graphs`
}
