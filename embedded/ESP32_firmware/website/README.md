# Power Quality Console

Authenticated React/Vite dashboard for live ESP32 telemetry and validated,
read-only historical-session graphs and waveform exports from Firebase
Realtime Database.

The historical graph view shows session measurement trends together with the
selected packet's H1–H25 voltage harmonic spectrum and H1–H25 current harmonic
spectrum. Raw waveform samples remain available only through the separate Full
Waveforms CSV export.

Sessions uploaded with a valid NTP anchor use actual measurement time on the
trend axis. Older or unanchored sessions use elapsed time from the first
retained packet and are labelled accordingly; upload timestamps are never
treated as measurement timestamps.

## Local development

1. Copy `.env.example` to `.env.local` and enter the Firebase web-app values.
2. Run `npm install`.
3. Run `npm run dev` and open the printed local URL.
4. Sign in with an authorized Firebase email/password account.

Use `VITE_HISTORY_DATA_MODE=mock` for the small deterministic historical
fixture. Firebase Authentication remains required; mock mode changes only the
historical repository and exercises the same manifest, chunk, PQR1, CRC32C,
graph, and waveform CSV pipelines.

## Checks

- `npm run typecheck`
- `npm run lint`
- `npm test`
- `npm run build`

The history routes are `/history`, `/history/:sessionKey`, and
`/history/:sessionKey/graphs`. Production SPA hosting must rewrite those
browser routes to `index.html`.
