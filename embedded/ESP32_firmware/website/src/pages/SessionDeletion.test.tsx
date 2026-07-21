/** @vitest-environment jsdom */

import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  createDeterministicMockHistory,
  MockHistoryRepository,
} from '../history/mockHistory'
import type { HistoryRepository } from '../history/types'
import { HistoryListPage } from './HistoryListPage'
import { SessionDetailsPage } from './SessionDetailsPage'

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
})

function deferredDeleteRepository(): {
  repository: HistoryRepository
  resolve: () => void
  reject: (error: Error) => void
  deleteSession: ReturnType<typeof vi.fn>
} {
  const fixture = createDeterministicMockHistory()
  let resolvePromise: () => void = () => undefined
  let rejectPromise: (error: Error) => void = () => undefined
  const deletion = new Promise<void>((resolve, reject) => {
    resolvePromise = resolve
    rejectPromise = reject
  })
  const deleteSession = vi.fn(() => deletion)
  return {
    repository: {
      mode: 'mock',
      listCompletedSessions: () => Promise.resolve(fixture.list),
      getManifest: () => Promise.resolve(fixture.manifest),
      getChunk: (_sessionKey, chunkKey) => Promise.resolve(
        fixture.chunks.get(chunkKey) ?? null,
      ),
      deleteSession,
    },
    resolve: resolvePromise,
    reject: rejectPromise,
    deleteSession,
  }
}

describe('historical session cloud deletion', () => {
  it('shows a destructive action and requires an identifying confirmation', async () => {
    const repository = new MockHistoryRepository()
    const deleteSession = vi.spyOn(repository, 'deleteSession')
    render(<SessionDetailsPage repository={repository} sessionKey="s_42" />)
    await screen.findByRole('heading', { name: 'Session 42' })

    fireEvent.click(screen.getByRole('button', { name: 'Delete Session' }))
    expect(screen.getByRole('heading', {
      name: 'Delete this uploaded session permanently?',
    })).toBeTruthy()
    expect(screen.getByText(/cloud copy only/i)).toBeTruthy()
    expect(screen.getByText(/physical device is not erased/i)).toBeTruthy()
    expect(screen.getByRole('alertdialog').textContent).toContain('Retained records3')
    expect(deleteSession).not.toHaveBeenCalled()

    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }))
    expect(screen.queryByRole('button', { name: 'Delete session' })).toBeNull()
    expect(deleteSession).not.toHaveBeenCalled()
  })

  it('waits for confirmation, issues one deletion while pending, and reports success', async () => {
    const pending = deferredDeleteRepository()
    const onDeleted = vi.fn()
    render(
      <SessionDetailsPage
        repository={pending.repository}
        sessionKey="s_42"
        onDeleted={onDeleted}
      />,
    )
    await screen.findByRole('heading', { name: 'Session 42' })
    fireEvent.click(screen.getByRole('button', { name: 'Delete Session' }))
    const confirm = screen.getByRole('button', { name: 'Delete session' })
    fireEvent.click(confirm)
    fireEvent.click(confirm)
    expect(pending.deleteSession).toHaveBeenCalledTimes(1)
    expect(pending.deleteSession).toHaveBeenCalledWith('PQ-3PH-001', 's_42')
    expect((screen.getByRole('button', { name: 'Deleting…' }) as HTMLButtonElement).disabled).toBe(true)

    pending.resolve()
    await waitFor(() => expect(onDeleted).toHaveBeenCalledWith('s_42'))
  })

  it('keeps the session visible and reports a Firebase deletion failure', async () => {
    const pending = deferredDeleteRepository()
    render(<SessionDetailsPage repository={pending.repository} sessionKey="s_42" />)
    await screen.findByRole('heading', { name: 'Session 42' })
    fireEvent.click(screen.getByRole('button', { name: 'Delete Session' }))
    fireEvent.click(screen.getByRole('button', { name: 'Delete session' }))
    pending.reject(new Error('simulated Firebase failure'))

    await screen.findByRole('alert')
    expect(screen.getByRole('heading', { name: 'Session 42' })).toBeTruthy()
    expect((screen.getByRole('button', { name: 'Delete session' }) as HTMLButtonElement).disabled).toBe(false)
  })

  it('refreshes to an empty list with a cloud-deletion success notice', async () => {
    const repository = new MockHistoryRepository()
    await repository.deleteSession('PQ-3PH-001', 's_42')
    render(
      <HistoryListPage
        repository={repository}
        successNotice="Session 42 was deleted from cloud history."
      />,
    )
    expect((await screen.findByRole('status')).textContent).toContain(
      'Session 42 was deleted from cloud history.',
    )
    expect(await screen.findByText('No completed sessions')).toBeTruthy()
  })

  it('shows a usable not-found state for a directly opened deleted URL', async () => {
    const repository = new MockHistoryRepository()
    await repository.deleteSession('PQ-3PH-001', 's_42')
    render(<SessionDetailsPage repository={repository} sessionKey="s_42" />)
    expect(await screen.findByText('Session cannot be opened')).toBeTruthy()
    expect(screen.getByText('Return to completed sessions')).toBeTruthy()
  })
})
