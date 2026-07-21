#include "FirebaseBridge.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "FirebaseConfig.h"
#include "SessionLogger.h"
#include "SessionStorage.h"

#ifndef FIREBASE_VERBOSE_LOGGING
#define FIREBASE_VERBOSE_LOGGING 0
#endif

#ifndef FIREBASE_SESSION_SYNC_ENABLED
#define FIREBASE_SESSION_SYNC_ENABLED 1
#endif

#ifndef FIREBASE_INSECURE_TLS_TEST
#error "Define FIREBASE_INSECURE_TLS_TEST in FirebaseConfig.h"
#endif

#if !FIREBASE_INSECURE_TLS_TEST && !defined(FIREBASE_ROOT_CA)
#error "Define FIREBASE_ROOT_CA to a trusted PEM certificate when TLS test mode is disabled"
#endif

static_assert(sizeof(FIREBASE_DEVICE_ID) - 1U <
                  SESSION_SYNC_DEVICE_ID_BYTES,
              "The public device identifier must fit firmware/cloud schemas");

const char* FirebaseBridge::deviceId() {
  return FIREBASE_DEVICE_ID;
}

namespace {

constexpr uint32_t kMinimumPublishIntervalMs = 500;
constexpr uint32_t kInitialRetryDelayMs = 1000;
constexpr uint32_t kMaximumRetryDelayMs = 60000;
constexpr uint32_t kTokenRefreshMarginMs = 300000;
constexpr uint32_t kHttpTimeoutMs = 5000;
constexpr uint32_t kSessionHttpTimeoutMs = 15000;
constexpr uint32_t kWorkerStackSize = 12288;
constexpr uint32_t kWorkerNotificationLive = 1U << 0U;
constexpr uint32_t kWorkerNotificationSync = 1U << 1U;
constexpr uint32_t kWorkerNotificationCancel = 1U << 2U;

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return deadlineMs == 0U ||
         static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

uint32_t millisecondsUntil(uint32_t nowMs, uint32_t deadlineMs) {
  return deadlineMs == 0U || deadlineReached(nowMs, deadlineMs)
             ? 0U
             : deadlineMs - nowMs;
}

class BoundedResponseStream : public Stream {
 public:
  BoundedResponseStream(char* destination, size_t capacity)
      : destination_(destination), capacity_(capacity) {
    if (destination_ != nullptr && capacity_ > 0U) {
      destination_[0] = '\0';
    }
  }

  size_t write(uint8_t value) override {
    return write(&value, 1U);
  }

  size_t write(const uint8_t* source, size_t length) override {
    if (source == nullptr) {
      return 0U;
    }
    const size_t available =
        capacity_ > storedLength_ + 1U
            ? capacity_ - storedLength_ - 1U
            : 0U;
    const size_t copyLength = min(length, available);
    if (copyLength > 0U && destination_ != nullptr) {
      memcpy(destination_ + storedLength_, source, copyLength);
      storedLength_ += copyLength;
      destination_[storedLength_] = '\0';
    }
    totalLength_ += length;
    if (copyLength != length) {
      truncated_ = true;
    }
    // Report the full input as consumed so HTTPClient can drain its response.
    return length;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t storedLength() const { return storedLength_; }
  size_t totalLength() const { return totalLength_; }
  bool truncated() const { return truncated_; }

 private:
  char* destination_ = nullptr;
  size_t capacity_ = 0U;
  size_t storedLength_ = 0U;
  size_t totalLength_ = 0U;
  bool truncated_ = false;
};

void configureTls(WiFiClientSecure& client) {
#if FIREBASE_INSECURE_TLS_TEST
  // TEMPORARY DEVELOPMENT MODE:
  // TLS certificate verification is disabled.
  // Set FIREBASE_INSECURE_TLS_TEST to 0 and configure FIREBASE_ROOT_CA
  // before using the firmware in production.
  client.setInsecure();
#else
  client.setCACert(FIREBASE_ROOT_CA);
#endif
}

void printHttpStatus(int status) {
#if !FIREBASE_VERBOSE_LOGGING
  if (status >= 200 && status < 300) {
    return;
  }
#endif
  Serial.print("Firebase HTTP status: ");
  Serial.println(status);
}

void appendFormUrlEncoded(String& output, const String& value) {
  static const char hex[] = "0123456789ABCDEF";

  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);

    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      output += static_cast<char>(c);
    } else {
      output += '%';
      output += hex[c >> 4];
      output += hex[c & 0x0F];
    }
  }
}

uint32_t parseLifetimeSeconds(JsonVariantConst value) {
  if (value.is<const char*>()) {
    return static_cast<uint32_t>(
        strtoul(value.as<const char*>(), nullptr, 10));
  }

  return value.as<uint32_t>();
}

void printFirebaseErrorMessage(
    const String& responseBody,
    const char* prefix) {
  JsonDocument response;

  if (deserializeJson(response, responseBody) !=
      DeserializationError::Ok) {
    return;
  }

  const char* message =
      response["error"]["message"].as<const char*>();

  if (message != nullptr) {
    Serial.print(prefix);
    Serial.println(message);
  }
}

}  // namespace

bool FirebaseBridge::begin() {
  if (started_) {
    return true;
  }

  snapshotQueue_ =
      xQueueCreate(1, sizeof(LiveTelemetrySnapshot));

  if (snapshotQueue_ == nullptr) {
    Serial.println("Firebase queue creation failed");
    return false;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      workerTaskEntry,
      "FirebaseWorker",
      kWorkerStackSize,
      this,
      1,
      &workerTaskHandle_,
      0);

  if (result != pdPASS) {
    vQueueDelete(snapshotQueue_);
    snapshotQueue_ = nullptr;
    Serial.println("Firebase worker creation failed");
    return false;
  }

  started_ = true;
#if FIREBASE_VERBOSE_LOGGING
  Serial.println("Firebase worker started");
#endif
  return true;
}

bool FirebaseBridge::begin(SessionStorage& storage, SessionLogger& logger) {
  sessionStorage_ = &storage;
  sessionLogger_ = &logger;
  sessionSyncUploader_.attach(
      storage, logger, FIREBASE_SESSION_SYNC_ENABLED != 0);
  return begin();
}

void FirebaseBridge::submit(
    const LiveTelemetrySnapshot& snapshot) {
  if (snapshotQueue_ != nullptr) {
    xQueueOverwrite(snapshotQueue_, &snapshot);
    notifyWorker(kWorkerNotificationLive);
  }
}

bool FirebaseBridge::requestSessionUpload() {
#if !FIREBASE_SESSION_SYNC_ENABLED
  return false;
#else
  if (!started_ || sessionStorage_ == nullptr || sessionLogger_ == nullptr) {
    sessionSyncUploader_.recordRequestRejection(
        SessionSyncError::FirebaseUnavailable);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sessionSyncUploader_.recordRequestRejection(
        SessionSyncError::WifiUnavailable);
    return false;
  }
  if (!sessionSyncUploader_.requestUpload()) {
    return false;
  }
  notifyWorker(kWorkerNotificationSync);
  return true;
#endif
}

void FirebaseBridge::cancelSessionUpload() {
#if FIREBASE_SESSION_SYNC_ENABLED
  sessionSyncUploader_.requestCancellation();
  notifyWorker(kWorkerNotificationCancel);
#endif
}

SessionSyncStatus FirebaseBridge::getSessionSyncStatus() const {
  SessionSyncStatus status = sessionSyncUploader_.getStatus();
  status.wifiConnected = WiFi.status() == WL_CONNECTED;
  status.firebaseAuthenticated = authenticated_;
  return status;
}

bool FirebaseBridge::isAuthenticated() {
  return authenticated_;
}

int FirebaseBridge::lastHttpStatus() {
  return lastHttpStatus_;
}

void FirebaseBridge::workerTaskEntry(void* context) {
  static_cast<FirebaseBridge*>(context)->workerTask();
}

void FirebaseBridge::workerTask() {
  for (;;) {
    drainLatestSnapshot();
    servicePendingLiveTelemetry();

    bool historicalRequestPerformed = false;
#if FIREBASE_SESSION_SYNC_ENABLED
    const uint32_t syncNowMs = millis();
    if (sessionSyncUploader_.hasPendingWork() &&
        sessionSyncUploader_.nextServiceDelayMs(syncNowMs) == 0U) {
      historicalRequestPerformed =
          sessionSyncUploader_.service(*this, syncNowMs);
    }
#endif

    // A historical step performs at most one RTDB transaction. Always check
    // the latest-only live queue again before another historical step.
    if (historicalRequestPerformed) {
      drainLatestSnapshot();
      servicePendingLiveTelemetry();
    }

    uint32_t waitMs = UINT32_MAX;
    const uint32_t nowMs = millis();
    if (liveSnapshotPending_) {
      uint32_t liveDelayMs = nextLiveAttemptMs_ == 0U
          ? 0U
          : millisecondsUntil(nowMs, nextLiveAttemptMs_);
      if (lastPublishMs_ != 0U) {
        const uint32_t elapsedMs = nowMs - lastPublishMs_;
        const uint32_t publishDelayMs =
            elapsedMs >= kMinimumPublishIntervalMs
                ? 0U
                : kMinimumPublishIntervalMs - elapsedMs;
        liveDelayMs = max(liveDelayMs, publishDelayMs);
      }
      waitMs = min(waitMs, liveDelayMs);
    }
#if FIREBASE_SESSION_SYNC_ENABLED
    if (sessionSyncUploader_.hasPendingWork()) {
      waitMs = min(
          waitMs, sessionSyncUploader_.nextServiceDelayMs(nowMs));
    }
#endif
    if (waitMs == 0U) {
      taskYIELD();
      continue;
    }

    uint32_t notificationValue = 0U;
    const TickType_t waitTicks = waitMs == UINT32_MAX
        ? portMAX_DELAY
        : pdMS_TO_TICKS(waitMs);
    xTaskNotifyWait(
        0U,
        UINT32_MAX,
        &notificationValue,
        waitTicks);
  }
}

bool FirebaseBridge::ensureAuthenticated() {
  if (authenticated_ && !tokenNeedsRefresh()) {
    return true;
  }

  bool success = false;

  if (!refreshToken_.isEmpty()) {
    success = refreshIdToken();
  }

  if (!success) {
    success = signInWithPassword();
  }

  authenticated_ = success;

  if (success) {
#if FIREBASE_VERBOSE_LOGGING
    Serial.println("Firebase authentication success");
#endif
  } else {
    Serial.println("Firebase authentication failure");
  }

  return success;
}

bool FirebaseBridge::signInWithPassword() {
  WiFiClientSecure client;
  configureTls(client);

  String url =
      "https://identitytoolkit.googleapis.com/v1/"
      "accounts:signInWithPassword?key=";
  url += FIREBASE_WEB_API_KEY;

  HTTPClient http;

  if (!http.begin(client, url)) {
    lastHttpStatus_ = -1;
    printHttpStatus(lastHttpStatus_);
    return false;
  }

  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  JsonDocument request;
  request["email"] = FIREBASE_DEVICE_EMAIL;
  request["password"] = FIREBASE_DEVICE_PASSWORD;
  request["returnSecureToken"] = true;

  String payload;
  payload.reserve(256);
  serializeJson(request, payload);

  lastHttpStatus_ = http.POST(payload);
  printHttpStatus(lastHttpStatus_);

  const String responseBody = http.getString();
  bool success = false;

  if (lastHttpStatus_ >= 200 && lastHttpStatus_ < 300) {
    JsonDocument response;
    const DeserializationError error =
        deserializeJson(response, responseBody);

    if (error) {
      Serial.print("Firebase auth JSON parse error: ");
      Serial.println(error.c_str());
    } else {
      const String idToken =
          response["idToken"].as<String>();
      const String refreshToken =
          response["refreshToken"].as<String>();
      const uint32_t lifetime =
          parseLifetimeSeconds(response["expiresIn"]);

      if (!idToken.isEmpty() &&
          !refreshToken.isEmpty() &&
          lifetime > 0) {
        idToken_ = idToken;
        refreshToken_ = refreshToken;
        tokenLifetimeSeconds_ = lifetime;
        tokenIssuedAtMs_ = millis();
        success = true;
      } else if (
          response["mfaPendingCredential"].is<const char*>()) {
        Serial.println("Firebase authentication requires MFA");
      } else {
        Serial.println(
            "Firebase auth response missing required fields");
      }
    }
  } else {
    printFirebaseErrorMessage(
        responseBody,
        "Firebase authentication error: ");
  }

  http.end();
  return success;
}

bool FirebaseBridge::refreshIdToken() {
  WiFiClientSecure client;
  configureTls(client);

  String url =
      "https://securetoken.googleapis.com/v1/token?key=";
  url += FIREBASE_WEB_API_KEY;

  HTTPClient http;

  if (!http.begin(client, url)) {
    lastHttpStatus_ = -1;
    printHttpStatus(lastHttpStatus_);
    return false;
  }

  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.addHeader(
      "Content-Type",
      "application/x-www-form-urlencoded");

  String payload =
      "grant_type=refresh_token&refresh_token=";

  payload.reserve(
      payload.length() + refreshToken_.length() * 3);

  appendFormUrlEncoded(payload, refreshToken_);

  lastHttpStatus_ = http.POST(payload);
  printHttpStatus(lastHttpStatus_);

  const String responseBody = http.getString();
  bool success = false;

  if (lastHttpStatus_ >= 200 && lastHttpStatus_ < 300) {
    JsonDocument response;
    const DeserializationError error =
        deserializeJson(response, responseBody);

    if (error) {
      Serial.print(
          "Firebase token refresh JSON parse error: ");
      Serial.println(error.c_str());
    } else {
      const String idToken =
          response["id_token"].as<String>();
      const String newRefreshToken =
          response["refresh_token"].as<String>();
      const uint32_t lifetime =
          parseLifetimeSeconds(response["expires_in"]);

      if (!idToken.isEmpty() && lifetime > 0) {
        idToken_ = idToken;

        if (!newRefreshToken.isEmpty()) {
          refreshToken_ = newRefreshToken;
        }

        tokenLifetimeSeconds_ = lifetime;
        tokenIssuedAtMs_ = millis();
        success = true;
      } else {
        Serial.println(
            "Firebase token refresh response missing required fields");
      }
    }
  } else {
    printFirebaseErrorMessage(
        responseBody,
        "Firebase token refresh error: ");
  }

  http.end();
  return success;
}

bool FirebaseBridge::publish(
    const LiveTelemetrySnapshot& snapshot) {
  WiFiClientSecure client;
  configureTls(client);

  String url = FIREBASE_DATABASE_URL;

  while (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }

  url += "/devices/";
  url += FIREBASE_DEVICE_ID;
  url += "/live.json?auth=";
  url += idToken_;

  HTTPClient http;

  if (!http.begin(client, url)) {
    lastHttpStatus_ = -1;
    printHttpStatus(lastHttpStatus_);
    return false;
  }

  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  JsonDocument document;

  document["deviceId"] = FIREBASE_DEVICE_ID;
  document["seq"] = snapshot.seq;
  document["uptimeMs"] = snapshot.uptimeMs;

  JsonObject metrics =
      document["metrics"].to<JsonObject>();

  metrics["vRms"] = snapshot.vRms;
  metrics["iRms"] = snapshot.iRms;
  metrics["frequency"] = snapshot.frequency;
  metrics["powerFactor"] = snapshot.powerFactor;
  metrics["activePower"] = snapshot.activePower;
  metrics["apparentPower"] = snapshot.apparentPower;
  metrics["reactivePower"] = snapshot.reactivePower;
  metrics["crestFactorV"] = snapshot.crestFactorV;
  metrics["crestFactorI"] = snapshot.crestFactorI;
  metrics["swellFactor"] = snapshot.swellFactor;
  metrics["thdV"] = snapshot.thdV;
  metrics["thdI"] = snapshot.thdI;

  JsonObject status =
      document["status"].to<JsonObject>();

  status["online"] = true;
  status["logging"] = snapshot.logging;
  status["wifiConnected"] = snapshot.wifiConnected;
  status["wifiRssi"] = snapshot.wifiRssi;

  String payload;
  payload.reserve(512);
  serializeJson(document, payload);

  lastHttpStatus_ = http.PUT(payload);
  printHttpStatus(lastHttpStatus_);

  const bool success =
      lastHttpStatus_ >= 200 &&
      lastHttpStatus_ < 300;

  if (success) {
#if FIREBASE_VERBOSE_LOGGING
    Serial.println("Firebase telemetry publish success");
#endif
  } else {
    const String responseBody = http.getString();

    printFirebaseErrorMessage(
        responseBody,
        "Firebase telemetry error: ");

    if (lastHttpStatus_ == 401 ||
        lastHttpStatus_ == 403) {
      authenticated_ = false;
    }
  }

  http.end();
  return success;
}

bool FirebaseBridge::performDatabaseRequest(
    FirebaseDatabaseMethod method,
    const char* relativePath,
    const uint8_t* requestBody,
    size_t requestLength,
    const FirebaseDatabaseRequestOptions& options,
    char* responseBody,
    size_t responseCapacity,
    FirebaseDatabaseResponse& response) {
  response = FirebaseDatabaseResponse{};
  const bool hasIfMatch =
      options.ifMatch != nullptr && options.ifMatch[0] != '\0';
  if (relativePath == nullptr || relativePath[0] == '\0' ||
      strchr(relativePath, '?') != nullptr ||
      strchr(relativePath, '#') != nullptr ||
      strstr(relativePath, ".json") != nullptr ||
      (hasIfMatch && options.printSilent) ||
      (requestLength > 0U && requestBody == nullptr) ||
      (responseCapacity > 0U && responseBody == nullptr)) {
    response.httpStatus = -1;
    lastHttpStatus_ = response.httpStatus;
    return false;
  }
  if (responseBody != nullptr && responseCapacity > 0U) {
    responseBody[0] = '\0';
  }
  if (WiFi.status() != WL_CONNECTED) {
    response.httpStatus = -1;
    lastHttpStatus_ = response.httpStatus;
    return false;
  }
  if (!ensureAuthenticated()) {
    response.httpStatus = lastHttpStatus_;
    response.authenticationFailed = !authenticated_;
    return false;
  }

  WiFiClientSecure client;
  configureTls(client);

  String url;
  const size_t urlCapacity = strlen(FIREBASE_DATABASE_URL) +
                             strlen(relativePath) +
                             idToken_.length() + 32U;
  if (!url.reserve(urlCapacity)) {
    response.httpStatus = -1;
    lastHttpStatus_ = response.httpStatus;
    return false;
  }
  url = FIREBASE_DATABASE_URL;
  while (url.endsWith("/")) {
    url.remove(url.length() - 1U);
  }
  url += "/";
  url += relativePath;
  url += ".json?auth=";
  url += idToken_;
  if (options.printSilent) {
    url += "&print=silent";
  }

  HTTPClient http;
  if (!http.begin(client, url)) {
    response.httpStatus = -1;
    lastHttpStatus_ = response.httpStatus;
    printHttpStatus(lastHttpStatus_);
    return false;
  }

  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kSessionHttpTimeoutMs);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");
  if (hasIfMatch) {
    http.addHeader("If-Match", options.ifMatch);
  }
  if (options.requestEtag) {
    static const char* responseHeaders[] = {"ETag"};
    http.collectHeaders(responseHeaders, 1U);
    http.addHeader("X-Firebase-ETag", "true");
  }

  switch (method) {
    case FirebaseDatabaseMethod::Get:
      response.httpStatus = http.GET();
      break;
    case FirebaseDatabaseMethod::Put:
      response.httpStatus = http.PUT(
          const_cast<uint8_t*>(requestBody), requestLength);
      break;
    case FirebaseDatabaseMethod::Patch:
      response.httpStatus = http.PATCH(
          const_cast<uint8_t*>(requestBody), requestLength);
      break;
    default:
      response.httpStatus = -1;
      break;
  }
  lastHttpStatus_ = response.httpStatus;
  printHttpStatus(lastHttpStatus_);

  if (options.requestEtag && http.hasHeader("ETag")) {
    const String etag = http.header("ETag");
    etag.toCharArray(response.etag, sizeof(response.etag));
  }

  // Conditional writes return JSON because print=silent is intentionally not
  // allowed with If-Match. Drain that body before end() so the uploader can
  // validate initial creation; final completion also retains its existing GET
  // verification. Failed writes carry the Firebase diagnostic text.
  const bool shouldReadResponse =
      method == FirebaseDatabaseMethod::Get ||
      hasIfMatch ||
      response.httpStatus < 200 || response.httpStatus >= 300;
  if (shouldReadResponse && responseBody != nullptr &&
      responseCapacity > 0U && response.httpStatus > 0 &&
      response.httpStatus != 204) {
    BoundedResponseStream destination(responseBody, responseCapacity);
    const int streamResult = http.writeToStream(&destination);
    response.bodyLength = destination.storedLength();
    response.bodyTruncated = destination.truncated();
    if (streamResult < 0 &&
        response.httpStatus >= 200 && response.httpStatus < 300) {
      // A response-body transport failure is retryable. Keep capacity
      // truncation distinct so an actually oversized cloud document remains
      // a terminal schema/size error.
      response.httpStatus = streamResult;
      response.bodyTruncated = false;
      lastHttpStatus_ = response.httpStatus;
      printHttpStatus(lastHttpStatus_);
    } else if (streamResult < 0) {
      // Preserve the original Firebase 4xx/5xx status for classification even
      // if its optional diagnostic body cannot be read completely.
      response.bodyTruncated = true;
    }
  }

  if (response.httpStatus == 401) {
    authenticated_ = false;
  }
  http.end();
  return response.httpStatus > 0;
}

bool FirebaseBridge::tokenNeedsRefresh() const {
  if (!authenticated_ ||
      tokenLifetimeSeconds_ == 0) {
    return true;
  }

  const uint32_t lifetimeMs =
      tokenLifetimeSeconds_ * 1000UL;

  const uint32_t refreshAtMs =
      lifetimeMs > kTokenRefreshMarginMs
          ? lifetimeMs - kTokenRefreshMarginMs
          : lifetimeMs / 2;

  return millis() - tokenIssuedAtMs_ >= refreshAtMs;
}

bool FirebaseBridge::servicePendingLiveTelemetry() {
  if (!liveSnapshotPending_) {
    return false;
  }

  const uint32_t nowMs = millis();
  if ((nextLiveAttemptMs_ != 0U &&
       !deadlineReached(nowMs, nextLiveAttemptMs_)) ||
      (lastPublishMs_ != 0U &&
       nowMs - lastPublishMs_ < kMinimumPublishIntervalMs)) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED || !ensureAuthenticated()) {
    const uint32_t delayMs = retryDelayMs_;
    noteRetry();
    nextLiveAttemptMs_ = millis() + delayMs;
    return true;
  }

  // Authentication can take long enough for a newer packet to arrive.
  drainLatestSnapshot();
  if (publish(pendingSnapshot_)) {
    lastPublishMs_ = millis();
    nextLiveAttemptMs_ = 0U;
    liveSnapshotPending_ = false;
    resetRetry();
    return true;
  }

  const uint32_t delayMs = retryDelayMs_;
  noteRetry();
  nextLiveAttemptMs_ = millis() + delayMs;
  return true;
}

void FirebaseBridge::drainLatestSnapshot() {
  if (snapshotQueue_ == nullptr) {
    return;
  }
  LiveTelemetrySnapshot latest{};
  while (xQueueReceive(snapshotQueue_, &latest, 0) == pdTRUE) {
    pendingSnapshot_ = latest;
    liveSnapshotPending_ = true;
  }
}

void FirebaseBridge::notifyWorker(uint32_t notificationBits) {
  if (workerTaskHandle_ != nullptr) {
    xTaskNotify(workerTaskHandle_, notificationBits, eSetBits);
  }
}

void FirebaseBridge::noteRetry() {
  Serial.print("Firebase retry delay: ");
  Serial.print(retryDelayMs_);
  Serial.println(" ms");

  retryDelayMs_ = min(
      retryDelayMs_ * 2,
      kMaximumRetryDelayMs);
}

void FirebaseBridge::resetRetry() {
  retryDelayMs_ = kInitialRetryDelayMs;
}
