import { crc32c } from './crc32c'
import {
  HISTORY_HARMONICS,
  HISTORY_PACKET_BYTES,
  HISTORY_RECORD_BYTES,
  HISTORY_WAVEFORM_SAMPLES,
  type ParsedWaveformPacket,
  type Pqr1RecordMetadata,
  type ValidatedPqr1Record,
  type WaveformPacketScalars,
} from './types'

export const PQR1_HEADER_BYTES = 64
export const PQR1_PAYLOAD_OFFSET = 64
export const PQR1_PADDING_OFFSET = 4_418
export const PQR1_PAYLOAD_CRC_OFFSET = 4_420
export const PQR1_WHOLE_RECORD_CRC_OFFSET = 4_424
export const PQR1_COMMIT_OFFSET = 4_428
export const PQR1_COMMIT_MARKER = 0x54494d43
export const PQR1_FORMAT_VERSION = 1

const PACKET_START_MARKER = 0xaa55aa55
// Canonical packed WaveformPacket_t offsets from final_display.ino:
// 0 start marker (u32), 4 sequence (u32), 8 voltage[1024] (i16),
// 2056 current[1024] (i16), 4104 twelve scalar float32 values,
// 4152 voltage harmonics H1-H25, 4252 current harmonics H1-H25,
// and 4352 the uint16 additive packet checksum.
const PACKET_SEQUENCE_OFFSET = 4
const VOLTAGE_SAMPLES_OFFSET = 8
const CURRENT_SAMPLES_OFFSET = 2_056
const SCALAR_METRICS_OFFSET = 4_104
const VOLTAGE_HARMONICS_OFFSET = 4_152
const CURRENT_HARMONICS_OFFSET = 4_252
const PACKET_CHECKSUM_OFFSET = 4_352

export type Pqr1ValidationCategory =
  | 'record-length'
  | 'magic'
  | 'record-version'
  | 'packet-format-version'
  | 'record-length-fields'
  | 'reserved-bytes'
  | 'alignment-padding'
  | 'commit-marker'
  | 'header-crc'
  | 'payload-crc'
  | 'whole-record-crc'
  | 'session-id'
  | 'logical-index'
  | 'packet-start-marker'
  | 'stm32-sequence'
  | 'packet-checksum'
  | 'packet-numeric-field'

export interface Pqr1ValidationContext {
  chunkIndex: number
  recordIndex: number
  expectedSessionId: bigint
  expectedLogicalIndex: bigint
}

export class Pqr1ValidationError extends Error {
  readonly category: Pqr1ValidationCategory
  readonly chunkIndex: number
  readonly recordIndex: number
  readonly logicalIndex: bigint | null

  constructor(
    category: Pqr1ValidationCategory,
    context: Pqr1ValidationContext,
    logicalIndex: bigint | null,
    detail: string,
  ) {
    const logical = logicalIndex === null ? 'unreadable' : logicalIndex.toString()
    super(
      `Chunk ${context.chunkIndex}, record ${context.recordIndex}, logical ${logical}: ${category} validation failed (${detail})`,
    )
    this.name = 'Pqr1ValidationError'
    this.category = category
    this.chunkIndex = context.chunkIndex
    this.recordIndex = context.recordIndex
    this.logicalIndex = logicalIndex
  }
}

function readUint64(view: DataView, offset: number): bigint {
  return view.getBigUint64(offset, true)
}

function fail(
  category: Pqr1ValidationCategory,
  context: Pqr1ValidationContext,
  logicalIndex: bigint | null,
  detail: string,
): never {
  throw new Pqr1ValidationError(category, context, logicalIndex, detail)
}

function finiteFloat(
  view: DataView,
  offset: number,
  fieldName: string,
  context: Pqr1ValidationContext,
  logicalIndex: bigint,
): number {
  const value = view.getFloat32(offset, true)
  if (!Number.isFinite(value)) {
    fail(
      'packet-numeric-field',
      context,
      logicalIndex,
      `${fieldName} is not finite`,
    )
  }
  return value
}

export function parseWaveformPacket(
  payload: Uint8Array,
  context: Pqr1ValidationContext,
  logicalIndex: bigint,
): ParsedWaveformPacket {
  if (payload.byteLength !== HISTORY_PACKET_BYTES) {
    fail(
      'record-length-fields',
      context,
      logicalIndex,
      `packet payload is ${payload.byteLength} bytes`,
    )
  }

  const view = new DataView(
    payload.buffer,
    payload.byteOffset,
    payload.byteLength,
  )
  const startMarker = view.getUint32(0, true)
  if (startMarker !== PACKET_START_MARKER) {
    fail(
      'packet-start-marker',
      context,
      logicalIndex,
      `expected 0x${PACKET_START_MARKER.toString(16).toUpperCase()}`,
    )
  }

  let calculatedChecksum = 0
  for (let offset = 0; offset < PACKET_CHECKSUM_OFFSET; offset += 1) {
    calculatedChecksum = (calculatedChecksum + payload[offset]) & 0xffff
  }
  const checksum = view.getUint16(PACKET_CHECKSUM_OFFSET, true)
  if (checksum !== calculatedChecksum) {
    fail(
      'packet-checksum',
      context,
      logicalIndex,
      `stored ${checksum}, calculated ${calculatedChecksum}`,
    )
  }

  const voltageSamples = new Int16Array(HISTORY_WAVEFORM_SAMPLES)
  const currentSamples = new Int16Array(HISTORY_WAVEFORM_SAMPLES)
  for (let index = 0; index < HISTORY_WAVEFORM_SAMPLES; index += 1) {
    voltageSamples[index] = view.getInt16(
      VOLTAGE_SAMPLES_OFFSET + index * 2,
      true,
    )
    currentSamples[index] = view.getInt16(
      CURRENT_SAMPLES_OFFSET + index * 2,
      true,
    )
  }

  const scalarNames: ReadonlyArray<keyof WaveformPacketScalars> = [
    'vRms',
    'iRms',
    'frequency',
    'powerFactor',
    'activePower',
    'apparentPower',
    'reactivePower',
    'crestFactorV',
    'crestFactorI',
    'swellFactor',
    'thdV',
    'thdI',
  ]
  const scalarValues: number[] = []
  for (let index = 0; index < scalarNames.length; index += 1) {
    scalarValues.push(
      finiteFloat(
        view,
        SCALAR_METRICS_OFFSET + index * 4,
        scalarNames[index],
        context,
        logicalIndex,
      ),
    )
  }
  const metrics: WaveformPacketScalars = {
    vRms: scalarValues[0],
    iRms: scalarValues[1],
    frequency: scalarValues[2],
    powerFactor: scalarValues[3],
    activePower: scalarValues[4],
    apparentPower: scalarValues[5],
    reactivePower: scalarValues[6],
    crestFactorV: scalarValues[7],
    crestFactorI: scalarValues[8],
    swellFactor: scalarValues[9],
    thdV: scalarValues[10],
    thdI: scalarValues[11],
  }

  const voltageHarmonics = new Float32Array(HISTORY_HARMONICS)
  const currentHarmonics = new Float32Array(HISTORY_HARMONICS)
  for (let index = 0; index < HISTORY_HARMONICS; index += 1) {
    voltageHarmonics[index] = finiteFloat(
      view,
      VOLTAGE_HARMONICS_OFFSET + index * 4,
      `V_H${index + 1}`,
      context,
      logicalIndex,
    )
    currentHarmonics[index] = finiteFloat(
      view,
      CURRENT_HARMONICS_OFFSET + index * 4,
      `I_H${index + 1}`,
      context,
      logicalIndex,
    )
  }

  return {
    startMarker,
    sequence: view.getUint32(PACKET_SEQUENCE_OFFSET, true),
    voltageSamples,
    currentSamples,
    metrics,
    voltageHarmonics,
    currentHarmonics,
    checksum,
  }
}

export function validatePqr1Record(
  record: Uint8Array,
  context: Pqr1ValidationContext,
): ValidatedPqr1Record {
  if (record.byteLength !== HISTORY_RECORD_BYTES) {
    fail(
      'record-length',
      context,
      null,
      `expected ${HISTORY_RECORD_BYTES}, received ${record.byteLength}`,
    )
  }

  const view = new DataView(record.buffer, record.byteOffset, record.byteLength)
  const logicalIndex = readUint64(view, 24)
  if (
    record[0] !== 0x50 ||
    record[1] !== 0x51 ||
    record[2] !== 0x52 ||
    record[3] !== 0x31
  ) {
    fail('magic', context, logicalIndex, 'expected PQR1')
  }
  if (view.getUint16(4, true) !== PQR1_FORMAT_VERSION) {
    fail('record-version', context, logicalIndex, 'unsupported format version')
  }
  if (
    view.getUint16(6, true) !== PQR1_HEADER_BYTES ||
    view.getUint32(8, true) !== HISTORY_RECORD_BYTES ||
    view.getUint32(12, true) !== HISTORY_PACKET_BYTES
  ) {
    fail('record-length-fields', context, logicalIndex, 'fixed lengths differ')
  }
  if (view.getUint16(44, true) !== 1) {
    fail(
      'packet-format-version',
      context,
      logicalIndex,
      'unsupported STM32 packet format version',
    )
  }
  if (view.getUint32(52, true) !== 0 || view.getUint32(56, true) !== 0) {
    fail('reserved-bytes', context, logicalIndex, 'reserved fields are non-zero')
  }
  if (record[PQR1_PADDING_OFFSET] !== 0 || record[PQR1_PADDING_OFFSET + 1] !== 0) {
    fail('alignment-padding', context, logicalIndex, 'padding is non-zero')
  }
  if (view.getUint32(PQR1_COMMIT_OFFSET, true) !== PQR1_COMMIT_MARKER) {
    fail('commit-marker', context, logicalIndex, 'commit marker is absent')
  }
  if (view.getUint32(60, true) !== crc32c(record.subarray(0, 60))) {
    fail('header-crc', context, logicalIndex, 'header CRC32C differs')
  }
  const payload = record.subarray(
    PQR1_PAYLOAD_OFFSET,
    PQR1_PAYLOAD_OFFSET + HISTORY_PACKET_BYTES,
  )
  if (view.getUint32(PQR1_PAYLOAD_CRC_OFFSET, true) !== crc32c(payload)) {
    fail('payload-crc', context, logicalIndex, 'payload CRC32C differs')
  }
  if (
    view.getUint32(PQR1_WHOLE_RECORD_CRC_OFFSET, true) !==
    crc32c(record.subarray(0, PQR1_WHOLE_RECORD_CRC_OFFSET))
  ) {
    fail('whole-record-crc', context, logicalIndex, 'record CRC32C differs')
  }

  const metadata: Pqr1RecordMetadata = {
    recordFormatVersion: view.getUint16(4, true),
    sessionId: readUint64(view, 16),
    logicalRecordIndex: logicalIndex,
    captureTimestampUs: readUint64(view, 32),
    stm32Sequence: view.getUint32(40, true),
    packetFormatVersion: view.getUint16(44, true),
    flags: view.getUint16(46, true),
    bootId: view.getUint32(48, true),
  }
  if (metadata.sessionId !== context.expectedSessionId) {
    fail('session-id', context, logicalIndex, 'record belongs to another session')
  }
  if (metadata.logicalRecordIndex !== context.expectedLogicalIndex) {
    fail('logical-index', context, logicalIndex, 'record is not consecutive')
  }

  const packet = parseWaveformPacket(payload, context, logicalIndex)
  if (
    packet.sequence !== metadata.stm32Sequence
  ) {
    fail(
      'stm32-sequence',
      context,
      logicalIndex,
      `header ${metadata.stm32Sequence}, payload ${packet.sequence}`,
    )
  }
  return { metadata, packet }
}
