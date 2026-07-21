export interface LiveMetrics {
  vRms: number
  iRms: number
  frequency: number
  powerFactor: number
  activePower: number
  apparentPower: number
  reactivePower: number
  crestFactorV: number
  crestFactorI: number
  swellFactor: number
  thdV: number
  thdI: number
}

export interface LiveDeviceStatus {
  online: boolean
  logging: boolean
  wifiConnected: boolean
  wifiRssi: number
}

export interface LiveTelemetry {
  deviceId: string
  seq: number
  uptimeMs: number
  metrics: LiveMetrics
  status: LiveDeviceStatus
}

type UnknownRecord = Record<string, unknown>

function record(value: unknown): UnknownRecord {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new TypeError('Expected an object')
  }
  return value as UnknownRecord
}

function numberField(source: UnknownRecord, key: string): number {
  const value = source[key]
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`Expected a finite number for ${key}`)
  }
  return value
}

function booleanField(source: UnknownRecord, key: string): boolean {
  const value = source[key]
  if (typeof value !== 'boolean') {
    throw new TypeError(`Expected a boolean for ${key}`)
  }
  return value
}

export function parseLiveTelemetry(value: unknown): LiveTelemetry | null {
  try {
    const root = record(value)
    const metrics = record(root.metrics)
    const status = record(root.status)

    if (typeof root.deviceId !== 'string' || root.deviceId.trim() === '') {
      throw new TypeError('Expected a device ID')
    }

    return {
      deviceId: root.deviceId,
      seq: numberField(root, 'seq'),
      uptimeMs: numberField(root, 'uptimeMs'),
      metrics: {
        vRms: numberField(metrics, 'vRms'),
        iRms: numberField(metrics, 'iRms'),
        frequency: numberField(metrics, 'frequency'),
        powerFactor: numberField(metrics, 'powerFactor'),
        activePower: numberField(metrics, 'activePower'),
        apparentPower: numberField(metrics, 'apparentPower'),
        reactivePower: numberField(metrics, 'reactivePower'),
        crestFactorV: numberField(metrics, 'crestFactorV'),
        crestFactorI: numberField(metrics, 'crestFactorI'),
        swellFactor: numberField(metrics, 'swellFactor'),
        thdV: numberField(metrics, 'thdV'),
        thdI: numberField(metrics, 'thdI'),
      },
      status: {
        online: booleanField(status, 'online'),
        logging: booleanField(status, 'logging'),
        wifiConnected: booleanField(status, 'wifiConnected'),
        wifiRssi: numberField(status, 'wifiRssi'),
      },
    }
  } catch {
    return null
  }
}
