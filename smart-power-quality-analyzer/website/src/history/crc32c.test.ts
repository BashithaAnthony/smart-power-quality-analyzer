import { describe, expect, it } from 'vitest'
import { crc32c, formatCrc32c } from './crc32c'

describe('CRC32C Castagnoli', () => {
  it('matches the standard 123456789 vector', () => {
    expect(formatCrc32c(crc32c(new TextEncoder().encode('123456789'))))
      .toBe('E3069283')
  })

  it('matches empty and deterministic binary vectors', () => {
    expect(formatCrc32c(crc32c(new Uint8Array(0)))).toBe('00000000')
    const binary = Uint8Array.from(
      { length: 32 },
      (_, index) => (index * 17 + 3) & 0xff,
    )
    expect(formatCrc32c(crc32c(binary))).toBe('6B08574E')
  })
})
