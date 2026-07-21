import {
  lazy,
  Suspense,
  useEffect,
  useMemo,
  useRef,
  useState,
  type FormEvent,
  type ReactNode,
} from 'react'
import { FirebaseError } from 'firebase/app'
import {
  onAuthStateChanged,
  signInWithEmailAndPassword,
  signOut,
  type User,
} from 'firebase/auth'
import { onValue, ref } from 'firebase/database'
import './App.css'
import {
  auth,
  database,
  firebaseConfigurationError,
} from './lib/firebase'
import {
  parseLiveTelemetry,
  type LiveTelemetry,
} from './types/liveTelemetry'
import { HistoryStatePanel } from './components/HistoryStatePanel'
import { AppLink } from './components/AppLink'
import { createHistoryRepository } from './history/repositoryFactory'
import { HistoryListPage } from './pages/HistoryListPage'
import { SessionDetailsPage } from './pages/SessionDetailsPage'
import { navigateTo, useAppRoute } from './routing'

const SessionGraphsPage = lazy(() => import('./pages/SessionGraphsPage').then(
  (module) => ({ default: module.SessionGraphsPage }),
))

const DEVICE_PATH = 'devices/PQ-3PH-001/live'
const STALE_AFTER_MS = 5_000

type DataState =
  | 'loading'
  | 'ready'
  | 'empty'
  | 'permission-denied'
  | 'connection-error'

interface MetricCardData {
  label: string
  value: string
  unit: string
  group: 'voltage' | 'current' | 'power' | 'quality'
}

function formatNumber(value: number, digits = 2): string {
  return new Intl.NumberFormat('en-US', {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits,
  }).format(value)
}

function formatPower(value: number, baseUnit: string): [string, string] {
  if (Math.abs(value) >= 1_000) {
    return [formatNumber(value / 1_000), `k${baseUnit}`]
  }
  return [formatNumber(value, 1), baseUnit]
}

function formatUptime(uptimeMs: number): string {
  const totalSeconds = Math.max(0, Math.floor(uptimeMs / 1_000))
  const days = Math.floor(totalSeconds / 86_400)
  const hours = Math.floor((totalSeconds % 86_400) / 3_600)
  const minutes = Math.floor((totalSeconds % 3_600) / 60)
  const seconds = totalSeconds % 60
  const clock = [hours, minutes, seconds]
    .map((value) => value.toString().padStart(2, '0'))
    .join(':')
  return days > 0 ? `${days}d ${clock}` : clock
}

function authErrorMessage(error: unknown): string {
  if (error instanceof FirebaseError) {
    if (
      error.code === 'auth/invalid-credential' ||
      error.code === 'auth/user-not-found' ||
      error.code === 'auth/wrong-password'
    ) {
      return 'The email or password is incorrect.'
    }
    if (error.code === 'auth/too-many-requests') {
      return 'Too many attempts. Wait a moment and try again.'
    }
    if (error.code === 'auth/network-request-failed') {
      return 'The authentication service could not be reached.'
    }
  }
  return 'Sign-in failed. Check your details and try again.'
}

function isPermissionDenied(error: Error): boolean {
  const code = 'code' in error ? String(error.code) : ''
  return (
    code.toLowerCase().includes('permission') ||
    error.message.toLowerCase().includes('permission_denied')
  )
}

function LoginScreen({
  configurationError,
  sessionError,
}: {
  configurationError: string | null
  sessionError: string | null
}) {
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState<string | null>(
    configurationError ?? sessionError,
  )
  const [isSubmitting, setIsSubmitting] = useState(false)

  async function handleSubmit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (auth === null) {
      setError(configurationError ?? 'Firebase is not configured.')
      return
    }

    setError(null)
    setIsSubmitting(true)
    try {
      await signInWithEmailAndPassword(auth, email.trim(), password)
    } catch (signInError) {
      setError(authErrorMessage(signInError))
    } finally {
      setIsSubmitting(false)
    }
  }

  return (
    <main className="login-shell">
      <section className="login-panel" aria-labelledby="login-title">
        <div className="brand-mark" aria-hidden="true">
          PQ
        </div>
        <p className="eyebrow">ElectroSquad instrumentation</p>
        <h1 id="login-title">Power Quality Console</h1>
        <p className="login-intro">
          Sign in with your authorized Firebase account to view live analyzer
          telemetry.
        </p>

        <form className="login-form" onSubmit={handleSubmit}>
          <label htmlFor="email">Email address</label>
          <input
            id="email"
            name="email"
            type="email"
            autoComplete="username"
            value={email}
            onChange={(event) => setEmail(event.target.value)}
            required
            disabled={isSubmitting || configurationError !== null}
          />

          <label htmlFor="password">Password</label>
          <input
            id="password"
            name="password"
            type="password"
            autoComplete="current-password"
            value={password}
            onChange={(event) => setPassword(event.target.value)}
            required
            disabled={isSubmitting || configurationError !== null}
          />

          {error !== null && (
            <div className="message message-error" role="alert">
              <span className="message-symbol" aria-hidden="true">
                !
              </span>
              <span>{error}</span>
            </div>
          )}

          <button
            className="primary-button"
            type="submit"
            disabled={isSubmitting || configurationError !== null}
          >
            {isSubmitting ? 'Authenticating…' : 'Sign in to console'}
          </button>
        </form>

        <p className="security-note">
          Credentials are sent directly to Firebase Authentication and are not
          stored by this dashboard.
        </p>
      </section>
    </main>
  )
}

function StatePanel({ state }: { state: Exclude<DataState, 'ready'> }) {
  const content = {
    loading: {
      title: 'Establishing live link',
      detail: 'Authenticating the database stream and waiting for telemetry.',
      symbol: '↻',
    },
    empty: {
      title: 'No live telemetry',
      detail: 'The device path exists but has not published a valid snapshot.',
      symbol: '—',
    },
    'permission-denied': {
      title: 'Database permission denied',
      detail: 'This account is authenticated but cannot read the device path.',
      symbol: '!',
    },
    'connection-error': {
      title: 'Live connection unavailable',
      detail: 'The telemetry stream could not be read. Check the network and configuration.',
      symbol: '×',
    },
  }[state]

  return (
    <section className={`state-panel state-${state}`} aria-live="polite">
      <span className="state-symbol" aria-hidden="true">
        {content.symbol}
      </span>
      <div>
        <h2>{content.title}</h2>
        <p>{content.detail}</p>
      </div>
    </section>
  )
}

function MetricCard({ metric }: { metric: MetricCardData }) {
  return (
    <article className={`metric-card metric-${metric.group}`}>
      <p>{metric.label}</p>
      <div className="metric-reading">
        <strong>{metric.value}</strong>
        <span>{metric.unit}</span>
      </div>
    </article>
  )
}

function SiteHeader({
  user,
  activeRoute,
}: {
  user: User
  activeRoute: 'live' | 'history'
}) {
  const [isSigningOut, setIsSigningOut] = useState(false)

  async function handleSignOut() {
    if (auth === null) return
    setIsSigningOut(true)
    try {
      await signOut(auth)
    } finally {
      setIsSigningOut(false)
    }
  }

  return (
    <header className="topbar">
      <div className="brand-lockup">
        <span className="brand-mark brand-mark-small" aria-hidden="true">
          PQ
        </span>
        <div>
          <p className="eyebrow">Smart power quality analyzer</p>
          <h1>Instrumentation Console</h1>
        </div>
      </div>
      <nav className="primary-navigation" aria-label="Primary navigation">
        <AppLink
          href="/"
          className="navigation-link"
          ariaCurrent={activeRoute === 'live' ? 'page' : undefined}
        >
          Live
        </AppLink>
        <AppLink
          href="/history"
          className="navigation-link"
          ariaCurrent={activeRoute === 'history' ? 'page' : undefined}
        >
          History
        </AppLink>
      </nav>
      <div className="account-actions">
        <span className="account-email" title={user.email ?? 'Authenticated user'}>
          {user.email ?? 'Authenticated user'}
        </span>
        <button
          className="secondary-button"
          type="button"
          onClick={handleSignOut}
          disabled={isSigningOut}
        >
          {isSigningOut ? 'Signing out…' : 'Sign out'}
        </button>
      </div>
    </header>
  )
}

function AuthenticatedShell({
  user,
  activeRoute,
  children,
}: {
  user: User
  activeRoute: 'live' | 'history'
  children: ReactNode
}) {
  return (
    <div className="dashboard-shell">
      <SiteHeader user={user} activeRoute={activeRoute} />
      {children}
      <footer className="dashboard-footer">
        <span>Authenticated power-quality console</span>
        <span>Historical graphs and exports are validated in this browser</span>
      </footer>
    </div>
  )
}

function Dashboard({ user }: { user: User }) {
  const [telemetry, setTelemetry] = useState<LiveTelemetry | null>(null)
  const [dataState, setDataState] = useState<DataState>('loading')
  const [lastUpdate, setLastUpdate] = useState<Date | null>(null)
  const [isStale, setIsStale] = useState(true)
  const [isSigningOut, setIsSigningOut] = useState(false)
  const lastProgressAt = useRef<number | null>(null)
  const lastSequenceSignature = useRef<string | null>(null)

  useEffect(() => {
    setTelemetry(null)
    setDataState('loading')
    setLastUpdate(null)
    setIsStale(true)
    lastProgressAt.current = null
    lastSequenceSignature.current = null

    if (database === null) {
      setDataState('connection-error')
      return
    }

    const liveReference = ref(database, DEVICE_PATH)
    const unsubscribe = onValue(
      liveReference,
      (snapshot) => {
        const receivedAt = Date.now()
        setLastUpdate(new Date(receivedAt))

        if (!snapshot.exists()) {
          setTelemetry(null)
          setDataState('empty')
          setIsStale(true)
          return
        }

        const parsed = parseLiveTelemetry(snapshot.val())
        if (parsed === null) {
          setTelemetry(null)
          setDataState('connection-error')
          setIsStale(true)
          return
        }

        const signature = `${parsed.seq}:${parsed.uptimeMs}`
        if (signature !== lastSequenceSignature.current) {
          lastSequenceSignature.current = signature
          lastProgressAt.current = receivedAt
          setIsStale(false)
        }

        setTelemetry(parsed)
        setDataState('ready')
      },
      (error) => {
        setTelemetry(null)
        setDataState(
          isPermissionDenied(error) ? 'permission-denied' : 'connection-error',
        )
        setIsStale(true)
      },
    )

    return unsubscribe
  }, [user.uid])

  useEffect(() => {
    const timer = window.setInterval(() => {
      const progressAt = lastProgressAt.current
      setIsStale(
        progressAt === null || Date.now() - progressAt > STALE_AFTER_MS,
      )
    }, 1_000)

    return () => window.clearInterval(timer)
  }, [])

  const metrics = useMemo<MetricCardData[]>(() => {
    if (telemetry === null) return []
    const activePower = formatPower(telemetry.metrics.activePower, 'W')
    const apparentPower = formatPower(telemetry.metrics.apparentPower, 'VA')
    const reactivePower = formatPower(telemetry.metrics.reactivePower, 'var')

    return [
      { label: 'RMS voltage', value: formatNumber(telemetry.metrics.vRms, 1), unit: 'V', group: 'voltage' },
      { label: 'RMS current', value: formatNumber(telemetry.metrics.iRms), unit: 'A', group: 'current' },
      { label: 'Frequency', value: formatNumber(telemetry.metrics.frequency), unit: 'Hz', group: 'quality' },
      { label: 'Power factor', value: formatNumber(telemetry.metrics.powerFactor, 3), unit: 'PF', group: 'quality' },
      { label: 'Active power', value: activePower[0], unit: activePower[1], group: 'power' },
      { label: 'Apparent power', value: apparentPower[0], unit: apparentPower[1], group: 'power' },
      { label: 'Reactive power', value: reactivePower[0], unit: reactivePower[1], group: 'power' },
      { label: 'Voltage crest factor', value: formatNumber(telemetry.metrics.crestFactorV), unit: 'CF', group: 'voltage' },
      { label: 'Current crest factor', value: formatNumber(telemetry.metrics.crestFactorI), unit: 'CF', group: 'current' },
      { label: 'Swell factor', value: formatNumber(telemetry.metrics.swellFactor), unit: 'SF', group: 'quality' },
      { label: 'Voltage THD', value: formatNumber(telemetry.metrics.thdV, 1), unit: '%', group: 'voltage' },
      { label: 'Current THD', value: formatNumber(telemetry.metrics.thdI, 1), unit: '%', group: 'current' },
    ]
  }, [telemetry])

  const reportedOnline = telemetry?.status.online ?? false
  const effectiveOnline =
    reportedOnline && telemetry?.status.wifiConnected === true && !isStale

  async function handleSignOut() {
    if (auth === null) return
    setIsSigningOut(true)
    try {
      await signOut(auth)
    } finally {
      setIsSigningOut(false)
    }
  }

  return (
    <div className="dashboard-shell">
      <header className="topbar">
        <div className="brand-lockup">
          <span className="brand-mark brand-mark-small" aria-hidden="true">
            PQ
          </span>
          <div>
            <p className="eyebrow">Smart power quality analyzer</p>
            <h1>Live Instrument Console</h1>
          </div>
        </div>
        <nav className="primary-navigation" aria-label="Primary navigation">
          <AppLink href="/" className="navigation-link" ariaCurrent="page">
            Live
          </AppLink>
          <AppLink href="/history" className="navigation-link">
            History
          </AppLink>
        </nav>
        <div className="account-actions">
          <span className="account-email" title={user.email ?? 'Authenticated user'}>
            {user.email ?? 'Authenticated user'}
          </span>
          <button
            className="secondary-button"
            type="button"
            onClick={handleSignOut}
            disabled={isSigningOut}
          >
            {isSigningOut ? 'Signing out…' : 'Sign out'}
          </button>
        </div>
      </header>

      <main className="dashboard-main">
        <section className="status-ribbon" aria-label="Live device status">
          <div className={`health-indicator ${effectiveOnline ? 'health-online' : 'health-offline'}`}>
            <span className="health-dot" aria-hidden="true" />
            <div>
              <span>Device link</span>
              <strong>
                {isStale ? 'Stale / offline' : effectiveOnline ? 'Live' : 'Offline'}
              </strong>
            </div>
          </div>
          <dl className="ribbon-details">
            <div>
              <dt>Device</dt>
              <dd>{telemetry?.deviceId ?? 'PQ-3PH-001'}</dd>
            </div>
            <div>
              <dt>Sequence</dt>
              <dd>{telemetry?.seq.toLocaleString('en-US') ?? '—'}</dd>
            </div>
            <div>
              <dt>Browser update</dt>
              <dd>
                {lastUpdate?.toLocaleTimeString([], {
                  hour: '2-digit',
                  minute: '2-digit',
                  second: '2-digit',
                }) ?? 'Waiting'}
              </dd>
            </div>
          </dl>
        </section>

        {dataState === 'ready' && telemetry !== null ? (
          <>
            <section className="section-block" aria-labelledby="measurements-title">
              <div className="section-heading">
                <div>
                  <p className="eyebrow">Three-phase telemetry</p>
                  <h2 id="measurements-title">Live measurements</h2>
                </div>
                <span className="refresh-note">Refresh target ≤ 500 ms</span>
              </div>
              <div className="metric-grid">
                {metrics.map((metric) => (
                  <MetricCard key={metric.label} metric={metric} />
                ))}
              </div>
            </section>

            <section className="system-panel" aria-labelledby="system-title">
              <div className="section-heading system-heading">
                <div>
                  <p className="eyebrow">Device diagnostics</p>
                  <h2 id="system-title">System state</h2>
                </div>
              </div>
              <dl className="system-grid">
                <div>
                  <dt>Uptime</dt>
                  <dd>{formatUptime(telemetry.uptimeMs)}</dd>
                </div>
                <div>
                  <dt>Wi-Fi</dt>
                  <dd>{telemetry.status.wifiConnected ? 'Connected' : 'Disconnected'}</dd>
                </div>
                <div>
                  <dt>Signal strength</dt>
                  <dd>{telemetry.status.wifiRssi} dBm</dd>
                </div>
                <div>
                  <dt>Data logging</dt>
                  <dd>{telemetry.status.logging ? 'Active' : 'Standby'}</dd>
                </div>
                <div>
                  <dt>Reported status</dt>
                  <dd>{reportedOnline ? 'Online' : 'Offline'}</dd>
                </div>
                <div>
                  <dt>Stream freshness</dt>
                  <dd>{isStale ? 'Over 5 seconds old' : 'Current'}</dd>
                </div>
              </dl>
            </section>
          </>
        ) : (
          <StatePanel
            state={dataState === 'ready' ? 'connection-error' : dataState}
          />
        )}
      </main>

      <footer className="dashboard-footer">
        <span>Authenticated power-quality console</span>
        <span>Historical graphs and exports are validated in this browser</span>
      </footer>
    </div>
  )
}

function NotFoundPage() {
  return (
    <main className="dashboard-main history-main">
      <HistoryStatePanel
        title="Page not found"
        detail="The requested dashboard route does not exist."
        action={(
          <AppLink className="state-link" href="/">
            Return to live telemetry
          </AppLink>
        )}
      />
    </main>
  )
}

function App() {
  const [user, setUser] = useState<User | null>(null)
  const [isAuthLoading, setIsAuthLoading] = useState(true)
  const [sessionError, setSessionError] = useState<string | null>(null)
  const [historyNotice, setHistoryNotice] = useState<string | null>(null)
  const route = useAppRoute()
  const historyRepository = useMemo(
    () => createHistoryRepository(database),
    [],
  )

  useEffect(() => {
    if (route.name !== 'history') setHistoryNotice(null)
  }, [route.name])

  useEffect(() => {
    if (auth === null) {
      setIsAuthLoading(false)
      return
    }

    return onAuthStateChanged(
      auth,
      (nextUser) => {
        setUser(nextUser)
        setSessionError(null)
        setIsAuthLoading(false)
      },
      () => {
        setSessionError('The authentication session could not be restored.')
        setIsAuthLoading(false)
      },
    )
  }, [])

  if (isAuthLoading) {
    return (
      <main className="loading-screen" aria-live="polite">
        <div className="loading-ring" aria-hidden="true" />
        <p>Restoring secure session…</p>
      </main>
    )
  }

  if (user === null) {
    return (
      <LoginScreen
        configurationError={firebaseConfigurationError}
        sessionError={sessionError}
      />
    )
  }

  if (route.name === 'live') {
    return <Dashboard user={user} />
  }

  let content: ReactNode
  if (route.name === 'history') {
    content = (
      <HistoryListPage
        repository={historyRepository}
        successNotice={historyNotice}
      />
    )
  } else if (route.name === 'history-detail') {
    content = (
      <SessionDetailsPage
        repository={historyRepository}
        sessionKey={route.sessionKey}
        onDeleted={(sessionKey) => {
          setHistoryNotice(`Session ${sessionKey.slice(2)} was deleted from cloud history.`)
          navigateTo('/history')
        }}
      />
    )
  } else if (route.name === 'history-graphs') {
    content = (
      <Suspense
        fallback={(
          <main className="dashboard-main history-main">
            <HistoryStatePanel
              title="Opening historical graphs"
              detail="Loading the browser chart workspace. Session chunks remain untouched."
            />
          </main>
        )}
      >
        <SessionGraphsPage
          repository={historyRepository}
          sessionKey={route.sessionKey}
        />
      </Suspense>
    )
  } else {
    content = <NotFoundPage />
  }

  return (
    <AuthenticatedShell user={user} activeRoute="history">
      {content}
    </AuthenticatedShell>
  )
}

export default App
