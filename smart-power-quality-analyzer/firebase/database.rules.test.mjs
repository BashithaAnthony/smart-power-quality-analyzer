import { after, before, beforeEach, test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import {
  assertFails,
  assertSucceeds,
  initializeTestEnvironment,
} from '@firebase/rules-unit-testing'
import { get, ref, remove, set, update } from 'firebase/database'

const PROJECT_ID = 'demo-smart-power-quality-analyzer'
const DEVICE_ID = 'PQ-3PH-001'
const DEVICE_UID = 'QUR81xrmojUZu8Y9pJ6sMqKewo23'
const DASHBOARD_UID = 'dashboard-admin-test'

let testEnvironment

async function seedDatabase() {
  await testEnvironment.withSecurityRulesDisabled(async (context) => {
    await set(ref(context.database()), {
      devices: {
        [DEVICE_ID]: {
          live: { seq: 100, status: { online: true } },
          sessionIndex: {
            s_7: { schemaVersion: 1, state: 'complete', sessionId: '7' },
            s_8: { schemaVersion: 2, state: 'complete', sessionId: '8' },
            s_9: { schemaVersion: 2, state: 'uploading', sessionId: '9' },
          },
          sessionData: {
            s_7: {
              manifest: { schemaVersion: 1, state: 'complete', sessionId: '7' },
              chunks: { '000000': { payload: 'fixture-seven' } },
            },
            s_8: {
              manifest: { schemaVersion: 2, state: 'complete', sessionId: '8' },
              chunks: { '000000': { payload: 'fixture-eight' } },
            },
            s_9: {
              manifest: { schemaVersion: 2, state: 'uploading', sessionId: '9' },
            },
          },
        },
        'PQ-3PH-OTHER': {
          sessionIndex: {
            s_7: { schemaVersion: 1, state: 'complete', sessionId: '7' },
          },
          sessionData: {
            s_7: { manifest: { schemaVersion: 1, state: 'complete' } },
          },
        },
      },
    })
  })
}

function selectedSessionDeletion(database) {
  return update(ref(database, `devices/${DEVICE_ID}`), {
    'sessionIndex/s_7': null,
    'sessionData/s_7': null,
  })
}

before(async () => {
  testEnvironment = await initializeTestEnvironment({
    projectId: PROJECT_ID,
    database: {
      host: '127.0.0.1',
      port: 9000,
      rules: readFileSync(new URL('database.rules.json', import.meta.url), 'utf8'),
    },
  })
})

beforeEach(seedDatabase)

after(async () => {
  await testEnvironment.cleanup()
})

test('unauthenticated deletion is denied', async () => {
  await assertFails(selectedSessionDeletion(
    testEnvironment.unauthenticatedContext().database(),
  ))
})

test('device uploader cannot delete completed sessions', async () => {
  await assertFails(selectedSessionDeletion(
    testEnvironment.authenticatedContext(DEVICE_UID).database(),
  ))
})

test('authenticated dashboard account can delete exactly one selected cloud session', async () => {
  const database = testEnvironment.authenticatedContext(DASHBOARD_UID).database()
  await assertSucceeds(selectedSessionDeletion(database))

  await testEnvironment.withSecurityRulesDisabled(async (context) => {
    const root = context.database()
    assert.equal((await get(ref(root, `devices/${DEVICE_ID}/sessionIndex/s_7`))).exists(), false)
    assert.equal((await get(ref(root, `devices/${DEVICE_ID}/sessionData/s_7`))).exists(), false)
    assert.equal((await get(ref(root, `devices/${DEVICE_ID}/sessionIndex/s_8`))).exists(), true)
    assert.equal((await get(ref(root, `devices/${DEVICE_ID}/sessionData/s_8`))).exists(), true)
  })
})

test('authenticated dashboard account cannot modify uploaded session contents', async () => {
  const database = testEnvironment.authenticatedContext(DASHBOARD_UID).database()
  await assertFails(set(
    ref(database, `devices/${DEVICE_ID}/sessionData/s_7/manifest/state`),
    'uploading',
  ))
  await assertFails(set(
    ref(database, `devices/${DEVICE_ID}/sessionIndex/s_7/state`),
    'uploading',
  ))
})

test('authenticated dashboard account cannot delete an in-progress upload', async () => {
  const database = testEnvironment.authenticatedContext(DASHBOARD_UID).database()
  await assertFails(update(ref(database, `devices/${DEVICE_ID}`), {
    'sessionIndex/s_9': null,
    'sessionData/s_9': null,
  }))
})

test('authenticated dashboard account cannot delete live telemetry', async () => {
  const database = testEnvironment.authenticatedContext(DASHBOARD_UID).database()
  await assertFails(remove(ref(database, `devices/${DEVICE_ID}/live`)))
})

test('authenticated dashboard account cannot delete another device path', async () => {
  const database = testEnvironment.authenticatedContext(DASHBOARD_UID).database()
  await assertFails(remove(ref(database, 'devices/PQ-3PH-OTHER/sessionData/s_7')))
})

test('another authenticated dashboard account uses the same narrow delete permission', async () => {
  const database = testEnvironment.authenticatedContext('dashboard-reader').database()
  await assertSucceeds(selectedSessionDeletion(database))
})
