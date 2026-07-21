import type { Database } from 'firebase/database'
import {
  FirebaseHistoryRepository,
} from '../lib/historyRepository'
import { MockHistoryRepository } from './mockHistory'
import type { HistoryRepository } from './types'

export type HistoryDataMode = 'firebase' | 'mock'

export function configuredHistoryDataMode(): HistoryDataMode {
  return import.meta.env.VITE_HISTORY_DATA_MODE === 'mock'
    ? 'mock'
    : 'firebase'
}

export function createHistoryRepository(
  database: Database | null,
  mode: HistoryDataMode = configuredHistoryDataMode(),
): HistoryRepository {
  return mode === 'mock'
    ? new MockHistoryRepository()
    : new FirebaseHistoryRepository(database)
}
