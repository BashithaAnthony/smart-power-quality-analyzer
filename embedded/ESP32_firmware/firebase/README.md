# Realtime Database rule deployment

Historical-session deletion follows the dashboard's existing authentication
model: any authenticated dashboard account except the dedicated ESP32 device
UID may delete a complete uploaded session. The permission exists only at the
validated `sessionIndex/s_<id>` and `sessionData/s_<id>` nodes; it does not
grant content updates or writes to live telemetry or another device path.

The website deletes one uploaded session atomically by setting only these two
locations to `null` in one multi-location update:

- `devices/PQ-3PH-001/sessionIndex/<session-key>`
- `devices/PQ-3PH-001/sessionData/<session-key>`

Rules are not deployed automatically. After reviewing and testing them, deploy
manually from the rules directory with the locally installed Firebase CLI:

```text
cd firebase
npx firebase-tools deploy --only database --project smart-power-quality-analyzer-1
```
