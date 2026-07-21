#include "TestConsoleConfig.h"
#include "WiFiManager.h"
#include <TFT_eSPI.h> 
#include <SPI.h>
#include "touch.h"
#include <math.h> // Needed for the sin() function
#include <WiFi.h>
#include "FirebaseBridge.h"
#include "SessionLogger.h"
#include "SessionStorage.h"
#include "SessionUiController.h"
#include "FlashRecordCodec.h"
#include "WallClockService.h"
#include "ProvisioningConfig.h"
#include "AcquisitionDiagnostics.h"
#include "AcquisitionPolicy.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>

// --- UART Pin Definitions for STM32 ---
#define STM_RX_PIN 6 // Connect to STM32 TX
#define STM_TX_PIN 7 // Connect to STM32 RX

// --- Hardware Control Pins ---
#define ENC_A 1
#define ENC_B 2
#define ENC_SW 5
#define BTN_SEL 38
#define BTN_BACK 39

// Board-level safety output. Software can enforce this only after application
// startup; use an external pull-down when GPIO15 must also be LOW at power-up.
constexpr gpio_num_t HARDWARE_HOLD_LOW_GPIO = GPIO_NUM_15;
static_assert(static_cast<int>(HARDWARE_HOLD_LOW_GPIO) == 15,
              "The board hold-low output must remain GPIO15");
static_assert(static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != STM_RX_PIN &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != STM_TX_PIN &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != ENC_A &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != ENC_B &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != ENC_SW &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != BTN_SEL &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != BTN_BACK,
              "GPIO15 must not conflict with UART or user-input pins");
static_assert(static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TOUCH_FT6336_SCL &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) !=
                      TOUCH_FT6336_SDA &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) !=
                      TOUCH_FT6336_INT &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) !=
                      TOUCH_FT6336_RST,
              "GPIO15 must not conflict with touch-controller pins");
#if defined(TFT_MOSI) && defined(TFT_MISO) && defined(TFT_SCLK) && \
    defined(TFT_CS) && defined(TFT_DC) && defined(TFT_RST)
static_assert(static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TFT_MOSI &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TFT_MISO &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TFT_SCLK &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TFT_CS &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TFT_DC &&
                  static_cast<int>(HARDWARE_HOLD_LOW_GPIO) != TFT_RST,
              "GPIO15 must not conflict with the active TFT pin map");
#endif

// --- UART & Packet Definitions ---
#define SAMPLES 1024
#define HARMONICS 25

typedef struct __attribute__((packed)) {
    uint32_t start_byte;
    uint32_t seq;
    int16_t  voltage[SAMPLES];
    int16_t  current[SAMPLES];
    float    v_rms, i_rms;
    float    frequency;
    float    power_factor;
    float    active_power, apparent_power, reactive_power;
    float    crest_factor_v, crest_factor_i;
    float    swell_factor;
    float    thd_v, thd_i;
    float    harmonics_v[HARMONICS];
    float    harmonics_i[HARMONICS];
    uint16_t checksum;
} WaveformPacket_t;

static_assert(sizeof(WaveformPacket_t) == SESSION_PACKET_BYTES,
              "WaveformPacket_t must remain exactly 4354 bytes");

WaveformPacket_t pkt;
FirebaseBridge firebaseBridge;
SessionLogger sessionLogger;
SessionStorage sessionStorage;
WallClockService wallClockService;
SessionUiController sessionUiController;
bool gpio15HoldLowConfigured = false;

// --- Timing Globals ---
uint32_t lastUIUpdate = 0;
const uint32_t UI_REFRESH_RATE = 500; 
const uint32_t LOG_STORAGE_REFRESH_RATE = 300;
const uint32_t FOOTER_CLOCK_REFRESH_RATE = 1000;

uint32_t lastGraphUpdate = 0;
const uint32_t GRAPH_REFRESH_RATE = 100; 

char systemFooterClockText[64] =
    "Sys Clock:  ---- / -- / --   --:--";
uint32_t lastFooterClockRefreshMs = 0U;
bool footerClockCacheInitialized = false;
bool footerClockHasValidTime = false;

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
bool uartRxBufferInternal = false;
uint32_t uartRxBufferConfiguredBytes = 0U;
uint32_t uartRxInternalHeapDeltaBytes = 0U;
#endif

// --- UI Globals ---
TFT_eSPI my_lcd = TFT_eSPI();

// --- Colors ---
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define YELLOW  0xFFE0
#define ORANGE  0xFDA0
#define MAGENTA 0xF81F
#define WHITE   0xFFFF
#define GRAY    0x8410
#define DARK_BG 0x18E3 
#define IND_BG      0x0000 
#define IND_TEXT    0xFFFF 
#define IND_ACCENT  0x07FF 
#define IND_WARN    0xFD20 

// --- Screen States ---
enum ScreenState {
  SPLASH_SCREEN,
  SETUP_DATETIME_SCREEN,  // Reserved legacy value; no runtime route enters it.
  HOME_SCREEN,
  MENU_SCREEN,
  MEASUREMENTS_SCREEN,
  GRAPHS_MENU,
  TIME_DOMAIN_MENU,
  FREQUENCY_DOMAIN_MENU,
  GRAPH_VOLTAGE,
  GRAPH_CURRENT,
  LOG_SCREEN,
  SETTINGS_SCREEN,
  WIFI_MENU_SCREEN,
  ABOUT_SCREEN
};

ScreenState currentState = SPLASH_SCREEN;
int currentMeasurementPage = 1; 
bool isLogging = false; 
bool isWifiConnected = false;
bool isTimeDomain = true; 
bool screenNeedsUpdate = true; 

// Settings Variables
int logIntervals[] = {1, 5, 10, 60}; 
int currentLogIntervalIdx = 0; 
int encoderSensitivity = 4; // Standard detent ticks per movement

// --- Navigation Globals ---
struct FocusRect {
    int16_t x, y, w, h;
};
FocusRect currentFocusElements[10];
int currentFocusCount = 0;
int currentFocusIndex = 0;
int lastFocusIndex = -1;
volatile int encoderDelta = 0;

// --- Forward Declarations ---
void updateHomeScreenData();
void updateMeasurementsScreenData();
void updateStaticGraphRealTime(bool isVoltage);
void updateFFTGraphRealTime(bool isVoltage);
void defineFocusElements();
void drawFocus();
void drawSystemFooter();
bool refreshSystemFooterClockCache(bool force);
void serviceSystemFooterClock();
void drawStartupConnectionStatus(const char* message, uint16_t color);
void drawStartupProvisioningInfo();
void updateLogStorageIndicators(const SessionUiSnapshot& ui, bool force);
void updateLogStatusIndicator(const SessionUiSnapshot& ui, bool force);
void handleInteraction(int16_t x, int16_t y);
bool startLoggingSession();
bool requestLoggingStop();
bool requestSessionSynchronization();
SessionUiClearAction requestRetainedSessionClear();
void synchronizeLoggingState();
bool configureGpio15HeldLow();
const char* sessionUiMessageText(SessionUiMessage message);
uint16_t sessionUiMessageColor(SessionUiMessage message);

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
void processSessionStorageTestConsole();
void printSessionStorageTestStatus();
void printSessionStorageTestHelp();
void reportSessionStorageTestStateChange();
#if SESSION_SYNC_TEST_CONSOLE
void printSessionSyncTestStatus();
#endif
#endif

// --- Hardware Interrupts & Inputs ---
void encoderISR() {
    static uint8_t old_AB = 0;
    static const int8_t enc_states[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    
    uint8_t A = digitalRead(ENC_A);
    uint8_t B = digitalRead(ENC_B);
    
    old_AB <<= 2;
    old_AB |= ((A << 1) | B);
    encoderDelta += enc_states[(old_AB & 0x0f)];
}

bool isSelectPressed() {
    static unsigned long lastPress = 0;
    bool stateSW = digitalRead(ENC_SW);
    bool stateBtn = digitalRead(BTN_SEL);
    if ((stateSW == LOW || stateBtn == LOW) && (millis() - lastPress > 250)) {
        lastPress = millis();
        return true;
    }
    return false;
}

bool isBackPressed() {
    static unsigned long lastPress = 0;
    if (digitalRead(BTN_BACK) == LOW && (millis() - lastPress > 250)) {
        lastPress = millis();
        return true;
    }
    return false;
}

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
void handleStm32UartReceiveError(hardwareSerial_error_t error) {
    if (error == UART_FIFO_OVF_ERROR) {
        AcquisitionDiagnostics::noteUartFifoOverflow();
    } else if (error == UART_BUFFER_FULL_ERROR) {
        AcquisitionDiagnostics::noteUartRingBufferOverflow();
    }
}
#endif

bool configureGpio15HeldLow() {
    // Load the LOW output latch before enabling the driver to minimize any
    // application-controlled HIGH transient, then assert LOW once more.
    const esp_err_t preloadResult =
        gpio_set_level(HARDWARE_HOLD_LOW_GPIO, 0U);
    const esp_err_t directionResult =
        gpio_set_direction(HARDWARE_HOLD_LOW_GPIO, GPIO_MODE_OUTPUT);
    const esp_err_t finalLevelResult =
        gpio_set_level(HARDWARE_HOLD_LOW_GPIO, 0U);
    return preloadResult == ESP_OK && directionResult == ESP_OK &&
           finalLevelResult == ESP_OK;
}

bool startLoggingSession() {
    const SessionUiRequestDecision decision = sessionUiController.requestStart(
        static_cast<uint32_t>(logIntervals[currentLogIntervalIdx]));
    screenNeedsUpdate = true;
    return decision == SessionUiRequestDecision::Allowed;
}

bool requestLoggingStop() {
    const SessionUiRequestDecision decision =
        sessionUiController.requestStop();
    const bool accepted = decision == SessionUiRequestDecision::Allowed ||
                          decision == SessionUiRequestDecision::Idempotent;
    if (accepted) {
        isLogging = false;
    }
    screenNeedsUpdate = true;
    return accepted;
}

bool requestSessionSynchronization() {
    const SessionUiRequestDecision decision =
        sessionUiController.requestSync();
    screenNeedsUpdate = true;
    return decision == SessionUiRequestDecision::Allowed ||
           decision == SessionUiRequestDecision::Idempotent ||
           decision == SessionUiRequestDecision::AlreadySynchronized;
}

SessionUiClearAction requestRetainedSessionClear() {
    const SessionUiClearAction action =
        sessionUiController.requestClear(millis());
    screenNeedsUpdate = true;
    return action;
}

void synchronizeLoggingState() {
    static bool snapshotInitialized = false;
    static SessionUiSnapshot previous{};
    sessionUiController.service(millis(), currentState == LOG_SCREEN);
    const SessionUiSnapshot current = sessionUiController.getSnapshot();
    const bool loggerActive =
        current.loggerState == SessionLoggerState::Active;
    if (isLogging != loggerActive) {
        isLogging = loggerActive;
    }

    const bool changed = !snapshotInitialized ||
        previous.message != current.message ||
        previous.loggerState != current.loggerState ||
        previous.persistentState != current.persistentState ||
        previous.localSynchronizationState !=
            current.localSynchronizationState ||
        previous.storageAvailable != current.storageAvailable ||
        previous.recoveryBlocked != current.recoveryBlocked ||
        previous.uploadedRecords != current.uploadedRecords ||
        previous.currentChunk != current.currentChunk ||
        previous.totalChunks != current.totalChunks ||
        previous.clearConfirmationArmed != current.clearConfirmationArmed ||
        previous.clearInProgress != current.clearInProgress ||
        previous.clearCompleted != current.clearCompleted;
    previous = current;
    snapshotInitialized = true;
    if (changed && currentState == LOG_SCREEN) {
        screenNeedsUpdate = true;
    }
    const bool storageEstimatePhase =
        current.loggerState == SessionLoggerState::Starting ||
        current.loggerState == SessionLoggerState::PreparingStorage ||
        current.loggerState == SessionLoggerState::Active ||
        current.loggerState == SessionLoggerState::Stopping ||
        current.loggerState == SessionLoggerState::Finalizing;
    if (currentState == LOG_SCREEN && !screenNeedsUpdate &&
        storageEstimatePhase) {
        updateLogStorageIndicators(current, false);
        updateLogStatusIndicator(current, false);
    }
}

const char* sessionUiMessageText(SessionUiMessage message) {
    switch (message) {
        case SessionUiMessage::Standby: return "Standby";
        case SessionUiMessage::Starting: return "Starting";
        case SessionUiMessage::PreparingStorage: return "Preparing storage...";
        case SessionUiMessage::Logging: return "Logging";
        case SessionUiMessage::Stopping: return "Stopping";
        case SessionUiMessage::Draining: return "Draining";
        case SessionUiMessage::Finalizing: return "Finalizing";
        case SessionUiMessage::Finalized: return "Finalized";
        case SessionUiMessage::StorageFull: return "Storage full";
        case SessionUiMessage::Recovered: return "Recovered";
        case SessionUiMessage::StartFailed: return "Start failed";
        case SessionUiMessage::StopFailed: return "Stop failed";
        case SessionUiMessage::WaitingForWifi: return "Waiting for Wi-Fi";
        case SessionUiMessage::Uploading: return "Uploading";
        case SessionUiMessage::Verifying: return "Verifying";
        case SessionUiMessage::Synced: return "Synced";
        case SessionUiMessage::SyncFailed: return "Sync failed";
        case SessionUiMessage::ClearConfirmation: return "Press Clear All again";
        case SessionUiMessage::ClearUnsyncedConfirmation: return "Unsynced data will be deleted";
        case SessionUiMessage::Clearing: return "Clearing...";
        case SessionUiMessage::StorageCleared: return "Storage cleared";
        case SessionUiMessage::ClearFailed: return "Clear failed";
        case SessionUiMessage::StorageError: return "Storage error";
        default: return "Standby";
    }
}

uint16_t sessionUiMessageColor(SessionUiMessage message) {
    switch (message) {
        case SessionUiMessage::Logging:
        case SessionUiMessage::Finalized:
        case SessionUiMessage::Synced:
        case SessionUiMessage::StorageCleared:
            return TFT_GREEN;
        case SessionUiMessage::Starting:
        case SessionUiMessage::PreparingStorage:
        case SessionUiMessage::Stopping:
        case SessionUiMessage::Draining:
        case SessionUiMessage::Finalizing:
        case SessionUiMessage::Recovered:
        case SessionUiMessage::WaitingForWifi:
        case SessionUiMessage::Verifying:
        case SessionUiMessage::ClearConfirmation:
        case SessionUiMessage::ClearUnsyncedConfirmation:
        case SessionUiMessage::Clearing:
            return TFT_ORANGE;
        case SessionUiMessage::Uploading:
            return TFT_CYAN;
        case SessionUiMessage::StartFailed:
        case SessionUiMessage::StopFailed:
        case SessionUiMessage::SyncFailed:
        case SessionUiMessage::ClearFailed:
        case SessionUiMessage::StorageError:
        case SessionUiMessage::StorageFull:
            return TFT_RED;
        case SessionUiMessage::Standby:
        default:
            return TFT_DARKGREY;
    }
}

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
const char* sessionLoggerStateText(SessionLoggerState state) {
    switch (state) {
        case SessionLoggerState::Disabled: return "Disabled";
        case SessionLoggerState::Idle: return "Idle";
        case SessionLoggerState::Starting: return "Starting";
        case SessionLoggerState::PreparingStorage: return "PreparingStorage";
        case SessionLoggerState::Active: return "Active";
        case SessionLoggerState::Stopping: return "Stopping";
        case SessionLoggerState::Finalizing: return "Finalizing";
        case SessionLoggerState::Finalized: return "Finalized";
        case SessionLoggerState::RecoveredIncomplete: return "RecoveredIncomplete";
        case SessionLoggerState::Clearing: return "Clearing";
        case SessionLoggerState::Rescanning: return "Rescanning";
        case SessionLoggerState::ErrorIncomplete: return "ErrorIncomplete";
        case SessionLoggerState::Error: return "Error";
        default: return "Unknown";
    }
}

const char* persistentSessionStateText(PersistentSessionState state) {
    switch (state) {
        case PersistentSessionState::Empty: return "Empty";
        case PersistentSessionState::Active: return "Active";
        case PersistentSessionState::Stopping: return "Stopping";
        case PersistentSessionState::Finalized: return "Finalized";
        case PersistentSessionState::RecoveredIncomplete: return "RecoveredIncomplete";
        case PersistentSessionState::ErrorIncomplete: return "ErrorIncomplete";
        default: return "Unknown";
    }
}

void printSessionStorageTestHelp() {
    Serial.println("Stage 3 storage test console:");
    Serial.println("  1/5/0/6 = select 1/5/10/60-second flush interval");
    Serial.println("  L = start, S = stop and drain, P = status");
    Serial.println("  C = arm retained-session clear, Y = confirm clear");
    Serial.println("  V = validate/re-scan retained storage, H = help");
#if SESSION_SYNC_TEST_CONSOLE
    Serial.println("Stage 4 session sync test console:");
    Serial.println("  U = start/resume upload, X = cancel upload, Q = sync status");
#endif
}

#if SESSION_SYNC_TEST_CONSOLE
bool sessionSyncOperationActive(SessionSyncState state) {
    return state != SessionSyncState::Disabled &&
           state != SessionSyncState::Idle &&
           state != SessionSyncState::Cancelled &&
           state != SessionSyncState::Complete &&
           state != SessionSyncState::Error;
}

void printSessionSyncTestStatus() {
    const SessionSyncStatus sync = firebaseBridge.getSessionSyncStatus();
    const SessionStorageStatus storage = sessionStorage.getStatus();

    Serial.println("Stage 4 sync status:");
    Serial.printf("  state/session key: %u/%s\n",
                  static_cast<unsigned>(sync.state),
                  sync.sessionKey[0] != '\0' ? sync.sessionKey : "none");
    Serial.printf("  chunks next/total, records uploaded/retained: %u/%u, %llu/%llu\n",
                  static_cast<unsigned>(sync.nextChunk),
                  static_cast<unsigned>(sync.chunkCount),
                  static_cast<unsigned long long>(sync.uploadedRecords),
                  static_cast<unsigned long long>(sync.retainedRecords));
    Serial.printf("  retries/HTTP/error/local sync: %u/%d/%u/%u\n",
                  static_cast<unsigned>(sync.retryCount),
                  sync.lastHttpStatus,
                  static_cast<unsigned>(sync.lastError),
                  static_cast<unsigned>(sync.localSynchronizationState));
    Serial.printf("  Wi-Fi/authenticated/reader/cancel/buffers: %s/%s/%s/%s/%s\n",
                  sync.wifiConnected ? "yes" : "no",
                  sync.firebaseAuthenticated ? "yes" : "no",
                  sync.readerOpen ? "yes" : "no",
                  sync.cancellationRequested ? "yes" : "no",
                  sync.psramBuffersAllocated ? "yes" : "no");
    Serial.printf("  storage recovery blocked/persistent state: %s/%u\n",
                  storage.recoveryBlocked ? "yes" : "no",
                  static_cast<unsigned>(storage.persistentSessionState));
}
#endif

void printSessionStorageTestStatus() {
    const SessionLoggerStatus logger = sessionLogger.getStatus();
    const SessionStorageStatus storage = sessionStorage.getStatus();
    const WallClockServiceStatus wallClock = wallClockService.getStatus();
    const SessionUiSnapshot ui = sessionUiController.getSnapshot();
    const SessionFlushDiagnostics flush =
        sessionLogger.getFlushDiagnostics();
    const AcquisitionDiagnosticsSnapshot acquisition =
        AcquisitionDiagnostics::getSnapshot();

    Serial.println("Stage 3 status:");
    Serial.print("  runtime state: ");
    Serial.println(sessionLoggerStateText(logger.state));
    Serial.print("  persistent state: ");
    Serial.println(persistentSessionStateText(storage.persistentSessionState));
    Serial.printf("  clear armed/in progress/completed/succeeded: %s/%s/%s/%s\n",
                  ui.clearConfirmationArmed ? "yes" : "no",
                  ui.clearInProgress ? "yes" : "no",
                  ui.clearCompleted ? "yes" : "no",
                  ui.clearSucceeded ? "yes" : "no");
    Serial.print("  initialized/storage available: ");
    Serial.print(logger.initialized ? "yes" : "no");
    Serial.print("/");
    Serial.println(logger.storageAvailable ? "yes" : "no");
    Serial.printf("  prepared/recovery blocked/reader open: %s/%s/%s\n",
                  storage.prepared ? "yes" : "no",
                  storage.recoveryBlocked ? "yes" : "no",
                  storage.readerOpen ? "yes" : "no");
    Serial.printf(
        "  preparation sectors/segments, ready/capacity stop: %u/%u, %u/%u, %s/%s\n",
        static_cast<unsigned>(storage.preparationSectorsCompleted),
        static_cast<unsigned>(storage.preparationSectorsTotal),
        static_cast<unsigned>(storage.preparationSegmentsCompleted),
        static_cast<unsigned>(storage.preparationSegmentsTotal),
        storage.dataAreaPrepared ? "yes" : "no",
        logger.storageCapacityReached ? "yes" : "no");
    Serial.printf("  session/interval: %llu/%u s\n",
                  static_cast<unsigned long long>(logger.sessionId),
                  static_cast<unsigned>(logger.selectedIntervalSeconds));
    Serial.printf("  wall clock configured/valid/current UTC ms: %s/%s/%llu\n",
                  wallClock.sntpConfigured ? "yes" : "no",
                  wallClock.systemClockValid ? "yes" : "no",
                  static_cast<unsigned long long>(wallClock.currentUnixEpochMs));
    Serial.printf("  GPIO15 configured/intended/readback: %s/LOW/%s\n",
                  gpio15HoldLowConfigured ? "yes" : "no",
                  gpio_get_level(HARDWARE_HOLD_LOW_GPIO) == 0
                      ? "LOW"
                      : "HIGH");
    Serial.printf("  start anchor valid/epoch/uptime/boot: %s/%llu/%llu/%u\n",
                  storage.startWallClockValid ? "yes" : "no",
                  static_cast<unsigned long long>(storage.startWallClockUnixMs),
                  static_cast<unsigned long long>(storage.startUptimeUs),
                  static_cast<unsigned>(storage.bootId));
    Serial.printf("  end anchor valid/epoch/uptime: %s/%llu/%llu\n",
                  storage.endWallClockValid ? "yes" : "no",
                  static_cast<unsigned long long>(storage.endWallClockUnixMs),
                  static_cast<unsigned long long>(storage.endUptimeUs));
    Serial.printf("  FIFO occupancy/high-water/capacity: %u/%u/%u\n",
                  static_cast<unsigned>(logger.fifoOccupancy),
                  static_cast<unsigned>(logger.fifoHighWaterMark),
                  static_cast<unsigned>(logger.fifoCapacity));
    Serial.printf("  offered/accepted/total stored/retained/overwritten/dropped: %llu/%llu/%llu/%llu/%llu/%llu\n",
                  static_cast<unsigned long long>(logger.validPacketsOffered),
                  static_cast<unsigned long long>(logger.acceptedPacketCount),
                  static_cast<unsigned long long>(storage.totalStoredRecords),
                  static_cast<unsigned long long>(storage.retainedRecordCount),
                  static_cast<unsigned long long>(storage.overwrittenRecordCount),
                  static_cast<unsigned long long>(logger.droppedPacketCount));
    Serial.printf("  invalid length/first seq/last seq: %llu/%u/%u\n",
                  static_cast<unsigned long long>(logger.invalidLengthRejectionCount),
                  static_cast<unsigned>(logger.firstStm32Sequence),
                  static_cast<unsigned>(logger.lastStm32Sequence));
    Serial.printf("  first/last retained/next logical index: %llu/%llu/%llu\n",
                  static_cast<unsigned long long>(storage.firstRetainedLogicalIndex),
                  static_cast<unsigned long long>(storage.lastRetainedLogicalIndex),
                  static_cast<unsigned long long>(storage.nextLogicalRecordIndex));
    Serial.printf("  metadata A/B/copy/generation: %s/%s/%u/%llu\n",
                  storage.metadataAValid ? "valid" : "invalid",
                  storage.metadataBValid ? "valid" : "invalid",
                  static_cast<unsigned>(storage.selectedMetadataCopy),
                  static_cast<unsigned long long>(storage.selectedMetadataGeneration));
    Serial.printf("  erase/write/read failures: %llu/%llu/%llu\n",
                  static_cast<unsigned long long>(logger.flashEraseFailureCount),
                  static_cast<unsigned long long>(logger.flashWriteFailureCount),
                  static_cast<unsigned long long>(logger.flashReadFailureCount));
    Serial.printf("  metadata/segment/record/codec validation failures: %llu/%llu/%llu/%llu\n",
                  static_cast<unsigned long long>(storage.metadataValidationFailureCount),
                  static_cast<unsigned long long>(storage.segmentValidationFailureCount),
                  static_cast<unsigned long long>(storage.recordValidationFailureCount),
                  static_cast<unsigned long long>(storage.codecValidationFailureCount));
    Serial.printf("  current segment/slot, oldest segment/slot: %u/%u, %u/%u\n",
                  static_cast<unsigned>(storage.currentDataSegment),
                  static_cast<unsigned>(storage.currentSlotInSegment),
                  static_cast<unsigned>(storage.oldestPhysicalSegment),
                  static_cast<unsigned>(storage.oldestPhysicalSlot));
    Serial.printf("  next segment sequence/offset: %llu/0x%08lX\n",
                  static_cast<unsigned long long>(storage.nextSegmentSequence),
                  static_cast<unsigned long>(storage.nextPartitionRelativeWriteOffset));
    Serial.printf("  truncated/finalized/stop drain: %s/%s/%s\n",
                  storage.storageTruncated ? "yes" : "no",
                  storage.finalized ? "yes" : "no",
                  logger.stopDrainComplete ? "yes" : "no");
    Serial.printf("  recovery/recovered/counters partial/gap: %s/%s/%s/%s\n",
                  storage.recoveryPerformed ? "yes" : "no",
                  storage.recoveredInterrupted ? "yes" : "no",
                  storage.countersPartial ? "yes" : "no",
                  storage.corruptionOrGap ? "yes" : "no");
    Serial.printf("  producer in flight/valid segments/erased segments: %u/%u/%u\n",
                  static_cast<unsigned>(logger.producerInFlight),
                  static_cast<unsigned>(storage.validSegmentCount),
                  static_cast<unsigned>(storage.erasedSegmentCount));
    Serial.printf("  logger/storage/codec/format/esp error: %u/%u/%u/%u/%ld\n",
                  static_cast<unsigned>(logger.lastError),
                  static_cast<unsigned>(storage.lastError),
                  static_cast<unsigned>(storage.lastCodecError),
                  static_cast<unsigned>(storage.lastFormatError),
                  static_cast<long>(storage.lastEspError));
    Serial.printf(
        "  UART RX requested/internal heap delta/internal: %u/%u/%s bytes\n",
        static_cast<unsigned>(uartRxBufferConfiguredBytes),
        static_cast<unsigned>(uartRxInternalHeapDeltaBytes),
        uartRxBufferInternal ? "yes" : "no");
    Serial.printf(
        "  flush active/success/start/end/duration us: %s/%s/%llu/%llu/%llu\n",
        flush.flushActive ? "yes" : "no",
        flush.lastFlushSuccessful ? "yes" : "no",
        static_cast<unsigned long long>(flush.flushStartTimestampUs),
        static_cast<unsigned long long>(flush.flushEndTimestampUs),
        static_cast<unsigned long long>(flush.flushDurationUs));
    Serial.printf(
        "  flush requested/written/segments erased: %u/%u/%u\n",
        static_cast<unsigned>(flush.recordsRequested),
        static_cast<unsigned>(flush.recordsSuccessfullyWritten),
        static_cast<unsigned>(flush.segmentsErased));
    Serial.printf(
        "  maximum write/erase duration us: %llu/%llu\n",
        static_cast<unsigned long long>(flush.maximumWriteDurationUs),
        static_cast<unsigned long long>(flush.maximumEraseDurationUs));
    Serial.printf("  UART bytes available before/after flush: %u/%u\n",
                  static_cast<unsigned>(flush.uartBytesAvailableBefore),
                  static_cast<unsigned>(flush.uartBytesAvailableAfter));
    Serial.printf(
        "  UART FIFO/ring overflow, checksum/resync/sequence gaps: %llu/%llu/%llu/%llu/%llu\n",
        static_cast<unsigned long long>(flush.uartFifoOverflowCount),
        static_cast<unsigned long long>(flush.uartRingBufferOverflowCount),
        static_cast<unsigned long long>(flush.packetChecksumFailureCount),
        static_cast<unsigned long long>(flush.packetResynchronizationCount),
        static_cast<unsigned long long>(flush.packetSequenceGapCount));
    Serial.printf(
        "  acquisition valid/submitted/accepted/rejected/live: %llu/%llu/%llu/%llu/%llu\n",
        static_cast<unsigned long long>(
            acquisition.checksumValidUartPackets),
        static_cast<unsigned long long>(
            acquisition.loggerSubmissionAttempts),
        static_cast<unsigned long long>(
            acquisition.loggerAcceptedPackets),
        static_cast<unsigned long long>(
            acquisition.loggerRejectedPackets),
        static_cast<unsigned long long>(
            acquisition.liveTelemetrySubmissions));
    Serial.printf(
        "  session acquisition valid/submitted/accepted/rejected: %llu/%llu/%llu/%llu\n",
        static_cast<unsigned long long>(
            acquisition.sessionChecksumValidPackets),
        static_cast<unsigned long long>(
            acquisition.sessionSubmissionAttempts),
        static_cast<unsigned long long>(
            acquisition.sessionAcceptedPackets),
        static_cast<unsigned long long>(
            acquisition.sessionRejectedPackets));
    Serial.printf(
        "  acquisition invariants valid=submitted/submitted=accepted+rejected: %s/%s\n",
        acquisition.checksumValidUartPackets ==
                acquisition.loggerSubmissionAttempts
            ? "ok"
            : "MISMATCH",
        acquisition.loggerSubmissionAttempts ==
                acquisition.loggerAcceptedPackets +
                    acquisition.loggerRejectedPackets
            ? "ok"
            : "MISMATCH");
    Serial.printf(
        "  erase ops preparing/active/stopping/finalizing: %llu/%llu/%llu/%llu\n",
        static_cast<unsigned long long>(
            acquisition.eraseWhilePreparingCount),
        static_cast<unsigned long long>(
            acquisition.eraseWhileActiveCount),
        static_cast<unsigned long long>(
            acquisition.eraseWhileStoppingCount),
        static_cast<unsigned long long>(
            acquisition.eraseWhileFinalizingCount));
    Serial.printf(
        "  write ops preparing/active/stopping/finalizing: %llu/%llu/%llu/%llu\n",
        static_cast<unsigned long long>(
            acquisition.writeWhilePreparingCount),
        static_cast<unsigned long long>(
            acquisition.writeWhileActiveCount),
        static_cast<unsigned long long>(
            acquisition.writeWhileStoppingCount),
        static_cast<unsigned long long>(
            acquisition.writeWhileFinalizingCount));
    Serial.printf(
        "  max erase/write us, valid packets during all erase/write: %llu/%llu, %llu/%llu\n",
        static_cast<unsigned long long>(
            acquisition.maximumEraseDurationUs),
        static_cast<unsigned long long>(
            acquisition.maximumWriteDurationUs),
        static_cast<unsigned long long>(
            acquisition.checksumValidDuringEraseOperations),
        static_cast<unsigned long long>(
            acquisition.checksumValidDuringWriteOperations));
    Serial.printf(
        "  erase/write operations with no acquisition progress: %llu/%llu\n",
        static_cast<unsigned long long>(
            acquisition.eraseOperationsWithNoAcquisitionProgress),
        static_cast<unsigned long long>(
            acquisition.writeOperationsWithNoAcquisitionProgress));
    Serial.printf(
        "  last erase phase/start/end/valid start/end: %u/%llu/%llu/%llu/%llu\n",
        static_cast<unsigned>(acquisition.lastErasePhase),
        static_cast<unsigned long long>(acquisition.lastEraseStartUs),
        static_cast<unsigned long long>(acquisition.lastEraseEndUs),
        static_cast<unsigned long long>(
            acquisition.lastEraseStartChecksumValid),
        static_cast<unsigned long long>(
            acquisition.lastEraseEndChecksumValid));
    Serial.printf(
        "  last write phase/start/end/valid start/end: %u/%llu/%llu/%llu/%llu\n",
        static_cast<unsigned>(acquisition.lastWritePhase),
        static_cast<unsigned long long>(acquisition.lastWriteStartUs),
        static_cast<unsigned long long>(acquisition.lastWriteEndUs),
        static_cast<unsigned long long>(
            acquisition.lastWriteStartChecksumValid),
        static_cast<unsigned long long>(
            acquisition.lastWriteEndChecksumValid));
    Serial.printf(
        "  flush sequence gaps/last before/first after: %llu/%s%u/%s%u\n",
        static_cast<unsigned long long>(flush.sequenceGapsDuringFlush),
        flush.hasLastSequenceBeforeFlush ? "" : "n/a ",
        static_cast<unsigned>(flush.lastSequenceBeforeFlush),
        flush.hasFirstSequenceAfterFlush ? "" : "n/a ",
        static_cast<unsigned>(flush.firstSequenceAfterFlush));
    Serial.printf(
        "  invariant accepted/stored/FIFO/in-flight: %llu/%llu/%llu/%llu (%s)\n",
        static_cast<unsigned long long>(flush.acceptedRecords),
        static_cast<unsigned long long>(flush.successfullyWrittenRecords),
        static_cast<unsigned long long>(flush.fifoPendingRecords),
        static_cast<unsigned long long>(flush.workerInFlightRecords),
        flush.acceptedInvariantSatisfied ? "ok" : "MISMATCH");
    Serial.printf(
        "  invariant offered/stored/dropped/FIFO/in-flight: %llu/%llu/%llu/%llu/%llu (%s)\n",
        static_cast<unsigned long long>(flush.validPacketsOffered),
        static_cast<unsigned long long>(flush.successfullyWrittenRecords),
        static_cast<unsigned long long>(flush.loggerDroppedRecordCount),
        static_cast<unsigned long long>(flush.fifoPendingRecords),
        static_cast<unsigned long long>(flush.workerInFlightRecords),
        flush.offeredInvariantSatisfied ? "ok" : "MISMATCH");
}

void processSessionStorageTestConsole() {
    while (Serial.available() > 0) {
        const char command = static_cast<char>(Serial.read());
        switch (command) {
            case '1': currentLogIntervalIdx = 0; break;
            case '5': currentLogIntervalIdx = 1; break;
            case '0': currentLogIntervalIdx = 2; break;
            case '6': currentLogIntervalIdx = 3; break;
            case 'l':
            case 'L':
                Serial.println(startLoggingSession()
                                   ? "Stage 3 session start requested"
                                   : "Stage 3 session start failed");
                break;
            case 's':
            case 'S':
                Serial.println(requestLoggingStop()
                                   ? "Stage 3 Stop requested"
                                   : "Stage 3 Stop request failed");
                break;
            case 'p':
            case 'P':
                printSessionStorageTestStatus();
                break;
            case 'c':
            case 'C': {
                const SessionUiClearAction action =
                    sessionUiController.armClear(millis());
                if (action == SessionUiClearAction::Armed) {
                    Serial.println("Stage 3 retained-session clear armed; press Y within 5 seconds");
                } else if (action == SessionUiClearAction::AlreadyInProgress) {
                    Serial.println("Stage 3 retained-session clear already in progress");
                } else {
                    Serial.println("Stage 3 retained-session clear blocked");
                }
                break;
            }
            case 'y':
            case 'Y': {
                const SessionUiClearAction action =
                    sessionUiController.confirmClear(millis());
                if (action == SessionUiClearAction::NotArmed) {
                    Serial.println("Stage 3 retained-session clear not armed");
                } else if (action == SessionUiClearAction::Requested) {
                    Serial.println("Stage 3 retained-session clear requested");
                } else if (action == SessionUiClearAction::AlreadyInProgress) {
                    Serial.println("Stage 3 retained-session clear already in progress");
                } else {
                    Serial.println("Stage 3 retained-session clear failed");
                }
                break;
            }
            case 'v':
            case 'V':
                sessionUiController.cancelClearConfirmation();
#if SESSION_SYNC_TEST_CONSOLE
                if (sessionSyncOperationActive(
                        firebaseBridge.getSessionSyncStatus().state)) {
                    Serial.println("Stage 3 retained-storage re-scan blocked while sync is active");
                    break;
                }
#endif
                Serial.println(sessionLogger.rescanRetainedStorage()
                                   ? "Stage 3 retained-storage re-scan requested"
                                   : "Stage 3 retained-storage re-scan failed");
                break;
#if SESSION_SYNC_TEST_CONSOLE
            case 'u':
            case 'U':
                Serial.println(requestSessionSynchronization()
                                   ? "Stage 4 sync requested"
                                   : "Stage 4 sync request rejected");
                break;
            case 'x':
            case 'X':
                sessionUiController.requestSyncCancellation();
                Serial.println("Stage 4 upload cancellation requested");
                break;
            case 'q':
            case 'Q':
                printSessionSyncTestStatus();
                break;
#endif
            case 'h':
            case 'H':
                printSessionStorageTestHelp();
                break;
            case '\r':
            case '\n':
            case ' ':
            case '\t':
                break;
            default:
                Serial.println("Unknown Stage 3 command; press H for help");
                break;
        }

        if (command == '1' || command == '5' ||
            command == '0' || command == '6') {
            Serial.printf("Stage 3 interval selected: %u seconds\n",
                          static_cast<unsigned>(
                              logIntervals[currentLogIntervalIdx]));
        }
    }

    reportSessionStorageTestStateChange();
}

void reportSessionStorageTestStateChange() {
    static bool initialized = false;
    static SessionLoggerState previousState = SessionLoggerState::Disabled;
    const SessionLoggerStatus logger = sessionLogger.getStatus();

    if (!initialized) {
        previousState = logger.state;
        initialized = true;
        return;
    }
    if (logger.state == previousState) {
        return;
    }
    const SessionLoggerState oldState = previousState;
    previousState = logger.state;

    if (logger.state == SessionLoggerState::ErrorIncomplete ||
        logger.state == SessionLoggerState::Error) {
        const SessionStorageStatus storage = sessionStorage.getStatus();
        Serial.printf(
            "Stage 3 storage error: logger=%u storage=%u codec=%u format=%u esp=%ld\n",
            static_cast<unsigned>(logger.lastError),
            static_cast<unsigned>(storage.lastError),
            static_cast<unsigned>(storage.lastCodecError),
            static_cast<unsigned>(storage.lastFormatError),
            static_cast<long>(storage.lastEspError));
    } else if ((oldState == SessionLoggerState::Starting ||
                oldState == SessionLoggerState::PreparingStorage) &&
        logger.state == SessionLoggerState::Active) {
        Serial.println("Stage 3 session start succeeded");
    } else if ((oldState == SessionLoggerState::Stopping ||
                oldState == SessionLoggerState::Finalizing) &&
               logger.state == SessionLoggerState::Finalized) {
        Serial.println("Stage 3 Stop drain and finalization completed");
    } else if (oldState == SessionLoggerState::Clearing &&
               logger.state == SessionLoggerState::Idle) {
        Serial.println("Stage 3 retained-session clear completed");
    } else if (oldState == SessionLoggerState::Rescanning) {
        Serial.println("Stage 3 retained-storage re-scan completed");
    }
}
#endif


// --- UART Reading Function ---
void readUART() {
    if (Serial1.available() > 0) {
        uint32_t magic = 0xAA55AA55;
        if (Serial1.find((uint8_t*)&magic, 4)) {
            int payload_size = sizeof(pkt) - 4; 
            int n = Serial1.readBytes(((uint8_t*)&pkt) + 4, payload_size);
            
            if (n == payload_size) {
                pkt.start_byte = magic;
                uint16_t sum = 0;
                for (size_t i = 0; i < sizeof(pkt) - sizeof(pkt.checksum); i++) {
                    sum += ((uint8_t*)&pkt)[i];
                }

                if (sum == pkt.checksum) {
                    // This is the earliest independent acquisition truth
                    // point: a complete packet has passed its wire checksum,
                    // before logger admission or any flash-dependent state.
                    AcquisitionDiagnostics::noteChecksumValidUartPacket(
                        pkt.seq);
                    const bool loggerAccepted =
                        sessionLogger.submitValidatedPacket(
                        reinterpret_cast<const uint8_t*>(&pkt),
                        sizeof(pkt),
                        pkt.seq,
                        static_cast<uint64_t>(esp_timer_get_time()));
                    AcquisitionDiagnostics::noteLoggerSubmissionResult(
                        loggerAccepted);

                    LiveTelemetrySnapshot snapshot = {
                        pkt.seq,
                        millis(),
                        pkt.v_rms,
                        pkt.i_rms,
                        pkt.frequency,
                        pkt.power_factor,
                        pkt.active_power,
                        pkt.apparent_power,
                        pkt.reactive_power,
                        pkt.crest_factor_v,
                        pkt.crest_factor_i,
                        pkt.swell_factor,
                        pkt.thd_v,
                        pkt.thd_i,
                        sessionLogger.isActive(),
                        WiFi.status() == WL_CONNECTED,
                        WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0
                    };
                    firebaseBridge.submit(snapshot);
                    AcquisitionDiagnostics::noteLiveTelemetrySubmission();

                    if (millis() - lastUIUpdate > UI_REFRESH_RATE) {
                        lastUIUpdate = millis();
                        if (currentState == HOME_SCREEN && !screenNeedsUpdate) {
                            updateHomeScreenData();
                        } else if (currentState == MEASUREMENTS_SCREEN && !screenNeedsUpdate) {
                            updateMeasurementsScreenData();
                        }
                    }

                    if (millis() - lastGraphUpdate > GRAPH_REFRESH_RATE) {
                        lastGraphUpdate = millis();
                        if (currentState == GRAPH_VOLTAGE && !screenNeedsUpdate) {
                            if (isTimeDomain) updateStaticGraphRealTime(true);
                            else updateFFTGraphRealTime(true);
                        } 
                        else if (currentState == GRAPH_CURRENT && !screenNeedsUpdate) {
                            if (isTimeDomain) updateStaticGraphRealTime(false);
                            else updateFFTGraphRealTime(false);
                        }
                    }
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
                } else {
                    AcquisitionDiagnostics::notePacketChecksumFailure();
#endif
                }
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
            } else {
                AcquisitionDiagnostics::notePacketSynchronizationLoss();
#endif
            }
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
        } else {
            AcquisitionDiagnostics::notePacketSynchronizationLoss();
#endif
        }
    }
}

// --- Live Data Update Functions (Flicker-Free) ---
void updateHomeScreenData() {
    int cardY = 50;
    int secY = 170;
    my_lcd.setTextColor(WHITE, IND_BG);
    char buffer[16];
    
    sprintf(buffer, "%5.1f", pkt.v_rms);
    my_lcd.drawCentreString(buffer, 82, cardY + 35, 6); 

    sprintf(buffer, "%5.2f", pkt.i_rms);
    my_lcd.drawCentreString(buffer, 240, cardY + 35, 6);

    sprintf(buffer, "%5.2f", pkt.active_power / 1000.0);
    my_lcd.drawCentreString(buffer, 397, cardY + 35, 6);

    sprintf(buffer, "%5.2f Hz", pkt.frequency);
    my_lcd.drawCentreString(buffer, 122, secY + 35, 4);

    sprintf(buffer, "%4.2f", pkt.power_factor);
    my_lcd.drawCentreString(buffer, 357, secY + 35, 4);
}

void updateDataCardValue(int x, int y, int w, int h, String value, uint16_t valColor) {
    my_lcd.fillRect(x + w - 110, y + 25, 105, 30, IND_BG); 
    my_lcd.setTextColor(valColor);
    my_lcd.drawRightString(value, x + w - 15, y + 30, 4);
}

void updateMeasurementsScreenData() {
    int col1 = 10;
    int col2 = 250;
    int cardW = 220;
    int cardH = 65;
    int row1 = 50;
    int row2 = 125;
    int row3 = 200;

    if (currentMeasurementPage == 1) {
        updateDataCardValue(col1, row1, cardW, cardH, String(pkt.v_rms, 1) + " V", TFT_CYAN);
        updateDataCardValue(col2, row1, cardW, cardH, String(pkt.i_rms, 2) + " A", TFT_ORANGE);
        updateDataCardValue(col1, row2, cardW, cardH, String(pkt.frequency, 2) + " Hz", TFT_MAGENTA);
        updateDataCardValue(col2, row2, cardW, cardH, String(pkt.active_power / 1000.0, 2) + " kW", TFT_GREEN);
        updateDataCardValue(col1, row3, cardW, cardH, String(pkt.apparent_power / 1000.0, 2) + " kVA", TFT_PINK);
        updateDataCardValue(col2, row3, cardW, cardH, String(pkt.reactive_power / 1000.0, 2) + " kVAR", TFT_YELLOW);
    } else {
        updateDataCardValue(col1, row1, cardW, cardH, String(pkt.power_factor, 2), TFT_CYAN);
        updateDataCardValue(col2, row1, cardW, cardH, String(pkt.swell_factor, 2), TFT_ORANGE);
        updateDataCardValue(col1, row2, cardW, cardH, String(pkt.thd_v, 1) + " %", TFT_MAGENTA);
        updateDataCardValue(col2, row2, cardW, cardH, String(pkt.thd_i, 1) + " %", TFT_GREEN);
        updateDataCardValue(col1, row3, cardW, cardH, String(pkt.crest_factor_v, 2), TFT_PINK);
        updateDataCardValue(col2, row3, cardW, cardH, String(pkt.crest_factor_i, 2), TFT_YELLOW);
    }
}

void updateStaticGraphRealTime(bool isVoltage) {
  int gX = 15; 
  int gY = 50;
  int gW = 320;
  int gH = 195;
  int midY = gY + (gH / 2);

  my_lcd.fillRect(gX, gY, gW, gH, BLACK);
  my_lcd.drawLine(gX, midY, gX + gW, midY, GRAY);

  uint16_t waveColor = isVoltage ? IND_ACCENT : IND_WARN;
  float scale = isVoltage ? (97.0 / 12000.0) : (97.0 / 4000.0); 

  int prev_x = gX;
  int prev_y = midY;

  for (int i = 0; i < gW; i++) {
    int array_idx = (i * SAMPLES) / gW;
    if (array_idx >= SAMPLES) array_idx = SAMPLES - 1; 

    int16_t raw_val = isVoltage ? pkt.voltage[array_idx] : pkt.current[array_idx];
    int y = midY - (int)(raw_val * scale);
    
    if (y < gY) y = gY;
    if (y > gY + gH - 1) y = gY + gH - 1;

    int x = gX + i;
    if (i > 0) {
      my_lcd.drawLine(prev_x, prev_y, x, y, waveColor);
    }
    prev_x = x;
    prev_y = y;
  }

  int dataX = 407; // Center mapped coordinate
  int startY = 75;
  int spacing = 22;
  my_lcd.setTextColor(WHITE, IND_BG); 
  char buf[20];
  
  if (isVoltage) {
    sprintf(buf, "Vrms: %4.1fV", pkt.v_rms);
    my_lcd.drawCentreString(buf, dataX, startY, 2);
    sprintf(buf, "Irms: %4.2fA", pkt.i_rms);
    my_lcd.drawCentreString(buf, dataX, startY + spacing, 2);
    sprintf(buf, "Hz: %4.1f", pkt.frequency);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 2), 2);
    sprintf(buf, "P.F.: %4.2f", pkt.power_factor);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 3), 2);
    sprintf(buf, "C.F.: %4.2f", pkt.crest_factor_v);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 4), 2);
    sprintf(buf, "THD: %4.1f%%", pkt.thd_v);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 5), 2);
    sprintf(buf, "Pwr: %4.2fkW", pkt.active_power / 1000.0);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 6), 2);
  } else {
    sprintf(buf, "Irms: %4.2fA", pkt.i_rms);
    my_lcd.drawCentreString(buf, dataX, startY, 2);
    sprintf(buf, "Vrms: %4.1fV", pkt.v_rms);
    my_lcd.drawCentreString(buf, dataX, startY + spacing, 2);
    sprintf(buf, "Hz: %4.1f", pkt.frequency);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 2), 2);
    sprintf(buf, "P.F.: %4.2f", pkt.power_factor);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 3), 2);
    sprintf(buf, "C.F.: %4.2f", pkt.crest_factor_i);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 4), 2);
    sprintf(buf, "THD: %4.1f%%", pkt.thd_i);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 5), 2);
    sprintf(buf, "Pwr: %4.2fkW", pkt.active_power / 1000.0);
    my_lcd.drawCentreString(buf, dataX, startY + (spacing * 6), 2);
  }
}

void updateFFTGraphRealTime(bool isVoltage) {
  int gX = 16; 
  int gY = 50;
  int gW = 319;
  int gH = 194; 
  int bottomY = gY + gH;

  my_lcd.fillRect(gX, gY, gW, gH, IND_BG);

  uint16_t barColor = isVoltage ? IND_ACCENT : IND_WARN;
  int totalBars = HARMONICS; 
  
  int barWidth = (gW / totalBars) - 2; 
  if(barWidth < 2) barWidth = 2;
  int barSpacing = 2; 
  float scale = (float)gH / 110.0; 

  for (int i = 0; i < totalBars; i++) {
    float raw_val = isVoltage ? pkt.harmonics_v[i] : pkt.harmonics_i[i];
    
    int barH = (int)(raw_val * scale);
    if (barH > gH) barH = gH;
    if (barH < 0) barH = 0;

    int barX = gX + 2 + (i * (barWidth + barSpacing));
    
    if (barH > 0) {
      my_lcd.fillRect(barX, bottomY - barH, barWidth, barH, barColor);
    }
  }

  int dataX = 407; // Center mapped coordinate
  int startY = 75;
  int textSpacing = 19;
  my_lcd.setTextColor(WHITE, IND_BG); 
  char buf[20];
  
  float thd = isVoltage ? pkt.thd_v : pkt.thd_i;
  sprintf(buf, "THD: %4.1f%%", thd);
  my_lcd.drawCentreString(buf, dataX, startY, 2);

  float h1 = isVoltage ? pkt.harmonics_v[0] : pkt.harmonics_i[0];
  sprintf(buf, "H1: %4.1f%%", h1);
  my_lcd.drawCentreString(buf, dataX, startY + textSpacing, 2);
  
  float h2 = isVoltage ? pkt.harmonics_v[1] : pkt.harmonics_i[1];
  sprintf(buf, "H2: %4.1f%%", h2);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 2), 2);
  
  float h3 = isVoltage ? pkt.harmonics_v[2] : pkt.harmonics_i[2];
  sprintf(buf, "H3: %4.1f%%", h3);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 3), 2);
  
  float h5 = isVoltage ? pkt.harmonics_v[4] : pkt.harmonics_i[4];
  sprintf(buf, "H5: %4.1f%%", h5);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 4), 2);

  float h10 = isVoltage ? pkt.harmonics_v[9] : pkt.harmonics_i[9];
  sprintf(buf, "H10: %4.1f%%", h10);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 5), 2);

  float h15 = isVoltage ? pkt.harmonics_v[14] : pkt.harmonics_i[14];
  sprintf(buf, "H15: %4.1f%%", h15);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 6), 2);

  float h20 = isVoltage ? pkt.harmonics_v[19] : pkt.harmonics_i[19];
  sprintf(buf, "H20: %4.1f%%", h20);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 7), 2);

  float h25 = isVoltage ? pkt.harmonics_v[24] : pkt.harmonics_i[24];
  sprintf(buf, "H25: %4.1f%%", h25);
  my_lcd.drawCentreString(buf, dataX, startY + (textSpacing * 8), 2);
}

// --- Drawing Functions ---

bool refreshSystemFooterClockCache(bool force) {
    const uint32_t nowMs = millis();
    if (!force && footerClockCacheInitialized &&
        static_cast<uint32_t>(nowMs - lastFooterClockRefreshMs) <
            FOOTER_CLOCK_REFRESH_RATE) {
        return false;
    }
    footerClockCacheInitialized = true;
    lastFooterClockRefreshMs = nowMs;

    struct tm sriLankaTime{};
    char updatedText[sizeof(systemFooterClockText)];
    if (wallClockService.readSriLankaLocalTime(sriLankaTime)) {
        snprintf(updatedText,
                 sizeof(updatedText),
                 "Sys Clock:  %04d / %02d / %02d   %02d:%02d",
                 sriLankaTime.tm_year + 1900,
                 sriLankaTime.tm_mon + 1,
                 sriLankaTime.tm_mday,
                 sriLankaTime.tm_hour,
                 sriLankaTime.tm_min);
        footerClockHasValidTime = true;
    } else if (!footerClockHasValidTime) {
        snprintf(updatedText,
                 sizeof(updatedText),
                 "Sys Clock:  ---- / -- / --   --:--");
    } else {
        // Once synchronized, retain the last authoritative display value if
        // the system clock is temporarily unavailable.
        return false;
    }

    if (strcmp(updatedText, systemFooterClockText) == 0) {
        return false;
    }
    snprintf(systemFooterClockText,
             sizeof(systemFooterClockText),
             "%s",
             updatedText);
    return true;
}

void serviceSystemFooterClock() {
    if (refreshSystemFooterClockCache(false) &&
        currentState != SPLASH_SCREEN && !screenNeedsUpdate) {
        drawSystemFooter();
    }
}

void drawSystemFooter() {
    refreshSystemFooterClockCache(false);
    my_lcd.fillRect(0, 300, 480, 20, TFT_DARKGREY);
    my_lcd.setTextColor(WHITE, TFT_DARKGREY);
    my_lcd.drawCentreString(systemFooterClockText, 240, 302, 2);
}

void drawStartupConnectionStatus(const char* message, uint16_t color) {
  my_lcd.fillRect(0, 230, 480, 25, IND_BG);
  my_lcd.setTextColor(color, IND_BG);
  my_lcd.drawCentreString(message != nullptr ? message : "", 240, 235, 2);
}

void drawStartupProvisioningInfo() {
  my_lcd.setTextColor(IND_ACCENT, IND_BG);
  char row[48];
  snprintf(row, sizeof(row), "Device ID: %s", FirebaseBridge::deviceId());
  my_lcd.drawString(row, 10, 48, 2);
  snprintf(
      row, sizeof(row), "Setup Wi-Fi: %s", ProvisioningConfig::kPortalSsid);
  my_lcd.drawString(row, 10, 74, 2);
  const char* password = ProvisioningConfig::kStartupPortalPassword;
  snprintf(row,
           sizeof(row),
           "Password: %s",
           (password != nullptr && password[0] != '\0')
               ? password
               : "Open network");
  my_lcd.drawString(row, 10, 100, 2);
}

void drawSplashScreen() {
  my_lcd.fillScreen(IND_BG);
  int midX = 240; 
  int logoY = 90;
  
  my_lcd.drawCircle(midX, logoY, 50, IND_ACCENT);
  my_lcd.drawCircle(midX, logoY, 49, IND_ACCENT); 
  my_lcd.drawLine(midX - 30, logoY, midX - 10, logoY - 30, IND_WARN);
  my_lcd.drawLine(midX - 10, logoY - 30, midX + 10, logoY + 30, IND_WARN);
  my_lcd.drawLine(midX + 10, logoY + 30, midX + 30, logoY, IND_WARN);
  my_lcd.drawLine(midX - 30, logoY + 1, midX - 10, logoY - 29, IND_WARN);
  my_lcd.drawLine(midX - 10, logoY - 29, midX + 10, logoY + 31, IND_WARN);
  my_lcd.drawLine(midX + 10, logoY + 31, midX + 30, logoY + 1, IND_WARN);

  // Public provisioning details are drawn before WiFiManager can block in
  // autoConnect(), and status updates below never clear this region.
  drawStartupProvisioningInfo();
  
  my_lcd.setTextColor(IND_TEXT, IND_BG);
  my_lcd.drawCentreString("Power Quality", midX, 150, 4);
  my_lcd.drawCentreString("Analyzer", midX, 180, 4);
  
  int barWidth = 260;
  int barX = midX - (barWidth / 2);
  int barY = 260;
  my_lcd.drawRect(barX, barY, barWidth, 15, IND_TEXT);

  drawStartupConnectionStatus("Initializing Core...", IND_TEXT);
  my_lcd.fillRect(barX + 2, barY + 2, (barWidth / 4) * 1 - 4, 11, IND_ACCENT);
  delay(600); 

  drawStartupConnectionStatus("Connecting...", IND_TEXT);
  my_lcd.fillRect(barX + 2, barY + 2, (barWidth / 4) * 2 - 4, 11, IND_ACCENT);
  
  WiFiManager wm;
  wm.setConnectTimeout(10); 
  wm.setConfigPortalTimeout(180); 
  wm.setAPCallback([](WiFiManager*) {
    drawStartupConnectionStatus("Configuration portal active", IND_WARN);
  });
  isWifiConnected = wm.autoConnect(
      ProvisioningConfig::kPortalSsid,
      ProvisioningConfig::kStartupPortalPassword);

  drawStartupConnectionStatus(
      isWifiConnected ? "Connected - Calibrating..."
                      : "Connection failed - Offline mode",
      isWifiConnected ? TFT_GREEN : TFT_RED);
  my_lcd.fillRect(barX + 2, barY + 2, (barWidth / 4) * 3 - 4, 11, IND_ACCENT);
  delay(600); 

  if (isWifiConnected) {
    drawStartupConnectionStatus("System Ready (WiFi Online)", TFT_GREEN);
  } else {
    drawStartupConnectionStatus("System Ready (Offline Mode)", TFT_DARKGREY);
  }
  my_lcd.fillRect(barX + 2, barY + 2, (barWidth / 4) * 4 - 4, 11, IND_ACCENT);
  delay(1000); 
  
  currentState = HOME_SCREEN;
  screenNeedsUpdate = true;
}

void drawSystemHeader(String title) {
    my_lcd.fillRect(0, 0, 480, 40, GRAY); 
    my_lcd.setTextColor(WHITE); 
    my_lcd.drawString(title, 10, 10, 4);
    String wifiText = isWifiConnected ? "WiFi: Connected" : "WiFi: Offline";
    String logText = isLogging ? "Log: On" : "Log: Off";
    my_lcd.drawRightString(wifiText + "  |  " + logText, 470, 12, 2);
}

void drawHomeScreen() {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("Overview");

    int cardY = 50;
    int cardH = 110;
    
    my_lcd.drawRoundRect(10, cardY, 145, cardH, 5, WHITE);
    my_lcd.setTextColor(IND_ACCENT, IND_BG);
    my_lcd.drawCentreString("Voltage", 82, cardY + 10, 2);
    my_lcd.drawCentreString("Volts RMS", 82, cardY + 80, 2);

    my_lcd.drawRoundRect(165, cardY, 150, cardH, 5, WHITE);
    my_lcd.setTextColor(IND_WARN, IND_BG); 
    my_lcd.drawCentreString("Current", 240, cardY + 10, 2);
    my_lcd.drawCentreString("Amps RMS", 240, cardY + 80, 2);

    my_lcd.drawRoundRect(325, cardY, 145, cardH, 5, WHITE);
    my_lcd.setTextColor(GREEN, IND_BG);
    my_lcd.drawCentreString("Active Power", 397, cardY + 10, 2);
    my_lcd.drawCentreString("kW", 397, cardY + 80, 2);

    int secY = 170;
    int secH = 70;
    
    my_lcd.drawRoundRect(10, secY, 225, secH, 5, WHITE);
    my_lcd.setTextColor(YELLOW, IND_BG);
    my_lcd.drawCentreString("Frequency", 122, secY + 10, 2);

    my_lcd.drawRoundRect(245, secY, 225, secH, 5, WHITE);
    my_lcd.setTextColor(MAGENTA, IND_BG);
    my_lcd.drawCentreString("Power Factor", 357, secY + 10, 2);

    int btnY = 245;
    int btnH = 45;
    my_lcd.fillRoundRect(140, btnY, 200, btnH, 5, RED);
    my_lcd.drawRoundRect(140, btnY, 200, btnH, 5, WHITE); 
    my_lcd.setTextColor(WHITE); 
    my_lcd.drawCentreString("Main Menu", 240, btnY + 12, 4);
}

void drawMenuScreen() {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("Main Menu");

    int btnW = 215; 
    int btnH = 65;
    int col1 = 15;  
    int col2 = 250; 
    
    int row1 = 65;
    my_lcd.fillRoundRect(col1, row1, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col1, row1, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Measurements", col1 + (btnW / 2), row1 + 22, 4);
    
    my_lcd.fillRoundRect(col2, row1, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col2, row1, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Graphs", col2 + (btnW / 2), row1 + 22, 4);

    int row2 = 145;
    my_lcd.fillRoundRect(col1, row2, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col1, row2, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Data Log", col1 + (btnW / 2), row2 + 22, 4);

    my_lcd.fillRoundRect(col2, row2, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col2, row2, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Settings", col2 + (btnW / 2), row2 + 22, 4);

    int row3 = 225;
    my_lcd.fillRoundRect(col1, row3, btnW, btnH, 5, TFT_RED);
    my_lcd.drawRoundRect(col1, row3, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Back", col1 + (btnW / 2), row3 + 22, 4);

    my_lcd.fillRoundRect(col2, row3, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col2, row3, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("About / Info", col2 + (btnW / 2), row3 + 22, 4);
}

void drawDataCard(int x, int y, int w, int h, String label) {
    my_lcd.drawRoundRect(x, y, w, h, 3, TFT_LIGHTGREY);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString(label, x + 10, y + 8, 2);
}

void drawMeasurementsScreen() {
    my_lcd.fillScreen(IND_BG);
    my_lcd.fillRect(0, 0, 480, 40, GRAY); 
    my_lcd.setTextColor(WHITE); 
    
    if (currentMeasurementPage == 1) {
        drawSystemHeader("Measurements (1/2)");
    } else {
        drawSystemHeader("Measurements (2/2)");
    }

    int col1 = 10;
    int col2 = 250;
    int cardW = 220;
    int cardH = 65;
    int row1 = 50;
    int row2 = 125;
    int row3 = 200;

    if (currentMeasurementPage == 1) {
        drawDataCard(col1, row1, cardW, cardH, "Voltage (RMS)");
        drawDataCard(col2, row1, cardW, cardH, "Current (RMS)");
        drawDataCard(col1, row2, cardW, cardH, "Frequency");
        drawDataCard(col2, row2, cardW, cardH, "Active Power");
        drawDataCard(col1, row3, cardW, cardH, "Apparent Pwr");
        drawDataCard(col2, row3, cardW, cardH, "Reactive Pwr");
    } 
    else {
        drawDataCard(col1, row1, cardW, cardH, "Power Factor");
        drawDataCard(col2, row1, cardW, cardH, "Swell Factor");
        drawDataCard(col1, row2, cardW, cardH, "THD (Voltage)");
        drawDataCard(col2, row2, cardW, cardH, "THD (Current)");
        drawDataCard(col1, row3, cardW, cardH, "Crest Fac (Voltage)");
        drawDataCard(col2, row3, cardW, cardH, "Crest Fac (Current)");
    }

    int btnY = 255;
    my_lcd.fillRoundRect(10, btnY, 120, 35, 5, TFT_RED);
    my_lcd.drawRoundRect(10, btnY, 120, 35, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Back", 70, btnY + 10, 2);

    my_lcd.fillRoundRect(350, btnY, 120, 35, 5, BLUE);
    my_lcd.drawRoundRect(350, btnY, 120, 35, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    if (currentMeasurementPage == 1) {
        my_lcd.drawCentreString("Next Page >", 410, btnY + 10, 2);
    } else {
        my_lcd.drawCentreString("< Prev Page", 410, btnY + 10, 2);
    }
}

void updateLogStorageIndicators(const SessionUiSnapshot& ui, bool force) {
    static uint64_t lastUsedRecords = UINT64_MAX;
    static uint64_t lastRemainingRecords = UINT64_MAX;
    static uint32_t lastCapacity = UINT32_MAX;
    static uint32_t lastFilledPercent = UINT32_MAX;
    static uint32_t lastRefreshMs = 0U;

    const uint32_t nowMs = millis();
    if (!force &&
        static_cast<uint32_t>(nowMs - lastRefreshMs) <
            LOG_STORAGE_REFRESH_RATE) {
        return;
    }
    if (!force &&
        lastUsedRecords == ui.effectiveUsedRecords &&
        lastRemainingRecords == ui.remainingRecords &&
        lastCapacity == ui.maximumRecords &&
        lastFilledPercent == ui.storageFilledPercent) {
        lastRefreshMs = nowMs;
        return;
    }

    constexpr int barX = 20;
    constexpr int barY = 50;
    constexpr int barW = 440;
    constexpr int barInteriorWidth = barW - 4;
    my_lcd.fillRoundRect(
        barX + 2, barY + 22, barInteriorWidth, 21, 4, IND_BG);
    int fillWidth = 0;
    if (ui.maximumRecords > 0U) {
        fillWidth = static_cast<int>(
            (ui.effectiveUsedRecords *
             static_cast<uint64_t>(barInteriorWidth)) /
            ui.maximumRecords);
        if (fillWidth > barInteriorWidth) fillWidth = barInteriorWidth;
    }
    if (fillWidth > 4) {
        my_lcd.fillRoundRect(
            barX + 2, barY + 22, fillWidth, 21, 4, TFT_ORANGE);
    }

    my_lcd.fillRect(0, barY + 48, 480, 20, IND_BG);
    my_lcd.setTextColor(WHITE, IND_BG);
    char storageText[48];
    snprintf(storageText,
             sizeof(storageText),
             "%llu Used | %llu Rem (%u%%)",
             static_cast<unsigned long long>(ui.effectiveUsedRecords),
             static_cast<unsigned long long>(ui.remainingRecords),
             static_cast<unsigned>(ui.storageFilledPercent));
    my_lcd.drawCentreString(storageText, 240, barY + 50, 2);

    lastUsedRecords = ui.effectiveUsedRecords;
    lastRemainingRecords = ui.remainingRecords;
    lastCapacity = ui.maximumRecords;
    lastFilledPercent = ui.storageFilledPercent;
    lastRefreshMs = nowMs;
}

void updateLogStatusIndicator(const SessionUiSnapshot& ui, bool force) {
    static bool initialized = false;
    static SessionUiMessage lastMessage = SessionUiMessage::Standby;
    static uint32_t lastCurrentChunk = 0U;
    static uint32_t lastTotalChunks = 0U;
    static uint32_t lastPreparationPercent = UINT32_MAX;
    static uint32_t lastRefreshMs = 0U;
    constexpr uint32_t kStatusRefreshMs = 250U;
    const uint32_t nowMs = millis();
    const bool changed = !initialized ||
        lastMessage != ui.message ||
        lastCurrentChunk != ui.currentChunk ||
        lastTotalChunks != ui.totalChunks ||
        lastPreparationPercent != ui.preparationPercent;
    if (!force && (!changed || nowMs - lastRefreshMs < kStatusRefreshMs)) {
        return;
    }

    constexpr int kStatusY = 130;
    my_lcd.fillRect(135, kStatusY, 340, 18, IND_BG);
    my_lcd.setTextColor(sessionUiMessageColor(ui.message), IND_BG);
    char statusText[40];
    if (ui.message == SessionUiMessage::PreparingStorage &&
        ui.preparationSectorsTotal > 0U) {
        snprintf(statusText,
                 sizeof(statusText),
                 "Preparing storage %u%%",
                 static_cast<unsigned>(ui.preparationPercent));
    } else if ((ui.message == SessionUiMessage::Uploading ||
                ui.message == SessionUiMessage::Verifying) &&
               ui.totalChunks > 0U) {
        snprintf(statusText,
                 sizeof(statusText),
                 "%s %u/%u",
                 sessionUiMessageText(ui.message),
                 static_cast<unsigned>(ui.currentChunk),
                 static_cast<unsigned>(ui.totalChunks));
    } else {
        snprintf(statusText,
                 sizeof(statusText),
                 "%s",
                 sessionUiMessageText(ui.message));
    }
    my_lcd.drawString(statusText, 140, kStatusY, 2);

    initialized = true;
    lastMessage = ui.message;
    lastCurrentChunk = ui.currentChunk;
    lastTotalChunks = ui.totalChunks;
    lastPreparationPercent = ui.preparationPercent;
    lastRefreshMs = nowMs;
}

void drawLogScreen() {
    const SessionUiSnapshot ui = sessionUiController.getSnapshot();
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("Data Log & Sync");

    int barX = 20;
    int barY = 50;
    int barW = 440;
    my_lcd.setTextColor(TFT_LIGHTGREY);
    my_lcd.drawString("Local Storage Capacity (16MB Flash)", barX, barY, 2);
    my_lcd.drawRoundRect(barX, barY + 20, barW, 25, 4, WHITE);
    updateLogStorageIndicators(ui, true);

    int statY = 130;
    my_lcd.setTextColor(TFT_LIGHTGREY);
    my_lcd.drawString("Status :", 20, statY, 2);
    
    updateLogStatusIndicator(ui, true);
    
    my_lcd.setTextColor(TFT_LIGHTGREY);
    my_lcd.drawString("Unsynced Data :", 20, statY + 20, 2);
    my_lcd.setTextColor(WHITE);
    char unsyncedText[40];
    if (ui.clearConfirmationArmed) {
        snprintf(unsyncedText,
                 sizeof(unsyncedText),
                 "Press Clear All again");
    } else if (ui.uploadInProgress && ui.retainedRecords > 0U) {
        const uint32_t percent = static_cast<uint32_t>(
            (ui.uploadedRecords * 100U) / ui.retainedRecords);
        snprintf(unsyncedText,
                 sizeof(unsyncedText),
                 "%llu/%llu Records (%u%%)",
                 static_cast<unsigned long long>(ui.uploadedRecords),
                 static_cast<unsigned long long>(ui.retainedRecords),
                 static_cast<unsigned>(percent > 100U ? 100U : percent));
    } else if (ui.synchronized) {
        snprintf(unsyncedText, sizeof(unsyncedText), "None (Synced)");
    } else if (ui.retainedRecords == 0U) {
        snprintf(unsyncedText, sizeof(unsyncedText), "None");
    } else {
        snprintf(unsyncedText,
                 sizeof(unsyncedText),
                 "%llu Records",
                 static_cast<unsigned long long>(ui.retainedRecords));
    }
    my_lcd.drawString(unsyncedText, 140, statY + 20, 2);

    int btnY1 = 185;
    my_lcd.fillRoundRect(70,
                         btnY1,
                         150,
                         45,
                         5,
                         ui.startAvailable ? TFT_GREEN : TFT_DARKGREY);
    my_lcd.drawRoundRect(70, btnY1, 150, 45, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Start Log", 145, btnY1 + 10, 4);

    my_lcd.fillRoundRect(260,
                         btnY1,
                         150,
                         45,
                         5,
                         ui.stopAvailable ? TFT_ORANGE : TFT_DARKGREY);
    my_lcd.drawRoundRect(260, btnY1, 150, 45, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Stop Log", 335, btnY1 + 10, 4);

    int btnY2 = 245;
    my_lcd.fillRoundRect(10, btnY2, 140, 50, 5, TFT_RED);
    my_lcd.drawRoundRect(10, btnY2, 140, 50, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Back", 80, btnY2 + 15, 4);

    my_lcd.fillRoundRect(170,
                         btnY2,
                         140,
                         50,
                         5,
                         ui.clearAvailable ? TFT_DARKGREY : IND_BG);
    my_lcd.drawRoundRect(170, btnY2, 140, 50, 5, WHITE);
    my_lcd.setTextColor(ui.clearAvailable ? TFT_RED : TFT_DARKGREY);
    my_lcd.drawCentreString("Clear All", 240, btnY2 + 15, 4);

    my_lcd.fillRoundRect(330,
                         btnY2,
                         140,
                         50,
                         5,
                         ui.syncAvailable ? TFT_BLUE : TFT_DARKGREY);
    my_lcd.drawRoundRect(330, btnY2, 140, 50, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Push Sync", 400, btnY2 + 15, 4);
}

void drawGraphsMenu() {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("Graphs Menu");

    int btnW = 215; 
    int btnH = 100;
    int col1 = 15;  
    int col2 = 250; 
    int row1 = 80;
    
    my_lcd.fillRoundRect(col1, row1, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col1, row1, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Time Domain", col1 + (btnW / 2), row1 + 37, 4);

    my_lcd.fillRoundRect(col2, row1, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col2, row1, btnW, btnH, 5, WHITE);
    my_lcd.drawCentreString("Freq Domain", col2 + (btnW / 2), row1 + 37, 4);

    my_lcd.fillRoundRect(165, 217, 150, 60, 5, TFT_RED);
    my_lcd.drawRoundRect(165, 217, 150, 60, 5, WHITE);
    my_lcd.drawCentreString("Back", 240, 215 + 22, 4);
}

void drawParameterMenu(String title) {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader(title);

    int btnW = 215; 
    int btnH = 100;
    int col1 = 15;  
    int col2 = 250; 
    int row1 = 80;
    
    my_lcd.fillRoundRect(col1, row1, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col1, row1, btnW, btnH, 5, IND_ACCENT); 
    my_lcd.drawCentreString("Voltage", col1 + (btnW / 2), row1 + 37, 4);

    my_lcd.fillRoundRect(col2, row1, btnW, btnH, 5, TFT_DARKGREY);
    my_lcd.drawRoundRect(col2, row1, btnW, btnH, 5, IND_WARN); 
    my_lcd.drawCentreString("Current", col2 + (btnW / 2), row1 + 37, 4);

    my_lcd.fillRoundRect(165, 217, 150, 60, 5, TFT_RED);
    my_lcd.drawRoundRect(165, 217, 150, 60, 5, WHITE);
    my_lcd.drawCentreString("Back", 240, 215 + 22, 4);
}

void drawFFTGraph(bool isVoltage) {
  my_lcd.fillScreen(IND_BG);
  drawSystemHeader(isVoltage ? "FFT: Voltage" : "FFT: Current");

  int gX = 15;
  int gY = 50;
  int gW = 320;
  int gH = 195;
  int bottomY = gY + gH;

  my_lcd.drawLine(gX, gY, gX, bottomY, WHITE); 
  my_lcd.drawLine(gX, bottomY, gX + gW, bottomY, WHITE); 

  my_lcd.drawRoundRect(345, 50, 125, 195, 3, TFT_LIGHTGREY);
  my_lcd.setTextColor(GREEN, IND_BG);
  my_lcd.drawCentreString("Harmonics", 407, 58, 2);

  my_lcd.fillRoundRect(15, 250, 120, 45, 5, TFT_RED);
  my_lcd.drawRoundRect(15, 250, 120, 45, 5, WHITE);
  my_lcd.setTextColor(WHITE);
  my_lcd.drawCentreString("Back", 75, 262, 4);

  // Switch Button
  my_lcd.fillRoundRect(345, 250, 120, 45, 5, BLUE);
  my_lcd.drawRoundRect(345, 250, 120, 45, 5, WHITE);
  my_lcd.setTextColor(WHITE);
  my_lcd.drawCentreString("Time View", 405, 262, 4);
}

void drawStaticGraph(bool isVoltage) {
  my_lcd.fillScreen(IND_BG); 
  drawSystemHeader(isVoltage ? "Waveform: Voltage" : "Waveform: Current");

  int gX = 15;
  int gY = 50;
  int gW = 320;
  int gH = 195;
  int midY = gY + (gH / 2);

  my_lcd.drawRect(gX - 1, gY - 1, gW + 2, gH + 2, WHITE);
  my_lcd.drawLine(gX, midY, gX + gW, midY, GRAY);

  my_lcd.drawRoundRect(345, 50, 125, 195, 3, TFT_LIGHTGREY);
  my_lcd.setTextColor(GREEN, IND_BG);
  my_lcd.drawCentreString("Live Data", 407, 58, 2);

  my_lcd.fillRoundRect(15, 250, 120, 45, 5, TFT_RED);
  my_lcd.drawRoundRect(15, 250, 120, 45, 5, WHITE);
  my_lcd.setTextColor(WHITE);
  my_lcd.drawCentreString("Back", 75, 262, 4);

  // Switch Button
  my_lcd.fillRoundRect(345, 250, 120, 45, 5, BLUE);
  my_lcd.drawRoundRect(345, 250, 120, 45, 5, WHITE);
  my_lcd.setTextColor(WHITE);
  my_lcd.drawCentreString("FFT View", 405, 262, 4);
}

void drawSettingsScreen() {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("System Settings");

    int col1 = 15;
    int col2 = 250;
    int btnW = 215;
    int boxH = 85;
    int row1 = 60;
    int row2 = 160;

    my_lcd.drawRoundRect(col1, row1, btnW, boxH, 5, TFT_LIGHTGREY);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString("WiFi Network", col1 + 10, row1 + 10, 2);
    uint16_t wifiColor = isWifiConnected ? TFT_GREEN : TFT_DARKGREY;
    my_lcd.fillRoundRect(col1 + 10, row1 + 35, btnW - 20, 40, 5, wifiColor);
    my_lcd.setTextColor(isWifiConnected ? IND_BG : WHITE);
    my_lcd.drawCentreString(isWifiConnected ? "Connected" : "Connect...", col1 + (btnW/2), row1 + 45, 4);

    my_lcd.drawRoundRect(col2, row1, btnW, boxH, 5, TFT_LIGHTGREY);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString("Log Interval", col2 + 10, row1 + 10, 2);
    my_lcd.fillRoundRect(col2 + 10, row1 + 35, btnW - 20, 40, 5, BLUE);
    my_lcd.setTextColor(WHITE);
    String intervalText = String(logIntervals[currentLogIntervalIdx]) + " SECONDS";
    my_lcd.drawCentreString(intervalText, col2 + (btnW/2), row1 + 45, 4);

    my_lcd.drawRoundRect(col1, row2, btnW, boxH, 5, TFT_LIGHTGREY);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString("Encoder Sensitivity", col1 + 10, row2 + 10, 2);
    my_lcd.fillRoundRect(col1 + 10, row2 + 35, 45, 40, 5, TFT_DARKGREY);
    my_lcd.drawCentreString("-", col1 + 32, row2 + 45, 4);
    my_lcd.drawCentreString(String(encoderSensitivity), col1 + (btnW/2), row2 + 45, 4);
    my_lcd.fillRoundRect(col1 + btnW - 55, row2 + 35, 45, 40, 5, TFT_DARKGREY);
    my_lcd.drawCentreString("+", col1 + btnW - 32, row2 + 45, 4);

    my_lcd.drawRoundRect(col2, row2, btnW, boxH, 5, TFT_LIGHTGREY);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString("System Clock", col2 + 10, row2 + 10, 2);
    my_lcd.fillRoundRect(
        col2 + 10, row2 + 35, btnW - 20, 40, 5, TFT_DARKGREY);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("NTP Automatic", col2 + (btnW/2), row2 + 45, 4);

    my_lcd.fillRoundRect(10, 255, 120, 40, 5, TFT_RED);
    my_lcd.drawRoundRect(10, 255, 120, 40, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Back", 70, 265, 4);
}

void drawAboutScreen() {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("System Info");

    int col1 = 20;
    int col2 = 250;
    int startY = 60;
    int lineSpacing = 20; 

    my_lcd.setTextColor(IND_ACCENT);
    my_lcd.drawString("Firmware Versions", col1, startY, 2);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString("Main OS:  v1.0.2", col1, startY + lineSpacing, 2);
    my_lcd.drawString("DSP Core: v1.4.0", col1, startY + (lineSpacing * 2), 2);
    my_lcd.drawString("UI Engine: v1.1.0", col1, startY + (lineSpacing * 3), 2);

    my_lcd.setTextColor(IND_ACCENT);
    my_lcd.drawString("Network Diagnostics", col1, startY + (lineSpacing * 5), 2);
    my_lcd.setTextColor(WHITE);

    String ipAddress = "Not Connected";
    if (WiFi.status() == WL_CONNECTED) {
        ipAddress = WiFi.localIP().toString(); 
    }
    my_lcd.drawString("IP: " + ipAddress, col1, startY + (lineSpacing * 6), 2);
    my_lcd.drawString("MAC: " + WiFi.macAddress(), col1, startY + (lineSpacing * 7), 2);

    my_lcd.setTextColor(IND_WARN);
    my_lcd.drawString("Hardware Specs", col2, startY, 2);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawString("MCU: ESP32-S3-WROOM-1-N16R8", col2, startY + lineSpacing, 2);
    my_lcd.drawString("RAM: 8MB PSRAM", col2, startY + (lineSpacing * 2), 2);
    my_lcd.drawString("Flash: 16MB Local", col2, startY + (lineSpacing * 3), 2);
    my_lcd.drawString("Manufactured: 2026.07.12", col2, startY + (lineSpacing * 4), 2);

    my_lcd.setTextColor(IND_WARN);
    my_lcd.drawString("Project Identity", col2, startY + (lineSpacing * 6), 2);
    my_lcd.setTextColor(WHITE);
    char deviceIdentity[40];
    snprintf(deviceIdentity,
             sizeof(deviceIdentity),
             "Device: %s",
             FirebaseBridge::deviceId());
    my_lcd.drawString(deviceIdentity, col2, startY + (lineSpacing * 7), 2);
    my_lcd.drawString("Team: ElectroSquad", col2, startY + (lineSpacing * 8), 2);
    my_lcd.drawString("Dept: ENTC", col2, startY + (lineSpacing * 9), 2);
    my_lcd.drawString("University: University of Moratuwa", col2, startY + (lineSpacing * 10), 2);

    my_lcd.fillRoundRect(20, 245, 120, 45, 5, TFT_RED);
    my_lcd.drawRoundRect(20, 245, 120, 45, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Back", 80, 257, 4);
}

void startWiFiSetup() {
    my_lcd.fillScreen(IND_BG);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("WiFi Setup Mode", 240, 80, 4);
    my_lcd.setTextColor(IND_ACCENT);
    char provisioningText[48];
    snprintf(provisioningText,
             sizeof(provisioningText),
             "Connect phone to: %s",
             ProvisioningConfig::kPortalSsid);
    my_lcd.drawCentreString(provisioningText, 240, 130, 2);
    snprintf(provisioningText,
             sizeof(provisioningText),
             "Password: %s",
             ProvisioningConfig::kSettingsPortalPassword);
    my_lcd.drawCentreString(provisioningText, 240, 160, 2);
    my_lcd.setTextColor(TFT_RED);
    my_lcd.drawCentreString("Touch disabled. Wait 3 mins to timeout.", 240, 220, 2);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180); 

    if (!wm.startConfigPortal(
            ProvisioningConfig::kPortalSsid,
            ProvisioningConfig::kSettingsPortalPassword)) {
    } else {
        isWifiConnected = true;
    }
    
    currentState = WIFI_MENU_SCREEN;
    screenNeedsUpdate = true;
}

void drawWifiMenuScreen() {
    my_lcd.fillScreen(IND_BG);
    drawSystemHeader("WiFi Manager");

    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Current Status:", 240, 70, 2);
    
    if (WiFi.status() == WL_CONNECTED) {
        my_lcd.setTextColor(TFT_GREEN);
        my_lcd.drawCentreString("Connected To: " + WiFi.SSID(), 240, 100, 4);
    } else {
        my_lcd.setTextColor(TFT_DARKGREY);
        my_lcd.drawCentreString("Offline", 240, 100, 4);
    }

    my_lcd.fillRoundRect(115, 150, 250, 50, 5, BLUE);
    my_lcd.drawRoundRect(115, 150, 250, 50, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Start Setup Portal", 240, 165, 4);

    my_lcd.fillRoundRect(50, 230, 150, 50, 5, TFT_RED);
    my_lcd.drawRoundRect(50, 230, 150, 50, 5, WHITE);
    my_lcd.setTextColor(WHITE);
    my_lcd.drawCentreString("Back", 125, 245, 4);

    if (WiFi.status() == WL_CONNECTED) {
        my_lcd.fillRoundRect(280, 230, 150, 50, 5, TFT_ORANGE);
        my_lcd.drawRoundRect(280, 230, 150, 50, 5, WHITE);
        my_lcd.setTextColor(WHITE);
        my_lcd.drawCentreString("Disconnect", 355, 245, 4);
    }
}


// --- New UI Focus Mapping & Rendering Logic ---
void defineFocusElements() {
    currentFocusCount = 0;
    switch (currentState) {
        case HOME_SCREEN:
            currentFocusElements[0] = {140, 245, 200, 45};
            currentFocusCount = 1;
            break;
        case MENU_SCREEN:
            currentFocusElements[0] = {15, 65, 215, 65};
            currentFocusElements[1] = {250, 65, 215, 65};
            currentFocusElements[2] = {15, 145, 215, 65};
            currentFocusElements[3] = {250, 145, 215, 65};
            currentFocusElements[4] = {15, 225, 215, 65};
            currentFocusElements[5] = {250, 225, 215, 65};
            currentFocusCount = 6;
            break;
        case MEASUREMENTS_SCREEN:
            currentFocusElements[0] = {10, 255, 120, 35};
            currentFocusElements[1] = {350, 255, 120, 35};
            currentFocusCount = 2;
            break;
        case LOG_SCREEN:
            currentFocusElements[0] = {70, 185, 150, 45};
            currentFocusElements[1] = {260, 185, 150, 45};
            currentFocusElements[2] = {10, 245, 140, 50};
            currentFocusElements[3] = {170, 245, 140, 50};
            currentFocusElements[4] = {330, 245, 140, 50};
            currentFocusCount = 5;
            break;
        case GRAPHS_MENU:
        case TIME_DOMAIN_MENU:
        case FREQUENCY_DOMAIN_MENU:
            currentFocusElements[0] = {15, 80, 215, 100};
            currentFocusElements[1] = {250, 80, 215, 100};
            currentFocusElements[2] = {165, 217, 150, 60};
            currentFocusCount = 3;
            break;
        case GRAPH_VOLTAGE:
        case GRAPH_CURRENT:
            currentFocusElements[0] = {15, 250, 120, 45};
            currentFocusElements[1] = {345, 250, 120, 45};
            currentFocusCount = 2;
            break;
        case SETTINGS_SCREEN:
            currentFocusElements[0] = {25, 95, 195, 40}; // Fixed alignment for "Connected" contour
            currentFocusElements[1] = {260, 95, 195, 40};
            currentFocusElements[2] = {25, 195, 45, 40};
            currentFocusElements[3] = {175, 195, 45, 40};
            // Preserve the legacy focus index as a non-navigating NTP status
            // tile so unrelated Settings focus indexes remain unchanged.
            currentFocusElements[4] = {260, 195, 195, 40};
            currentFocusElements[5] = {10, 255, 120, 40};
            currentFocusCount = 6;
            break;
        case WIFI_MENU_SCREEN:
            currentFocusElements[0] = {115, 150, 250, 50};
            currentFocusElements[1] = {50, 230, 150, 50};
            if (WiFi.status() == WL_CONNECTED) {
                currentFocusElements[2] = {280, 230, 150, 50};
                currentFocusCount = 3;
            } else {
                currentFocusCount = 2;
            }
            break;
        case ABOUT_SCREEN:
            currentFocusElements[0] = {20, 245, 120, 45};
            currentFocusCount = 1;
            break;
        default:
            break;
    }
    
    // Prevent index out of bounds when switching screens
    if(currentFocusIndex >= currentFocusCount) currentFocusIndex = 0; 
    lastFocusIndex = -1;
}

void drawFocus() {
    if (currentFocusCount == 0) return;
    
    // Clear previous focus box with background color
    if (lastFocusIndex >= 0 && lastFocusIndex < currentFocusCount && lastFocusIndex != currentFocusIndex) {
        FocusRect r = currentFocusElements[lastFocusIndex];
        my_lcd.drawRoundRect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8, IND_BG);
        my_lcd.drawRoundRect(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 9, IND_BG);
    }

    // Draw high-contrast focus box for the current element
    if (currentFocusIndex >= 0 && currentFocusIndex < currentFocusCount) {
        FocusRect r = currentFocusElements[currentFocusIndex];
        my_lcd.drawRoundRect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 8, TFT_YELLOW);
        my_lcd.drawRoundRect(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 9, TFT_YELLOW);
    }
    lastFocusIndex = currentFocusIndex;
}


// --- Main Setup ---
void setup(void) {
  gpio15HoldLowConfigured = configureGpio15HeldLow();
  Serial.begin(115200);
  if (!gpio15HoldLowConfigured) {
    Serial.println("GPIO15 LOW configuration failed");
  }
#if FLASH_RECORD_CODEC_SELF_TEST
  FlashRecordCodec::runSelfTest();
#endif
  
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderISR, CHANGE);
  
  my_lcd.init();
  my_lcd.setRotation(1); 
  touch_init(my_lcd.width(), my_lcd.height(), my_lcd.getRotation());
  wallClockService.begin();
  sessionStorage.begin();
  // Recovery may scan the entire pqlog ring. Start the STM32 UART only after
  // that scan so its finite RX buffer cannot overflow during boot recovery.
  // HardwareSerial installs its IDF RX ring through malloc(). Temporarily
  // raise the internal-allocation preference above this bounded ring size so
  // the buffer remains accessible while external-memory cache is unavailable.
  const size_t internalHeapBeforeUart =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(ACQUISITION_UART_RX_BUFFER_BYTES + 1U);
#endif
  const size_t configuredRxBuffer =
      Serial1.setRxBufferSize(ACQUISITION_UART_RX_BUFFER_BYTES);
  Serial1.setTimeout(10);
  Serial1.begin(ACQUISITION_UART_BAUD,
                SERIAL_8N1,
                STM_RX_PIN,
                STM_TX_PIN);
#if CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
  const bool fifoThresholdConfigured = Serial1.setRxFIFOFull(
      ACQUISITION_UART_FIFO_THRESHOLD_BYTES);
  const size_t internalHeapAfterUart =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internalHeapDelta =
      internalHeapBeforeUart >= internalHeapAfterUart
          ? internalHeapBeforeUart - internalHeapAfterUart
          : 0U;
  const bool rxBufferInternal =
      internalHeapDelta >= ACQUISITION_UART_RX_BUFFER_BYTES;
  if (configuredRxBuffer != ACQUISITION_UART_RX_BUFFER_BYTES ||
      !fifoThresholdConfigured || !rxBufferInternal) {
    Serial.println(
        "UART acquisition error: internal RX buffering could not be verified");
  }
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  uartRxBufferConfiguredBytes = static_cast<uint32_t>(configuredRxBuffer);
  uartRxInternalHeapDeltaBytes = static_cast<uint32_t>(internalHeapDelta);
  uartRxBufferInternal = rxBufferInternal;
  Serial1.onReceiveError(handleStm32UartReceiveError);
#endif
  sessionLogger.begin(sessionStorage, wallClockService);
  firebaseBridge.begin(sessionStorage, sessionLogger);
  sessionUiController.begin(sessionLogger, sessionStorage, firebaseBridge);
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
  printSessionStorageTestHelp();
#endif
}

// --- Main Touch/Click Handler ---
void handleInteraction(int16_t x, int16_t y) {
    switch (currentState) {
        case HOME_SCREEN:
            if (x > 140 && x < 340 && y > 245 && y < 290) {
                currentState = MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            break;

        case MENU_SCREEN:
            if (x > 15 && x < 230 && y > 55 && y < 120) {
                currentState = MEASUREMENTS_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 250 && x < 465 && y > 55 && y < 120) {
                currentState = GRAPHS_MENU; 
                screenNeedsUpdate = true;
            }
            else if (x > 15 && x < 230 && y > 135 && y < 200) {
                currentState = LOG_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 250 && x < 465 && y > 135 && y < 200) {
                currentState = SETTINGS_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 15 && x < 230 && y > 215 && y < 280) {
                currentState = HOME_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 250 && x < 465 && y > 215 && y < 280) {
                currentState = ABOUT_SCREEN;
                screenNeedsUpdate = true;
            }
            break;

        case MEASUREMENTS_SCREEN:
            if (x > 10 && x < 130 && y > 255 && y < 290) {
                currentState = MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 350 && x < 470 && y > 255 && y < 290) {
                if (currentMeasurementPage == 1) {
                    currentMeasurementPage = 2;
                } else {
                    currentMeasurementPage = 1;
                }
                screenNeedsUpdate = true;
            }
            break;

        case LOG_SCREEN:
            if (x > 70 && x < 220 && y > 185 && y < 230) {
                startLoggingSession();
            }
            else if (x > 260 && x < 410 && y > 185 && y < 230) {
                requestLoggingStop();
            }
            else if (x > 10 && x < 150 && y > 245 && y < 295) {
                sessionUiController.cancelClearConfirmation();
                currentState = MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 170 && x < 310 && y > 245 && y < 295) {
                requestRetainedSessionClear();
            }
            else if (x > 330 && x < 470 && y > 245 && y < 295) {
                requestSessionSynchronization();
            }
            break;

        case GRAPHS_MENU:
            if (x > 15 && x < 230 && y > 80 && y < 180) {
                isTimeDomain = true;
                currentState = TIME_DOMAIN_MENU;
                screenNeedsUpdate = true;
            }
            else if (x > 250 && x < 465 && y > 80 && y < 180) {
                isTimeDomain = false;
                currentState = FREQUENCY_DOMAIN_MENU;
                screenNeedsUpdate = true;
            }
            else if (x > 165 && x < 315 && y > 215 && y < 280) {
                currentState = MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            break;

        case TIME_DOMAIN_MENU:
            if (x > 165 && x < 315 && y > 215 && y < 280) {
                currentState = GRAPHS_MENU;
                screenNeedsUpdate = true;
            }
            else if (x > 15 && x < 230 && y > 80 && y < 180) {
                currentState = GRAPH_VOLTAGE;
                screenNeedsUpdate = true;
            }
            else if (x > 250 && x < 465 && y > 80 && y < 180) {
                currentState = GRAPH_CURRENT;
                screenNeedsUpdate = true;
            }
            break;

        case FREQUENCY_DOMAIN_MENU:
            if (x > 15 && x < 230 && y > 80 && y < 180) {
                currentState = GRAPH_VOLTAGE;
                screenNeedsUpdate = true;
            }
            else if (x > 250 && x < 465 && y > 80 && y < 180) {
                currentState = GRAPH_CURRENT;
                screenNeedsUpdate = true;
            }
            else if (x > 165 && x < 315 && y > 215 && y < 280) {
                currentState = GRAPHS_MENU;
                screenNeedsUpdate = true;
            }
            break;

        case GRAPH_VOLTAGE:
        case GRAPH_CURRENT:
            if (x > 15 && x < 135 && y > 245 && y < 290) {
                currentState = isTimeDomain ? TIME_DOMAIN_MENU : FREQUENCY_DOMAIN_MENU;
                screenNeedsUpdate = true;
            }
            else if (x > 345 && x < 465 && y > 245 && y < 290) {
                isTimeDomain = !isTimeDomain;
                screenNeedsUpdate = true;
            }
            break;

        case SETTINGS_SCREEN:
            if (x > 10 && x < 130 && y > 255 && y < 295) {
                currentState = MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 25 && x < 220 && y > 95 && y < 135) {
                currentState = WIFI_MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 260 && x < 455 && y > 95 && y < 135) {
                currentLogIntervalIdx++;
                if (currentLogIntervalIdx > 3) currentLogIntervalIdx = 0; 
                screenNeedsUpdate = true;
            }
            else if (x > 25 && x < 70 && y > 195 && y < 235) {
                if (encoderSensitivity > 1) encoderSensitivity--;
                screenNeedsUpdate = true;
            }
            else if (x > 175 && x < 220 && y > 195 && y < 235) {
                if (encoderSensitivity < 10) encoderSensitivity++;
                screenNeedsUpdate = true;
            }
            break;

        case WIFI_MENU_SCREEN:
            if (x > 115 && x < 365 && y > 150 && y < 200) { startWiFiSetup(); }
            else if (x > 50 && x < 200 && y > 230 && y < 280) {
                currentState = SETTINGS_SCREEN;
                screenNeedsUpdate = true;
            }
            else if (x > 280 && x < 430 && y > 230 && y < 280) {
                if (WiFi.status() == WL_CONNECTED) {
                    WiFi.disconnect();
                    isWifiConnected = false;
                    screenNeedsUpdate = true;
                }
            }
            break;

        case ABOUT_SCREEN:
            if (x > 20 && x < 170 && y > 245 && y < 290) {
                currentState = MENU_SCREEN;
                screenNeedsUpdate = true;
            }
            break;
    }
}

// --- Main Loop ---
void loop() {
    // 1. Process Incoming Packets over RX0 & Trigger Graph/Text Updates
    synchronizeLoggingState();
    readUART();
    wallClockService.service();
    serviceSystemFooterClock();
#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE
    processSessionStorageTestConsole();
#endif

    // 2. Draw Static Screens
    if (screenNeedsUpdate) {
        screenNeedsUpdate = false; 

        switch (currentState) {
            case SPLASH_SCREEN: drawSplashScreen(); break;
            case HOME_SCREEN: drawHomeScreen(); updateHomeScreenData(); break;
            case MENU_SCREEN: drawMenuScreen(); break;
            case MEASUREMENTS_SCREEN: drawMeasurementsScreen(); updateMeasurementsScreenData(); break;
            case LOG_SCREEN: drawLogScreen(); break;
            case SETTINGS_SCREEN: drawSettingsScreen(); break;
            case WIFI_MENU_SCREEN: drawWifiMenuScreen(); break;
            case ABOUT_SCREEN: drawAboutScreen(); break;
            case GRAPHS_MENU: drawGraphsMenu(); break;
            case TIME_DOMAIN_MENU: drawParameterMenu("Time Domain"); break;
            case FREQUENCY_DOMAIN_MENU: drawParameterMenu("Freq Domain"); break;
            case GRAPH_VOLTAGE: 
                if (isTimeDomain) { drawStaticGraph(true); updateStaticGraphRealTime(true); } 
                else { drawFFTGraph(true); updateFFTGraphRealTime(true); }
                break;
            case GRAPH_CURRENT: 
                if (isTimeDomain) { drawStaticGraph(false); updateStaticGraphRealTime(false); } 
                else { drawFFTGraph(false); updateFFTGraphRealTime(false); }
                break;
        }
        
        // Ensure footer draws on all screens except Splash
        if(currentState != SPLASH_SCREEN) {
            drawSystemFooter();
            defineFocusElements();
            drawFocus();
        }
    }

    // 3. Handle Rotary Encoder Input
    if (abs(encoderDelta) >= encoderSensitivity) {
        // Calculate how many distinct UI ticks occurred based on sensitivity
        int ticks = encoderDelta / encoderSensitivity;
        encoderDelta = encoderDelta % encoderSensitivity; // Keep remainder to stay smooth
        
        if (currentFocusCount > 0) {
            // Normal Navigation behavior
            currentFocusIndex += ticks;
            
            if (currentFocusIndex >= currentFocusCount) currentFocusIndex = currentFocusIndex % currentFocusCount;
            while (currentFocusIndex < 0) currentFocusIndex += currentFocusCount;
            
            drawFocus(); 
        }
    }

    // 4. Handle Hardware Button Input (Select & Back)
    if (isSelectPressed() && currentFocusCount > 0) {
        int16_t simX = currentFocusElements[currentFocusIndex].x + (currentFocusElements[currentFocusIndex].w / 2);
        int16_t simY = currentFocusElements[currentFocusIndex].y + (currentFocusElements[currentFocusIndex].h / 2);
        handleInteraction(simX, simY);
    }
    else if (isBackPressed()) {
        switch(currentState) {
            case MENU_SCREEN: handleInteraction(100, 250); break; 
            case MEASUREMENTS_SCREEN: handleInteraction(70, 275); break;
            case LOG_SCREEN: handleInteraction(80, 270); break;
            case GRAPHS_MENU: handleInteraction(240, 240); break;
            case TIME_DOMAIN_MENU: handleInteraction(240, 240); break;
            case FREQUENCY_DOMAIN_MENU: handleInteraction(240, 240); break;
            case GRAPH_VOLTAGE: handleInteraction(75, 275); break;
            case GRAPH_CURRENT: handleInteraction(75, 275); break;
            case SETTINGS_SCREEN: handleInteraction(70, 275); break;
            case WIFI_MENU_SCREEN: handleInteraction(125, 255); break;
            case ABOUT_SCREEN: handleInteraction(80, 270); break;
            default: break; 
        }
    }

    // 5. Handle Touch Input
    if (touch_touched()) {
        int16_t x = touch_last_x;
        int16_t y = touch_last_y;
        while (touch_touched()) { delay(10); } 
        handleInteraction(x, y);
    }
}
