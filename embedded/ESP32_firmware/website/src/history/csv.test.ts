import { describe, expect, it } from 'vitest'
import {
  WAVEFORM_CSV_COLUMNS,
  escapeCsvField,
  waveformCsvHeader,
  waveformCsvRow,
} from './csv'
import { createDeterministicMockHistory } from './mockHistory'
import { validatePqr1Record } from './pqr1'
import { decodeBase64 } from './base64'
import { parseSessionChunk, parseSessionManifest } from './validation'

function fixtureRecord() {
  const fixture = createDeterministicMockHistory()
  const manifest = parseSessionManifest('s_42', fixture.manifest)
  const chunk = parseSessionChunk(fixture.chunks.get('000000'), manifest, 0)
  const bytes = decodeBase64(chunk.payload).subarray(0, 4_432)
  const record = validatePqr1Record(bytes, {
    chunkIndex: 0,
    recordIndex: 0,
    expectedSessionId: manifest.sessionId,
    expectedLogicalIndex: manifest.firstLogicalIndex,
  })
  return { manifest, record }
}

describe('RFC 4180 CSV generation', () => {
  it('escapes commas, quotes, and newlines', () => {
    expect(escapeCsvField('plain')).toBe('plain')
    expect(escapeCsvField('a,b')).toBe('"a,b"')
    expect(escapeCsvField('a"b')).toBe('"a""b"')
    expect(escapeCsvField('a\nb')).toBe('"a\nb"')
  })

  it('generates long-format waveform rows', () => {
    const { manifest, record } = fixtureRecord()
    expect(waveformCsvHeader()).toContain('sample_index,voltage_sample,current_sample')
    expect(WAVEFORM_CSV_COLUMNS).toHaveLength(11)
    const row = waveformCsvRow(manifest, record, 17)
    expect(row.split(',')).toHaveLength(11)
    expect(row).toContain(',17,')
    expect(row.endsWith('\r\n')).toBe(true)
  })
})
