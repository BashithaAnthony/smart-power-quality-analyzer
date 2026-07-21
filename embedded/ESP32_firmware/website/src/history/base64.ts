const BASE64_ALPHABET =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

const BASE64_VALUES = new Int16Array(128).fill(-1)
for (let index = 0; index < BASE64_ALPHABET.length; index += 1) {
  BASE64_VALUES[BASE64_ALPHABET.charCodeAt(index)] = index
}

function base64Value(character: string): number {
  const code = character.charCodeAt(0)
  return code < BASE64_VALUES.length ? BASE64_VALUES[code] : -1
}

export function decodeBase64(source: string): Uint8Array {
  if (source.length === 0) return new Uint8Array(0)
  if (source.length % 4 !== 0) {
    throw new Error('Base64 length must be a multiple of four')
  }

  let padding = 0
  if (source.endsWith('==')) padding = 2
  else if (source.endsWith('=')) padding = 1

  const output = new Uint8Array((source.length / 4) * 3 - padding)
  let outputIndex = 0

  for (let index = 0; index < source.length; index += 4) {
    const finalQuartet = index + 4 === source.length
    const character0 = source[index]
    const character1 = source[index + 1]
    const character2 = source[index + 2]
    const character3 = source[index + 3]
    const value0 = base64Value(character0)
    const value1 = base64Value(character1)
    const value2 = character2 === '=' ? 0 : base64Value(character2)
    const value3 = character3 === '=' ? 0 : base64Value(character3)

    if (value0 < 0 || value1 < 0 || value2 < 0 || value3 < 0) {
      throw new Error('Base64 contains an invalid character')
    }
    if ((character2 === '=' || character3 === '=') && !finalQuartet) {
      throw new Error('Base64 padding is only valid in the final quartet')
    }
    if (character2 === '=' && character3 !== '=') {
      throw new Error('Base64 has invalid padding')
    }
    if (character2 === '=' && (value1 & 0x0f) !== 0) {
      throw new Error('Base64 has non-zero padding bits')
    }
    if (character3 === '=' && character2 !== '=' && (value2 & 0x03) !== 0) {
      throw new Error('Base64 has non-zero padding bits')
    }

    const packed =
      (value0 << 18) | (value1 << 12) | (value2 << 6) | value3
    if (outputIndex < output.length) output[outputIndex++] = packed >>> 16
    if (outputIndex < output.length) output[outputIndex++] = packed >>> 8
    if (outputIndex < output.length) output[outputIndex++] = packed
  }

  return output
}

export function encodeBase64(source: Uint8Array): string {
  const parts: string[] = []
  for (let index = 0; index < source.length; index += 3) {
    const remaining = source.length - index
    const byte0 = source[index]
    const byte1 = remaining > 1 ? source[index + 1] : 0
    const byte2 = remaining > 2 ? source[index + 2] : 0
    const packed = (byte0 << 16) | (byte1 << 8) | byte2
    parts.push(
      BASE64_ALPHABET[(packed >>> 18) & 0x3f],
      BASE64_ALPHABET[(packed >>> 12) & 0x3f],
      remaining > 1 ? BASE64_ALPHABET[(packed >>> 6) & 0x3f] : '=',
      remaining > 2 ? BASE64_ALPHABET[packed & 0x3f] : '=',
    )
  }
  return parts.join('')
}
