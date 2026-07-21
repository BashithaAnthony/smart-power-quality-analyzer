#pragma once

#define FIREBASE_PROJECT_ID \
  "smart-power-quality-analyzer-1"

#define FIREBASE_DATABASE_URL \
  "https://smart-power-quality-analyzer-1-default-rtdb.asia-southeast1.firebasedatabase.app"

#define FIREBASE_WEB_API_KEY \
  "YOUR_REAL_FIREBASE_WEB_API_KEY"

#define FIREBASE_DEVICE_EMAIL \
  "YOUR_DEVICE_USER_EMAIL"

#define FIREBASE_DEVICE_PASSWORD \
  "YOUR_DEVICE_USER_PASSWORD"

#define FIREBASE_DEVICE_ID \
  "PQ-3PH-001"

// Set to 0 to disable historical RTDB session synchronization while keeping
// the existing live-telemetry bridge configuration available.
#define FIREBASE_SESSION_SYNC_ENABLED 1

// TEMPORARY DEVELOPMENT TEST ONLY. This disables TLS certificate verification
// and is unsafe for production. Set to 0 and define FIREBASE_ROOT_CA to a
// trusted PEM certificate before production use.
#define FIREBASE_INSECURE_TLS_TEST 1
