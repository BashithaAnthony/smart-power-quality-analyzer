const REFLECTED_CASTAGNOLI_POLYNOMIAL = 0x82f63b78
const INITIAL_VALUE = 0xffffffff
const FINAL_XOR = 0xffffffff

const CRC32C_TABLE = new Uint32Array(256)

for (let tableIndex = 0; tableIndex < CRC32C_TABLE.length; tableIndex += 1) {
  let value = tableIndex
  for (let bitIndex = 0; bitIndex < 8; bitIndex += 1) {
    value = (value >>> 1) ^
      ((value & 1) === 1 ? REFLECTED_CASTAGNOLI_POLYNOMIAL : 0)
  }
  CRC32C_TABLE[tableIndex] = value >>> 0
}

/**
 * CRC-32C Castagnoli, matching the ESP32 codec: reflected input processing,
 * polynomial 0x82F63B78, initial 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 */
export function crc32c(bytes: Uint8Array): number {
  let crc = INITIAL_VALUE
  for (const byte of bytes) {
    const tableIndex = (crc ^ byte) & 0xff
    crc = (CRC32C_TABLE[tableIndex] ^ (crc >>> 8)) >>> 0
  }
  return (crc ^ FINAL_XOR) >>> 0
}

export function formatCrc32c(crc: number): string {
  return (crc >>> 0).toString(16).toUpperCase().padStart(8, '0')
}
