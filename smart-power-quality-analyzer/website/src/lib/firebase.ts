import { initializeApp, type FirebaseApp } from 'firebase/app'
import { getAuth, type Auth } from 'firebase/auth'
import { getDatabase, type Database } from 'firebase/database'

const environmentVariables = {
  VITE_FIREBASE_API_KEY: import.meta.env.VITE_FIREBASE_API_KEY,
  VITE_FIREBASE_AUTH_DOMAIN: import.meta.env.VITE_FIREBASE_AUTH_DOMAIN,
  VITE_FIREBASE_DATABASE_URL: import.meta.env.VITE_FIREBASE_DATABASE_URL,
  VITE_FIREBASE_PROJECT_ID: import.meta.env.VITE_FIREBASE_PROJECT_ID,
  VITE_FIREBASE_STORAGE_BUCKET: import.meta.env.VITE_FIREBASE_STORAGE_BUCKET,
  VITE_FIREBASE_MESSAGING_SENDER_ID:
    import.meta.env.VITE_FIREBASE_MESSAGING_SENDER_ID,
  VITE_FIREBASE_APP_ID: import.meta.env.VITE_FIREBASE_APP_ID,
}

const missingVariables = Object.entries(environmentVariables)
  .filter(([, value]) => typeof value !== 'string' || value.trim() === '')
  .map(([key]) => key)

export let firebaseConfigurationError =
  missingVariables.length > 0
    ? `Firebase configuration is incomplete (${missingVariables.join(', ')}).`
    : null

let app: FirebaseApp | null = null
let auth: Auth | null = null
let database: Database | null = null

if (firebaseConfigurationError === null) {
  try {
    app = initializeApp({
      apiKey: environmentVariables.VITE_FIREBASE_API_KEY,
      authDomain: environmentVariables.VITE_FIREBASE_AUTH_DOMAIN,
      databaseURL: environmentVariables.VITE_FIREBASE_DATABASE_URL,
      projectId: environmentVariables.VITE_FIREBASE_PROJECT_ID,
      storageBucket: environmentVariables.VITE_FIREBASE_STORAGE_BUCKET,
      messagingSenderId:
        environmentVariables.VITE_FIREBASE_MESSAGING_SENDER_ID,
      appId: environmentVariables.VITE_FIREBASE_APP_ID,
    })
    auth = getAuth(app)
    database = getDatabase(app)
  } catch {
    firebaseConfigurationError = 'Firebase configuration could not be initialized.'
  }
}

export { app, auth, database }
