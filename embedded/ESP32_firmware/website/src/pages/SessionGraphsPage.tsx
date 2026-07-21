import { useEffect, useMemo, useRef, useState } from 'react'
import {
  Bar,
  BarChart,
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { AppLink } from '../components/AppLink'
import { GraphLoadingProgress } from '../components/GraphLoadingProgress'
import { HistoryStatePanel } from '../components/HistoryStatePanel'
import { SessionWarnings } from '../components/SessionWarnings'
import {
  chunkIndexForLogicalIndex,
  createHarmonicChartData,
  createTrendChartData,
  formatCaptureLocalDateTime,
  formatCaptureUtcIso,
  formatTrendAxisTick,
  GRAPH_PAGE_SECTIONS,
  loadGraphTrendData,
  loadSelectedPacketHarmonics,
  isTrendMetricKey,
  metricDefinition,
  TREND_METRICS,
  trendCrossesLocalDateBoundary,
  type GraphChunkCache,
  type GraphLoadProgress,
  type ResolvedScalarTrendSummary,
  type SelectedPacketHarmonicsData,
  type TrendChartPoint,
  type TrendMetricKey,
} from '../history/graphData'
import { HistoryPipelineCancelledError } from '../history/chunkPipeline'
import { historyErrorMessage } from '../history/historyError'
import type { HistoryRepository, SessionManifest } from '../history/types'
import {
  formatLocalTimestamp,
  isValidSessionKey,
  parseSessionManifest,
} from '../history/validation'
import { subscribeToSessionInvalidation } from '../history/sessionInvalidation'

function isMetricChartPoint(value: unknown): value is TrendChartPoint {
  if (typeof value !== 'object' || value === null) return false
  return (
    'logicalIndex' in value && typeof value.logicalIndex === 'string' &&
    'stm32Sequence' in value && typeof value.stm32Sequence === 'number' &&
    'metricValue' in value && typeof value.metricValue === 'number'
    && 'timeAxisValue' in value && typeof value.timeAxisValue === 'number'
    && 'packetEpochMs' in value &&
      (value.packetEpochMs === null || typeof value.packetEpochMs === 'number')
    && 'elapsedSeconds' in value && typeof value.elapsedSeconds === 'number'
  )
}

function GraphTooltip({
  active,
  payloadValue,
  metricLabel,
  unit,
}: {
  active: boolean
  payloadValue: unknown
  metricLabel: string
  unit: string
}) {
  if (!active || !isMetricChartPoint(payloadValue)) return null
  return (
    <div className="chart-tooltip">
      <strong>Logical {payloadValue.logicalIndex}</strong>
      {payloadValue.packetEpochMs === null ? (
        <span>Elapsed time {payloadValue.elapsedSeconds.toFixed(3)} s</span>
      ) : (
        <>
          <span>{formatCaptureLocalDateTime(payloadValue.packetEpochMs)}</span>
          <span>UTC {formatCaptureUtcIso(payloadValue.packetEpochMs)}</span>
        </>
      )}
      <span>STM32 sequence {payloadValue.stm32Sequence}</span>
      <span>{metricLabel}: {payloadValue.metricValue} {unit}</span>
    </div>
  )
}

function HarmonicSpectrum({
  packet,
  source,
  loading,
}: {
  packet: SelectedPacketHarmonicsData | null
  source: 'voltage' | 'current'
  loading: boolean
}) {
  const title = source === 'voltage'
    ? GRAPH_PAGE_SECTIONS.voltageHarmonics
    : GRAPH_PAGE_SECTIONS.currentHarmonics
  const titleId = `${source}-harmonics-title`
  const harmonicData = useMemo(
    () => packet === null ? [] : createHarmonicChartData(packet, source),
    [packet, source],
  )

  return (
    <section
      className="graph-panel harmonic-panel"
      aria-labelledby={titleId}
      aria-busy={loading}
      data-spectrum-source={source}
      data-logical-index={packet?.logicalIndex.toString() ?? ''}
    >
      <div className="graph-panel-heading">
        <div>
          <p className="eyebrow">Selected packet spectrum</p>
          <h3 id={titleId}>{title}</h3>
        </div>
      </div>
      <div
        className="chart-frame harmonic-chart-frame"
        role="img"
        aria-label={`${title}, stored values without normalization`}
      >
        {packet === null ? (
          <div className="harmonic-chart-placeholder" role="status">
            {loading
              ? 'Validating the selected packet chunk…'
              : 'Select a validated packet to view this spectrum.'}
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={harmonicData} margin={{ top: 8, right: 16, bottom: 12, left: 0 }}>
              <CartesianGrid stroke="#20384a" strokeDasharray="3 3" />
              <XAxis dataKey="harmonic" stroke="#87a1b4" interval={1} />
              <YAxis stroke="#87a1b4" width={70} label={{ value: 'Stored value', angle: -90, position: 'insideLeft', fill: '#87a1b4' }} />
              <Tooltip cursor={{ fill: 'rgba(83, 210, 230, 0.08)' }} />
              <Bar dataKey="value" name="Stored harmonic value" fill={source === 'voltage' ? '#53d2e6' : '#f1b85a'} />
            </BarChart>
          </ResponsiveContainer>
        )}
      </div>
    </section>
  )
}

export function SessionGraphsPage({
  repository,
  sessionKey,
}: {
  repository: HistoryRepository
  sessionKey: string
}) {
  const [manifest, setManifest] = useState<SessionManifest | null>(null)
  const [manifestError, setManifestError] = useState<string | null>(null)
  const [summaries, setSummaries] = useState<ResolvedScalarTrendSummary[] | null>(null)
  const [trendMetric, setTrendMetric] = useState<TrendMetricKey>('vRms')
  const [selectedPosition, setSelectedPosition] = useState(0)
  const [selectedPacket, setSelectedPacket] = useState<SelectedPacketHarmonicsData | null>(null)
  const [trendProgress, setTrendProgress] = useState<GraphLoadProgress | null>(null)
  const [trendError, setTrendError] = useState<string | null>(null)
  const [trendNotice, setTrendNotice] = useState<string | null>(null)
  const [packetError, setPacketError] = useState<string | null>(null)
  const [packetLoading, setPacketLoading] = useState(false)
  const trendController = useRef<AbortController | null>(null)
  const packetController = useRef<AbortController | null>(null)
  const packetRequestGeneration = useRef(0)
  const packetRequestTimer = useRef<number | null>(null)
  const chunkCache = useRef<GraphChunkCache | null>(null)

  useEffect(() => {
    let active = true
    trendController.current?.abort()
    packetController.current?.abort()
    packetRequestGeneration.current += 1
    if (packetRequestTimer.current !== null) {
      window.clearTimeout(packetRequestTimer.current)
      packetRequestTimer.current = null
    }
    trendController.current = null
    packetController.current = null
    setManifest(null)
    setManifestError(null)
    setSummaries(null)
    setSelectedPacket(null)
    setTrendProgress(null)
    setTrendError(null)
    setTrendNotice(null)
    setPacketError(null)
    setPacketLoading(false)
    chunkCache.current = null
    if (!isValidSessionKey(sessionKey)) {
      setManifestError('The graph URL does not contain a valid session key.')
      return () => { active = false }
    }
    repository.getManifest(sessionKey).then(
      (value) => {
        if (!active) return
        try {
          setManifest(parseSessionManifest(sessionKey, value))
        } catch (error) {
          setManifestError(historyErrorMessage(error))
        }
      },
      (error) => {
        if (active) setManifestError(historyErrorMessage(error))
      },
    )
    return () => { active = false }
  }, [repository, sessionKey])

  useEffect(() => () => {
    trendController.current?.abort()
    packetController.current?.abort()
    packetRequestGeneration.current += 1
    if (packetRequestTimer.current !== null) {
      window.clearTimeout(packetRequestTimer.current)
      packetRequestTimer.current = null
    }
    chunkCache.current = null
  }, [])

  useEffect(() => subscribeToSessionInvalidation(sessionKey, () => {
    trendController.current?.abort()
    packetController.current?.abort()
    trendController.current = null
    packetController.current = null
    packetRequestGeneration.current += 1
    if (packetRequestTimer.current !== null) {
      window.clearTimeout(packetRequestTimer.current)
      packetRequestTimer.current = null
    }
    chunkCache.current = null
    setManifest(null)
    setSummaries(null)
    setSelectedPacket(null)
    setTrendProgress(null)
    setTrendNotice(null)
    setPacketLoading(false)
    setManifestError('This uploaded session no longer exists in cloud history.')
  }), [sessionKey])

  async function startTrendLoad(): Promise<void> {
    if (manifest === null || trendController.current !== null) return
    const controller = new AbortController()
    trendController.current = controller
    setSummaries(null)
    setSelectedPacket(null)
    setTrendError(null)
    setTrendNotice(null)
    setPacketError(null)
    setTrendProgress(null)
    chunkCache.current = null
    try {
      const loaded = await loadGraphTrendData({
        repository,
        manifest,
        signal: controller.signal,
        onProgress: setTrendProgress,
      })
      if (!controller.signal.aborted) {
        setSummaries(loaded)
        setSelectedPosition(0)
      }
    } catch (error) {
      if (error instanceof HistoryPipelineCancelledError) {
        setTrendProgress(null)
        setTrendNotice('Graph loading cancelled. No partial graph data was kept.')
      } else {
        setTrendError(historyErrorMessage(error))
      }
    } finally {
      if (trendController.current === controller) trendController.current = null
    }
  }

  function cancelTrendLoad(): void {
    trendController.current?.abort()
    setTrendProgress((current) => current === null
      ? current
      : { ...current, cancellationRequested: true })
  }

  useEffect(() => {
    if (manifest === null || summaries === null || summaries.length === 0) return
    const summary = summaries[selectedPosition]
    if (summary === undefined) return
    const requestGeneration = packetRequestGeneration.current + 1
    packetRequestGeneration.current = requestGeneration
    packetController.current?.abort()
    packetController.current = null
    if (packetRequestTimer.current !== null) {
      window.clearTimeout(packetRequestTimer.current)
      packetRequestTimer.current = null
    }
    setPacketLoading(true)
    setPacketError(null)
    setSelectedPacket(null)
    const selectedChunkIndex = chunkIndexForLogicalIndex(
      manifest,
      summary.logicalIndex,
    )
    const cachedChunk = chunkCache.current?.chunkIndex === selectedChunkIndex
      ? chunkCache.current
      : null
    let controller: AbortController | null = null

    const loadPacket = (): void => {
      packetRequestTimer.current = null
      if (packetRequestGeneration.current !== requestGeneration) return
      controller = new AbortController()
      packetController.current = controller
      void loadSelectedPacketHarmonics(
        repository,
        manifest,
        summary.logicalIndex,
        controller.signal,
        cachedChunk,
      ).then(
        (result) => {
          if (
            controller?.signal.aborted === true ||
            packetRequestGeneration.current !== requestGeneration
          ) {
            return
          }
          chunkCache.current = result.cache
          setSelectedPacket(result.packet)
        },
        (error) => {
          if (
            controller?.signal.aborted !== true &&
            packetRequestGeneration.current === requestGeneration
          ) {
            setPacketError(historyErrorMessage(error))
          }
        },
      ).finally(() => {
        if (
          packetController.current === controller &&
          packetRequestGeneration.current === requestGeneration
        ) {
          packetController.current = null
          setPacketLoading(false)
        }
      })
    }

    if (cachedChunk !== null) {
      loadPacket()
    } else {
      // Range inputs can emit many intermediate values while dragging. Delay
      // cross-chunk requests briefly so only the user's final selection is
      // likely to reach Firebase; same-chunk selections use the cache at once.
      packetRequestTimer.current = window.setTimeout(loadPacket, 120)
    }

    return () => {
      if (packetRequestTimer.current !== null) {
        window.clearTimeout(packetRequestTimer.current)
        packetRequestTimer.current = null
      }
      controller?.abort()
    }
  }, [manifest, repository, selectedPosition, summaries])

  const selectedMetric = metricDefinition(trendMetric)
  const actualTimeAvailable = summaries?.[0]?.packetEpochMs !== null &&
    summaries?.[0]?.packetEpochMs !== undefined
  const includeDateOnAxis = useMemo(
    () => trendCrossesLocalDateBoundary(summaries ?? []),
    [summaries],
  )
  const trendChartData = useMemo<TrendChartPoint[]>(
    () => createTrendChartData(summaries ?? [], trendMetric),
    [summaries, trendMetric],
  )
  const selectedSummary = summaries?.[selectedPosition] ?? null

  return (
    <main className="dashboard-main history-main graph-page">
      <nav className="breadcrumb" aria-label="Breadcrumb">
        <AppLink href="/history">History</AppLink>
        <span aria-hidden="true">/</span>
        <AppLink href={`/history/${encodeURIComponent(sessionKey)}`}>{sessionKey}</AppLink>
        <span aria-hidden="true">/</span>
        <span>Graphs</span>
      </nav>

      {manifest === null && manifestError === null && (
        <HistoryStatePanel
          title="Validating session manifest"
          detail="No historical chunks are downloaded until you choose Load Graph Data."
        />
      )}
      {manifestError !== null && (
        <HistoryStatePanel
          title="Graphs cannot be opened"
          detail={manifestError}
          tone="error"
          action={<AppLink className="state-link" href="/history">Return to history</AppLink>}
        />
      )}

      {manifest !== null && (
        <>
          <section className="detail-hero graph-hero" aria-labelledby="graph-session-title">
            <div>
              <p className="eyebrow">Historical session graphs</p>
              <h2 id="graph-session-title">Session {manifest.sessionId.toString()}</h2>
              <p>
                {manifest.retainedCount.toLocaleString()} records · logical{' '}
                {manifest.firstLogicalIndex.toString()}–{manifest.lastLogicalIndex.toString()} · uploaded{' '}
                {formatLocalTimestamp(manifest.uploadCompletedAt)}
              </p>
            </div>
            <AppLink className="secondary-button graph-back-link" href={`/history/${encodeURIComponent(sessionKey)}`}>
              Back to Session
            </AppLink>
          </section>

          <SessionWarnings manifest={manifest} />

          {summaries === null && trendController.current === null && trendProgress === null && (
            <section className="graph-load-panel">
              <div>
                <p className="eyebrow">On-demand validation</p>
                <h3>Graph data is not loaded yet</h3>
                <p>
                  Load scalar summaries sequentially from {manifest.chunkCount.toLocaleString()} validated chunks.
                  Both selected-packet harmonic spectra remain on demand.
                </p>
              </div>
              <button className="primary-button" type="button" onClick={() => void startTrendLoad()}>
                Load Graph Data
              </button>
            </section>
          )}

          {trendProgress !== null && summaries === null && trendError === null && (
            <GraphLoadingProgress progress={trendProgress} onCancel={cancelTrendLoad} />
          )}
          {trendNotice !== null && summaries === null && (
            <p className="export-notice" aria-live="polite">{trendNotice}</p>
          )}
          {trendError !== null && (
            <HistoryStatePanel
              title="Graph data validation failed"
              detail={trendError}
              tone="error"
              action={(
                <button className="secondary-button state-action" type="button" onClick={() => void startTrendLoad()}>
                  Retry Graph Data
                </button>
              )}
            />
          )}

          {summaries !== null && (
            <>
              <section className="graph-panel" aria-labelledby="trend-title">
                <div className="graph-panel-heading">
                  <div>
                    <p className="eyebrow">All retained packets</p>
                    <h3 id="trend-title">{GRAPH_PAGE_SECTIONS.trend}</h3>
                  </div>
                  <label className="graph-control">
                    Trend metric
                    <select
                      value={trendMetric}
                      onChange={(event) => {
                        if (isTrendMetricKey(event.target.value)) {
                          setTrendMetric(event.target.value)
                        }
                      }}
                    >
                      {TREND_METRICS.map((metric) => (
                        <option key={metric.key} value={metric.key}>{metric.label}</option>
                      ))}
                    </select>
                  </label>
                </div>
                <p className="graph-unit">Selected unit: <strong>{selectedMetric.unit}</strong></p>
                {!actualTimeAvailable && (
                  <p className="graph-time-notice" role="note">
                    Actual capture time is unavailable for this session.
                  </p>
                )}
                <p className="graph-axis-label">
                  X-axis: {actualTimeAvailable ? 'Actual capture time (local)' : 'Elapsed time (s)'}
                </p>
                <div className="chart-frame chart-frame-trend" role="img" aria-label={`${selectedMetric.label} by ${actualTimeAvailable ? 'actual capture time' : 'elapsed time'}`}>
                  <ResponsiveContainer width="100%" height="100%">
                    <LineChart data={trendChartData} margin={{ top: 8, right: 16, bottom: 14, left: 4 }}>
                      <CartesianGrid stroke="#20384a" strokeDasharray="3 3" />
                      <XAxis
                        dataKey="timeAxisValue"
                        type="number"
                        domain={['dataMin', 'dataMax']}
                        stroke="#87a1b4"
                        minTickGap={32}
                        tickFormatter={(value: number) => formatTrendAxisTick(
                          value,
                          actualTimeAvailable,
                          includeDateOnAxis,
                        )}
                      />
                      <YAxis stroke="#87a1b4" width={76} label={{ value: selectedMetric.unit, angle: -90, position: 'insideLeft', fill: '#87a1b4' }} />
                      <Tooltip
                        content={({ active, payload }) => (
                          <GraphTooltip
                            active={active === true}
                            payloadValue={payload?.[0]?.payload}
                            metricLabel={selectedMetric.label}
                            unit={selectedMetric.unit}
                          />
                        )}
                      />
                      <Line dataKey="metricValue" name={selectedMetric.label} stroke={selectedMetric.color} dot={false} activeDot={{ r: 4 }} isAnimationActive={false} />
                    </LineChart>
                  </ResponsiveContainer>
                </div>
              </section>

              <section className="packet-selector-panel" aria-labelledby="packet-selector-title">
                <div>
                  <p className="eyebrow">Harmonic spectrum packet</p>
                  <h3 id="packet-selector-title">{GRAPH_PAGE_SECTIONS.packetSelector}</h3>
                  {selectedSummary !== null && (
                    <p aria-live="polite">
                      Logical {selectedSummary.logicalIndex.toString()} · STM32 sequence{' '}
                      {selectedSummary.stm32Sequence} · packet {selectedPosition + 1} of{' '}
                      {summaries.length} · chunk{' '}
                      {chunkIndexForLogicalIndex(manifest, selectedSummary.logicalIndex) + 1}
                    </p>
                  )}
                </div>
                <div className="packet-controls">
                  <button
                    className="secondary-button"
                    type="button"
                    disabled={selectedPosition === 0}
                    onClick={() => setSelectedPosition((position) => Math.max(0, position - 1))}
                  >
                    Previous packet
                  </button>
                  <label className="graph-control graph-slider-control">
                    Packet position
                    <input
                      type="range"
                      min="0"
                      max={summaries.length - 1}
                      value={selectedPosition}
                      onChange={(event) => setSelectedPosition(Number(event.target.value))}
                    />
                  </label>
                  <label className="graph-control">
                    Logical index
                    <select
                      value={selectedPosition}
                      onChange={(event) => setSelectedPosition(Number(event.target.value))}
                    >
                      {summaries.map((summary, position) => (
                        <option key={summary.logicalIndex.toString()} value={position}>
                          {summary.logicalIndex.toString()}
                        </option>
                      ))}
                    </select>
                  </label>
                  <button
                    className="secondary-button"
                    type="button"
                    disabled={selectedPosition === summaries.length - 1}
                    onClick={() => setSelectedPosition((position) => Math.min(summaries.length - 1, position + 1))}
                  >
                    Next packet
                  </button>
                </div>
              </section>

              {packetError !== null && (
                <div className="message message-error" role="alert">
                  <span className="message-symbol" aria-hidden="true">!</span>
                  <span>{packetError}</span>
                </div>
              )}
              <div className="harmonic-graphs" aria-live="polite">
                <HarmonicSpectrum
                  packet={selectedPacket}
                  source="voltage"
                  loading={packetLoading}
                />
                <HarmonicSpectrum
                  packet={selectedPacket}
                  source="current"
                  loading={packetLoading}
                />
              </div>
            </>
          )}
        </>
      )}
    </main>
  )
}
