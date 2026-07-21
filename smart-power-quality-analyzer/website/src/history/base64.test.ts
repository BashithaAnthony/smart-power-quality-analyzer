import { describe, expect, it } from 'vitest'
import { decodeBase64, encodeBase64 } from './base64'

describe('browser Base64 codec', () => {
  it.each([
    ['', ''],
    ['Zg==', 'f'],
    ['Zm8=', 'fo'],
    ['Zm9v', 'foo'],
    ['SGVsbG8sIFBRUjEh', 'Hello, PQR1!'],
  ])('decodes %s', (encoded, plain) => {
    expect(new TextDecoder().decode(decodeBase64(encoded))).toBe(plain)
  })

  it('round-trips deterministic binary without Buffer', () => {
    const bytes = Uint8Array.from(
      { length: 257 },
      (_, index) => (index * 73 + 19) & 0xff,
    )
    expect(decodeBase64(encodeBase64(bytes))).toEqual(bytes)
  })

  it('rejects invalid characters and padding', () => {
    expect(() => decodeBase64('%%%?')).toThrow(/invalid character/i)
    expect(() => decodeBase64('A===')).toThrow()
    expect(() => decodeBase64('Zh==')).toThrow(/padding bits/i)
  })
})
