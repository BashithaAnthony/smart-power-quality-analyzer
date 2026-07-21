import { describe, expect, it } from 'vitest'
import {
  buildSessionDeletionPaths,
  COMPLETED_HISTORY_QUERY,
} from './historyRepository'

describe('Firebase history repository policy', () => {
  it('uses the required completed-state sessionIndex query', () => {
    expect(COMPLETED_HISTORY_QUERY).toEqual({
      orderByChild: 'state',
      equalTo: 'complete',
    })
  })

  it('builds only the selected index and data deletion paths', () => {
    expect(buildSessionDeletionPaths('PQ-3PH-001', 's_42')).toEqual({
      index: 'sessionIndex/s_42',
      data: 'sessionData/s_42',
    })
  })

  it.each([
    ['', 's_42'],
    ['PQ-3PH-OTHER', 's_42'],
    ['PQ-3PH-001', ''],
    ['PQ-3PH-001', '../s_42'],
    ['PQ-3PH-001', 's_42/chunks'],
  ])('rejects unsafe deletion identifiers', (deviceId, sessionKey) => {
    expect(() => buildSessionDeletionPaths(deviceId, sessionKey)).toThrow()
  })
})
