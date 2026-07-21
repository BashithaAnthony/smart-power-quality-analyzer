import { describe, expect, it } from 'vitest'
import { parseRoute } from './routing'

describe('history graph routing', () => {
  it('parses the authenticated session graph route', () => {
    expect(parseRoute('/history/s_42/graphs')).toEqual({
      name: 'history-graphs',
      sessionKey: 's_42',
    })
  })

  it('passes an invalid session key to the graph page for strict validation', () => {
    expect(parseRoute('/history/not-a-session/graphs')).toEqual({
      name: 'history-graphs',
      sessionKey: 'not-a-session',
    })
  })

  it('preserves the existing detail route', () => {
    expect(parseRoute('/history/s_42')).toEqual({
      name: 'history-detail',
      sessionKey: 's_42',
    })
  })
})
