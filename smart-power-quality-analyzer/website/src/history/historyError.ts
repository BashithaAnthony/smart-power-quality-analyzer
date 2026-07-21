import { HistoryRepositoryError } from '../lib/historyRepository'

export function historyErrorMessage(error: unknown): string {
  if (error instanceof HistoryRepositoryError) return error.message
  if (error instanceof Error) return error.message
  return 'Historical data could not be loaded.'
}
