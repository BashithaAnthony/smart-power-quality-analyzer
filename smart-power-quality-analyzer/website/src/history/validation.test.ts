import { describe, expect, it } from 'vitest'
import { decodeBase64 } from './base64'
import { crc32c, formatCrc32c } from './crc32c'
import {
  createDeterministicMockHistory,
} from './mockHistory'
import type { SessionManifest } from './types'
import {
  calculateManifestCrc32c,
  formatChunkKey,
  isValidSessionKey,
  parseHistoryIndex,
  parseSessionChunk,
  parseSessionManifest,
} from './validation'

describe('historical Firebase validation', () => {
  function anchoredManifestFixture(): {
    manifest: SessionManifest
    raw: Record<string, unknown>
  } {
    const legacyFixture = createDeterministicMockHistory()
    const legacy = parseSessionManifest('s_42', legacyFixture.manifest)
    const manifest: SessionManifest = {
      ...legacy,
      schemaVersion: 2,
      sessionTimeValid: true,
      sessionStartEpochMs: 1_720_000_000_000n,
      sessionStartCaptureTimestampUs: 5_000_000n,
      sessionEndTimeValid: true,
      sessionEndEpochMs: 1_720_000_000_300n,
      sessionBootId: 0x1234abcd,
      timeSource: 'ntp',
      manifestCrc32c: '00000000',
    }
    manifest.manifestCrc32c = calculateManifestCrc32c(manifest)
    return {
      manifest,
      raw: {
        ...legacyFixture.manifest,
        schemaVersion: 2,
        sessionTimeValid: true,
        sessionStartEpochMs: manifest.sessionStartEpochMs?.toString(),
        sessionStartCaptureTimestampUs:
          manifest.sessionStartCaptureTimestampUs?.toString(),
        sessionEndTimeValid: true,
        sessionEndEpochMs: manifest.sessionEndEpochMs?.toString(),
        sessionBootId: manifest.sessionBootId,
        timeSource: 'ntp',
        manifestCrc32c: manifest.manifestCrc32c,
      },
    }
  }

  it('validates session keys and deterministic chunk keys', () => {
    expect(isValidSessionKey('s_18446744073709551615')).toBe(true)
    expect(isValidSessionKey('s_-1')).toBe(false)
    expect(isValidSessionKey('session_1')).toBe(false)
    expect(formatChunkKey(0)).toBe('000000')
    expect(formatChunkKey(999_999)).toBe('999999')
    expect(() => formatChunkKey(1_000_000)).toThrow()
  })

  it('accepts valid index entries and isolates malformed entries', () => {
    const fixture = createDeterministicMockHistory()
    const parsed = parseHistoryIndex({
      s_42: fixture.manifest,
      invalid: { state: 'complete' },
    })
    expect(parsed.sessions).toHaveLength(1)
    expect(parsed.sessions[0].sessionId).toBe(42n)
    expect(parsed.malformedEntries).toHaveLength(1)
    expect(parsed.malformedEntries[0].sessionKey).toBe('invalid')
  })

  it('rejects malformed Firebase roots without unsafe fallback', () => {
    expect(() => parseHistoryIndex([])).toThrow(/must be an object/i)
    expect(() => parseHistoryIndex('complete')).toThrow(/must be an object/i)
  })

  it('validates a complete manifest including canonical CRC', () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    expect(manifest.nextChunk).toBe(manifest.chunkCount)
    expect(manifest.uploadedRecords).toBe(manifest.retainedCount)
    expect(calculateManifestCrc32c(manifest)).toBe(manifest.manifestCrc32c)
    expect(manifest.sessionTimeValid).toBe(false)
    expect(manifest.sessionStartEpochMs).toBeNull()
  })

  it('validates new NTP anchors while preserving the legacy CRC format', () => {
    const legacy = createDeterministicMockHistory()
    const oldManifest = parseSessionManifest('s_42', legacy.manifest)
    const anchored = anchoredManifestFixture()
    const parsed = parseSessionManifest('s_42', anchored.raw)
    expect(parsed.sessionTimeValid).toBe(true)
    expect(parsed.sessionStartEpochMs).toBe(1_720_000_000_000n)
    expect(parsed.sessionStartCaptureTimestampUs).toBe(5_000_000n)
    expect(parsed.sessionEndEpochMs).toBe(1_720_000_000_300n)
    expect(parsed.sessionBootId).toBe(0x1234abcd)
    expect(parsed.timeSource).toBe('ntp')
    expect(calculateManifestCrc32c(parsed)).toBe(parsed.manifestCrc32c)
    expect(calculateManifestCrc32c(parsed)).not.toBe(
      calculateManifestCrc32c(oldManifest),
    )
  })

  it('rejects malformed time-anchor combinations as untrusted data', () => {
    const anchored = anchoredManifestFixture()
    expect(() => parseSessionManifest('s_42', {
      ...anchored.raw,
      sessionStartEpochMs: 'not-decimal',
    })).toThrow(/decimal/i)
    expect(() => parseSessionManifest('s_42', {
      ...anchored.raw,
      sessionBootId: 0,
    })).toThrow(/sessionBootId/i)
    expect(() => parseSessionManifest('s_42', {
      ...anchored.raw,
      sessionEndEpochMs: '1719999999999',
    })).toThrow(/cannot be before/i)

    const invalidClockManifest: SessionManifest = {
      ...anchored.manifest,
      sessionTimeValid: false,
      sessionStartEpochMs: null,
      sessionEndTimeValid: false,
      sessionEndEpochMs: null,
      manifestCrc32c: '00000000',
    }
    invalidClockManifest.manifestCrc32c =
      calculateManifestCrc32c(invalidClockManifest)
    const parsed = parseSessionManifest('s_42', {
      ...anchored.raw,
      sessionTimeValid: false,
      sessionStartEpochMs: '0',
      sessionEndTimeValid: false,
      sessionEndEpochMs: undefined,
      manifestCrc32c: invalidClockManifest.manifestCrc32c,
    })
    expect(parsed.sessionTimeValid).toBe(false)
    expect(parsed.sessionStartEpochMs).toBeNull()
  })

  it('rejects conflicting manifests', () => {
    const fixture = createDeterministicMockHistory()
    const wrongSession = { ...fixture.manifest, sessionId: '43' }
    expect(() => parseSessionManifest('s_42', wrongSession)).toThrow(
      /does not match/i,
    )
    const wrongProgress = { ...fixture.manifest, nextChunk: 0 }
    expect(() => parseSessionManifest('s_42', wrongProgress)).toThrow(
      /incomplete/i,
    )
    const wrongCrc = { ...fixture.manifest, manifestCrc32c: '00000000' }
    expect(() => parseSessionManifest('s_42', wrongCrc)).toThrow(/CRC32C/i)
  })

  it('validates final partial chunks and detects chunk corruption', () => {
    const fixture = createDeterministicMockHistory()
    const manifest = parseSessionManifest('s_42', fixture.manifest)
    const rawChunk = fixture.chunks.get('000000')
    const chunk = parseSessionChunk(rawChunk, manifest, 0)
    expect(chunk.meta.recordCount).toBe(3)
    const decoded = decodeBase64(chunk.payload)
    expect(formatCrc32c(crc32c(decoded))).toBe(chunk.meta.crc32c)
    decoded[10] ^= 1
    expect(formatCrc32c(crc32c(decoded))).not.toBe(chunk.meta.crc32c)
  })

  it('produces deterministic mock history', () => {
    const first = createDeterministicMockHistory()
    const second = createDeterministicMockHistory()
    expect(first.manifest).toEqual(second.manifest)
    expect(first.chunks.get('000000')).toEqual(second.chunks.get('000000'))
  })
})
