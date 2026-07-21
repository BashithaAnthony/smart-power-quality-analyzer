import { describe, expect, it } from 'vitest'
import {
  SESSION_DETAIL_ACTIONS,
  sessionGraphsPath,
} from '../history/sessionActions'

describe('session detail actions', () => {
  it('removes the measurements CSV action', () => {
    expect(Object.values(SESSION_DETAIL_ACTIONS)).not.toContain(
      'Download Measurements CSV',
    )
  })

  it('preserves waveform export and adds graph navigation', () => {
    expect(SESSION_DETAIL_ACTIONS.waveforms).toBe('Download Full Waveforms CSV')
    expect(SESSION_DETAIL_ACTIONS.graphs).toBe('View Data as Graphs')
    expect(sessionGraphsPath('s_42')).toBe('/history/s_42/graphs')
  })
})
