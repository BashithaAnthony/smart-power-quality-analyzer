import { describe, expect, it } from 'vitest'
import { crc32c } from './crc32c'
import { createPqr1Fixture } from './mockHistory'
import {
  PQR1_COMMIT_OFFSET,
  PQR1_PAYLOAD_CRC_OFFSET,
  PQR1_WHOLE_RECORD_CRC_OFFSET,
  Pqr1ValidationError,
  validatePqr1Record,
} from './pqr1'

const options = {
  sessionId: 99n,
  logicalIndex: 742n,
  captureTimestampUs: 8_765_432n,
  stm32Sequence: 1234,
  bootId: 0xa1b2c3d4,
  seed: 7,
}

function validate(record: Uint8Array, expectedSessionId = options.sessionId) {
  return validatePqr1Record(record, {
    chunkIndex: 4,
    recordIndex: 2,
    expectedSessionId,
    expectedLogicalIndex: options.logicalIndex,
  })
}

function expectCategory(record: Uint8Array, category: string): void {
  try {
    validate(record)
    throw new Error('Expected PQR1 validation to fail')
  } catch (error) {
    expect(error).toBeInstanceOf(Pqr1ValidationError)
    if (error instanceof Pqr1ValidationError) {
      expect(error.category).toBe(category)
      expect(error.message).toContain('Chunk 4, record 2')
    }
  }
}

describe('PQR1 fixed record parser', () => {
  it('validates and decodes the canonical record and packet layout', () => {
    const parsed = validate(createPqr1Fixture(options))
    expect(parsed.metadata.logicalRecordIndex).toBe(742n)
    expect(parsed.metadata.captureTimestampUs).toBe(8_765_432n)
    expect(parsed.metadata.stm32Sequence).toBe(1234)
    expect(parsed.packet.sequence).toBe(1234)
    expect(parsed.packet.voltageSamples).toHaveLength(1_024)
    expect(parsed.packet.currentSamples).toHaveLength(1_024)
    expect(parsed.packet.voltageHarmonics).toHaveLength(25)
    expect(parsed.packet.currentHarmonics).toHaveLength(25)
  })

  it('rejects bad magic', () => {
    const record = createPqr1Fixture(options)
    record[0] ^= 1
    expectCategory(record, 'magic')
  })

  it('rejects a missing commit marker', () => {
    const record = createPqr1Fixture(options)
    new DataView(record.buffer).setUint32(PQR1_COMMIT_OFFSET, 0, true)
    expectCategory(record, 'commit-marker')
  })

  it('rejects a bad header CRC', () => {
    const record = createPqr1Fixture(options)
    record[48] ^= 1
    expectCategory(record, 'header-crc')
  })

  it('rejects an unsupported packet format version', () => {
    const record = createPqr1Fixture(options)
    const view = new DataView(record.buffer)
    view.setUint16(44, 2, true)
    view.setUint32(60, crc32c(record.subarray(0, 60)), true)
    view.setUint32(
      PQR1_WHOLE_RECORD_CRC_OFFSET,
      crc32c(record.subarray(0, PQR1_WHOLE_RECORD_CRC_OFFSET)),
      true,
    )
    expectCategory(record, 'packet-format-version')
  })

  it('rejects a bad payload CRC', () => {
    const record = createPqr1Fixture(options)
    record[100] ^= 1
    expectCategory(record, 'payload-crc')
  })

  it('rejects a bad complete-record CRC', () => {
    const record = createPqr1Fixture(options)
    const view = new DataView(record.buffer)
    view.setUint32(
      PQR1_WHOLE_RECORD_CRC_OFFSET,
      view.getUint32(PQR1_WHOLE_RECORD_CRC_OFFSET, true) ^ 1,
      true,
    )
    expectCategory(record, 'whole-record-crc')
  })

  it('rejects the wrong session and non-consecutive logical indexes', () => {
    const record = createPqr1Fixture(options)
    expect(() => validate(record, 100n)).toThrow(/session-id/i)
    expect(() => validatePqr1Record(record, {
      chunkIndex: 4,
      recordIndex: 2,
      expectedSessionId: options.sessionId,
      expectedLogicalIndex: 743n,
    })).toThrow(/logical-index/i)
  })

  it('distinguishes payload and whole-record CRC fields', () => {
    const record = createPqr1Fixture(options)
    const view = new DataView(record.buffer)
    view.setUint32(
      PQR1_PAYLOAD_CRC_OFFSET,
      crc32c(record.subarray(64, 4_418)) ^ 1,
      true,
    )
    expectCategory(record, 'payload-crc')
  })
})
