import { FirebaseError } from 'firebase/app'
import {
  equalTo,
  get,
  orderByChild,
  query,
  ref,
  update,
  type Database,
  type Query,
} from 'firebase/database'
import {
  HISTORY_DEVICE_ID,
  type HistoryListResult,
  type HistoryRepository,
} from '../history/types'
import {
  formatChunkKey,
  isValidChunkKey,
  isValidSessionKey,
  parseHistoryIndex,
  HistoryValidationError,
} from '../history/validation'
import { invalidateSession } from '../history/sessionInvalidation'

const SESSION_INDEX_PATH = `devices/${HISTORY_DEVICE_ID}/sessionIndex`
const SESSION_DATA_PATH = `devices/${HISTORY_DEVICE_ID}/sessionData`

export interface SessionDeletionPaths {
  index: string
  data: string
}

export const COMPLETED_HISTORY_QUERY = Object.freeze({
  orderByChild: 'state',
  equalTo: 'complete',
})

export type HistoryRepositoryErrorKind =
  | 'authentication'
  | 'permission-denied'
  | 'network'
  | 'not-found'
  | 'malformed-data'
  | 'configuration'

export class HistoryRepositoryError extends Error {
  readonly kind: HistoryRepositoryErrorKind

  constructor(kind: HistoryRepositoryErrorKind, message: string) {
    super(message)
    this.name = 'HistoryRepositoryError'
    this.kind = kind
  }
}

function toRepositoryError(
  error: unknown,
  operation: 'read' | 'delete' = 'read',
): HistoryRepositoryError {
  if (error instanceof HistoryRepositoryError) return error
  if (error instanceof FirebaseError) {
    const code = error.code.toLowerCase()
    if (
      code.includes('permission-denied') ||
      code.includes('permission_denied')
    ) {
      return new HistoryRepositoryError(
        'permission-denied',
        operation === 'delete'
          ? 'This account is not authorized to delete historical sessions.'
          : 'This account cannot read historical sessions.',
      )
    }
    if (code.startsWith('auth/')) {
      return new HistoryRepositoryError(
        'authentication',
        'The authenticated Firebase session is no longer valid.',
      )
    }
    if (
      code.includes('network') ||
      code.includes('disconnected') ||
      code.includes('unavailable')
    ) {
      return new HistoryRepositoryError(
        'network',
        'Historical data could not be reached over the network.',
      )
    }
  }
  return new HistoryRepositoryError(
    'network',
    operation === 'delete'
      ? 'The uploaded session could not be deleted from Firebase.'
      : 'Historical data could not be read from Firebase.',
  )
}

export function buildCompletedSessionIndexQuery(database: Database): Query {
  return query(
    ref(database, SESSION_INDEX_PATH),
    orderByChild(COMPLETED_HISTORY_QUERY.orderByChild),
    equalTo(COMPLETED_HISTORY_QUERY.equalTo),
  )
}

export function buildSessionDeletionPaths(
  deviceId: string,
  sessionKey: string,
): SessionDeletionPaths {
  if (deviceId !== HISTORY_DEVICE_ID) {
    throw new HistoryRepositoryError(
      'malformed-data',
      'The device identifier is invalid.',
    )
  }
  if (!isValidSessionKey(sessionKey)) {
    throw new HistoryRepositoryError(
      'malformed-data',
      'The session key is invalid.',
    )
  }
  return {
    index: `sessionIndex/${sessionKey}`,
    data: `sessionData/${sessionKey}`,
  }
}

export class FirebaseHistoryRepository implements HistoryRepository {
  readonly mode = 'firebase' as const
  private readonly database: Database | null

  constructor(database: Database | null) {
    this.database = database
  }

  private requireDatabase(): Database {
    if (this.database === null) {
      throw new HistoryRepositoryError(
        'configuration',
        'Firebase Realtime Database is not configured.',
      )
    }
    return this.database
  }

  async listCompletedSessions(): Promise<HistoryListResult> {
    try {
      const snapshot = await get(
        buildCompletedSessionIndexQuery(this.requireDatabase()),
      )
      return parseHistoryIndex(snapshot.exists() ? snapshot.val() : null)
    } catch (error) {
      if (error instanceof HistoryRepositoryError) throw error
      if (error instanceof HistoryValidationError) {
        throw new HistoryRepositoryError('malformed-data', error.message)
      }
      throw toRepositoryError(error)
    }
  }

  async getManifest(sessionKey: string): Promise<unknown> {
    if (!isValidSessionKey(sessionKey)) {
      throw new HistoryRepositoryError(
        'malformed-data',
        'The session key is invalid.',
      )
    }
    try {
      const snapshot = await get(
        ref(
          this.requireDatabase(),
          `${SESSION_DATA_PATH}/${sessionKey}/manifest`,
        ),
      )
      if (!snapshot.exists()) {
        throw new HistoryRepositoryError(
          'not-found',
          'The session manifest does not exist.',
        )
      }
      return snapshot.val()
    } catch (error) {
      throw toRepositoryError(error)
    }
  }

  async getChunk(sessionKey: string, chunkKey: string): Promise<unknown> {
    if (
      !isValidSessionKey(sessionKey) ||
      !isValidChunkKey(chunkKey) ||
      chunkKey !== formatChunkKey(Number(chunkKey))
    ) {
      throw new HistoryRepositoryError(
        'malformed-data',
        'The session or chunk key is invalid.',
      )
    }
    try {
      const snapshot = await get(
        ref(
          this.requireDatabase(),
          `${SESSION_DATA_PATH}/${sessionKey}/chunks/${chunkKey}`,
        ),
      )
      if (!snapshot.exists()) {
        throw new HistoryRepositoryError(
          'not-found',
          `Chunk ${chunkKey} does not exist.`,
        )
      }
      return snapshot.val()
    } catch (error) {
      throw toRepositoryError(error)
    }
  }

  async deleteSession(deviceId: string, sessionKey: string): Promise<void> {
    const paths = buildSessionDeletionPaths(deviceId, sessionKey)
    try {
      await update(ref(this.requireDatabase(), `devices/${deviceId}`), {
        [paths.index]: null,
        [paths.data]: null,
      })
      invalidateSession(sessionKey)
    } catch (error) {
      throw toRepositoryError(error, 'delete')
    }
  }
}
