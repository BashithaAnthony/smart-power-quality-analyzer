#include "AcquisitionDiagnostics.h"

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "AcquisitionPolicy.h"

namespace {

portMUX_TYPE diagnosticsMux = portMUX_INITIALIZER_UNLOCKED;
AcquisitionDiagnosticsSnapshot counters{};
bool packetResynchronizationPending = false;
bool awaitingFirstSequenceAfterFlush = false;
uint64_t sessionChecksumValidBaseline = 0U;
uint64_t sessionSubmissionBaseline = 0U;
uint64_t sessionAcceptedBaseline = 0U;
uint64_t sessionRejectedBaseline = 0U;

void updateSessionWindowLocked() {
  if (!counters.sessionAdmissionWindowActive) {
    return;
  }
  counters.sessionChecksumValidPackets =
      counters.checksumValidUartPackets - sessionChecksumValidBaseline;
  counters.sessionSubmissionAttempts =
      counters.loggerSubmissionAttempts - sessionSubmissionBaseline;
  counters.sessionAcceptedPackets =
      counters.loggerAcceptedPackets - sessionAcceptedBaseline;
  counters.sessionRejectedPackets =
      counters.loggerRejectedPackets - sessionRejectedBaseline;
}

void countPhaseOperationLocked(AcquisitionFlashOperation operation,
                               AcquisitionRuntimePhase phase) {
  if (operation == AcquisitionFlashOperation::Erase) {
    ++counters.eraseOperationCount;
    if (phase == AcquisitionRuntimePhase::PreparingStorage) {
      ++counters.eraseWhilePreparingCount;
    } else if (phase == AcquisitionRuntimePhase::Active) {
      ++counters.eraseWhileActiveCount;
    } else if (phase == AcquisitionRuntimePhase::Stopping) {
      ++counters.eraseWhileStoppingCount;
    } else if (phase == AcquisitionRuntimePhase::Finalizing) {
      ++counters.eraseWhileFinalizingCount;
    }
  } else if (operation == AcquisitionFlashOperation::Write) {
    ++counters.writeOperationCount;
    if (phase == AcquisitionRuntimePhase::PreparingStorage) {
      ++counters.writeWhilePreparingCount;
    } else if (phase == AcquisitionRuntimePhase::Active) {
      ++counters.writeWhileActiveCount;
    } else if (phase == AcquisitionRuntimePhase::Stopping) {
      ++counters.writeWhileStoppingCount;
    } else if (phase == AcquisitionRuntimePhase::Finalizing) {
      ++counters.writeWhileFinalizingCount;
    }
  }
}

}  // namespace

namespace AcquisitionDiagnostics {

void setPhase(AcquisitionRuntimePhase phase) {
  portENTER_CRITICAL(&diagnosticsMux);
  counters.available = true;
  counters.phase = phase;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void noteChecksumValidUartPacket(uint32_t stm32Sequence) {
  portENTER_CRITICAL(&diagnosticsMux);
  counters.available = true;
  ++counters.checksumValidUartPackets;
  if (packetResynchronizationPending) {
    ++counters.packetResynchronizations;
    packetResynchronizationPending = false;
  }
  if (counters.hasLastStm32Sequence &&
      stm32Sequence != counters.lastStm32Sequence + 1U) {
    // Backwards jumps and STM32 restarts count as one discontinuity rather
    // than an invented multi-billion-packet loss.
    counters.uartSequenceGaps += AcquisitionPolicy::sequenceGapMagnitude(
        counters.lastStm32Sequence, stm32Sequence);
  }
  counters.lastStm32Sequence = stm32Sequence;
  counters.hasLastStm32Sequence = true;
  if (awaitingFirstSequenceAfterFlush) {
    counters.firstSequenceAfterFlush = stm32Sequence;
    counters.hasFirstSequenceAfterFlush = true;
    awaitingFirstSequenceAfterFlush = false;
  }
  updateSessionWindowLocked();
  portEXIT_CRITICAL(&diagnosticsMux);
}

void noteLoggerSubmissionResult(bool accepted) {
  portENTER_CRITICAL(&diagnosticsMux);
  ++counters.loggerSubmissionAttempts;
  if (accepted) {
    ++counters.loggerAcceptedPackets;
  } else {
    ++counters.loggerRejectedPackets;
  }
  updateSessionWindowLocked();
  portEXIT_CRITICAL(&diagnosticsMux);
}

void noteLiveTelemetrySubmission() {
  portENTER_CRITICAL(&diagnosticsMux);
  ++counters.liveTelemetrySubmissions;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void notePacketChecksumFailure() {
  portENTER_CRITICAL(&diagnosticsMux);
  ++counters.packetChecksumFailures;
  packetResynchronizationPending = true;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void notePacketSynchronizationLoss() {
  portENTER_CRITICAL(&diagnosticsMux);
  packetResynchronizationPending = true;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void noteUartFifoOverflow() {
  portENTER_CRITICAL(&diagnosticsMux);
  ++counters.uartFifoOverflows;
  packetResynchronizationPending = true;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void noteUartRingBufferOverflow() {
  portENTER_CRITICAL(&diagnosticsMux);
  ++counters.uartRingBufferOverflows;
  packetResynchronizationPending = true;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void resetSessionAdmissionWindow() {
  portENTER_CRITICAL(&diagnosticsMux);
  counters.sessionAdmissionWindowValid = false;
  counters.sessionAdmissionWindowActive = false;
  counters.sessionChecksumValidPackets = 0U;
  counters.sessionSubmissionAttempts = 0U;
  counters.sessionAcceptedPackets = 0U;
  counters.sessionRejectedPackets = 0U;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void beginSessionAdmissionWindow() {
  portENTER_CRITICAL(&diagnosticsMux);
  sessionChecksumValidBaseline = counters.checksumValidUartPackets;
  sessionSubmissionBaseline = counters.loggerSubmissionAttempts;
  sessionAcceptedBaseline = counters.loggerAcceptedPackets;
  sessionRejectedBaseline = counters.loggerRejectedPackets;
  counters.sessionAdmissionWindowValid = true;
  counters.sessionAdmissionWindowActive = true;
  counters.sessionChecksumValidPackets = 0U;
  counters.sessionSubmissionAttempts = 0U;
  counters.sessionAcceptedPackets = 0U;
  counters.sessionRejectedPackets = 0U;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void endSessionAdmissionWindow() {
  portENTER_CRITICAL(&diagnosticsMux);
  updateSessionWindowLocked();
  counters.sessionAdmissionWindowActive = false;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void beginFlushWindow() {
  portENTER_CRITICAL(&diagnosticsMux);
  counters.flushWindowActive = true;
  counters.hasLastSequenceBeforeFlush = counters.hasLastStm32Sequence;
  counters.lastSequenceBeforeFlush = counters.lastStm32Sequence;
  counters.hasFirstSequenceAfterFlush = false;
  counters.firstSequenceAfterFlush = 0U;
  counters.flushStartChecksumValidPackets =
      counters.checksumValidUartPackets;
  counters.flushEndChecksumValidPackets = 0U;
  counters.flushStartSequenceGaps = counters.uartSequenceGaps;
  counters.flushEndSequenceGaps = 0U;
  awaitingFirstSequenceAfterFlush = false;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void endFlushWindow() {
  portENTER_CRITICAL(&diagnosticsMux);
  if (counters.flushWindowActive) {
    counters.flushWindowActive = false;
    counters.flushEndChecksumValidPackets =
        counters.checksumValidUartPackets;
    counters.flushEndSequenceGaps = counters.uartSequenceGaps;
    awaitingFirstSequenceAfterFlush = true;
  }
  portEXIT_CRITICAL(&diagnosticsMux);
}

AcquisitionFlashToken beginFlashOperation(
    AcquisitionFlashOperation operation) {
  AcquisitionFlashToken token{};
  token.valid = operation != AcquisitionFlashOperation::None;
  token.operation = operation;
  token.startedUs = static_cast<uint64_t>(esp_timer_get_time());
  portENTER_CRITICAL(&diagnosticsMux);
  token.phase = counters.phase;
  token.checksumValidAtStart = counters.checksumValidUartPackets;
  portEXIT_CRITICAL(&diagnosticsMux);
  return token;
}

void endFlashOperation(const AcquisitionFlashToken& token) {
  if (!token.valid) {
    return;
  }
  const uint64_t endedUs = static_cast<uint64_t>(esp_timer_get_time());
  portENTER_CRITICAL(&diagnosticsMux);
  const uint64_t checksumValidAtEnd = counters.checksumValidUartPackets;
  const uint64_t durationUs = endedUs - token.startedUs;
  const uint64_t acquiredDuringOperation =
      checksumValidAtEnd - token.checksumValidAtStart;
  countPhaseOperationLocked(token.operation, token.phase);
  if (token.operation == AcquisitionFlashOperation::Erase) {
    counters.checksumValidDuringEraseOperations += acquiredDuringOperation;
    if (acquiredDuringOperation == 0U) {
      ++counters.eraseOperationsWithNoAcquisitionProgress;
    }
    counters.lastEraseStartUs = token.startedUs;
    counters.lastEraseEndUs = endedUs;
    counters.lastEraseStartChecksumValid = token.checksumValidAtStart;
    counters.lastEraseEndChecksumValid = checksumValidAtEnd;
    counters.lastErasePhase = token.phase;
    if (durationUs > counters.maximumEraseDurationUs) {
      counters.maximumEraseDurationUs = durationUs;
      counters.maximumEraseStartChecksumValid =
          token.checksumValidAtStart;
      counters.maximumEraseEndChecksumValid = checksumValidAtEnd;
    }
  } else if (token.operation == AcquisitionFlashOperation::Write) {
    counters.checksumValidDuringWriteOperations += acquiredDuringOperation;
    if (acquiredDuringOperation == 0U) {
      ++counters.writeOperationsWithNoAcquisitionProgress;
    }
    counters.lastWriteStartUs = token.startedUs;
    counters.lastWriteEndUs = endedUs;
    counters.lastWriteStartChecksumValid = token.checksumValidAtStart;
    counters.lastWriteEndChecksumValid = checksumValidAtEnd;
    counters.lastWritePhase = token.phase;
    if (durationUs > counters.maximumWriteDurationUs) {
      counters.maximumWriteDurationUs = durationUs;
      counters.maximumWriteStartChecksumValid =
          token.checksumValidAtStart;
      counters.maximumWriteEndChecksumValid = checksumValidAtEnd;
    }
  }
  portEXIT_CRITICAL(&diagnosticsMux);
}

AcquisitionDiagnosticsSnapshot getSnapshot() {
  portENTER_CRITICAL(&diagnosticsMux);
  updateSessionWindowLocked();
  const AcquisitionDiagnosticsSnapshot snapshot = counters;
  portEXIT_CRITICAL(&diagnosticsMux);
  return snapshot;
}

}  // namespace AcquisitionDiagnostics

#endif
