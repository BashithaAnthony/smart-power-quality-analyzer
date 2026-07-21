#pragma once

#include <stdint.h>

#include "TestConsoleConfig.h"

enum class AcquisitionRuntimePhase : uint8_t {
  Idle = 0,
  PreparingStorage,
  Active,
  Stopping,
  Finalizing,
  Clearing,
  Retained,
  Error
};

enum class AcquisitionFlashOperation : uint8_t {
  None = 0,
  Erase,
  Write
};

struct AcquisitionFlashToken {
  bool valid;
  AcquisitionFlashOperation operation;
  AcquisitionRuntimePhase phase;
  uint64_t startedUs;
  uint64_t checksumValidAtStart;
};

struct AcquisitionDiagnosticsSnapshot {
  bool available;
  bool hasLastStm32Sequence;
  bool sessionAdmissionWindowValid;
  bool sessionAdmissionWindowActive;
  bool flushWindowActive;
  bool hasLastSequenceBeforeFlush;
  bool hasFirstSequenceAfterFlush;
  AcquisitionRuntimePhase phase;
  uint64_t checksumValidUartPackets;
  uint64_t loggerSubmissionAttempts;
  uint64_t loggerAcceptedPackets;
  uint64_t loggerRejectedPackets;
  uint64_t liveTelemetrySubmissions;
  uint64_t packetChecksumFailures;
  uint64_t packetResynchronizations;
  uint64_t uartSequenceGaps;
  uint64_t uartFifoOverflows;
  uint64_t uartRingBufferOverflows;
  uint32_t lastStm32Sequence;
  uint32_t lastSequenceBeforeFlush;
  uint32_t firstSequenceAfterFlush;
  uint64_t flushStartChecksumValidPackets;
  uint64_t flushEndChecksumValidPackets;
  uint64_t flushStartSequenceGaps;
  uint64_t flushEndSequenceGaps;
  uint64_t sessionChecksumValidPackets;
  uint64_t sessionSubmissionAttempts;
  uint64_t sessionAcceptedPackets;
  uint64_t sessionRejectedPackets;
  uint64_t eraseOperationCount;
  uint64_t writeOperationCount;
  uint64_t eraseWhilePreparingCount;
  uint64_t eraseWhileActiveCount;
  uint64_t eraseWhileStoppingCount;
  uint64_t eraseWhileFinalizingCount;
  uint64_t writeWhilePreparingCount;
  uint64_t writeWhileActiveCount;
  uint64_t writeWhileStoppingCount;
  uint64_t writeWhileFinalizingCount;
  uint64_t checksumValidDuringEraseOperations;
  uint64_t checksumValidDuringWriteOperations;
  uint64_t eraseOperationsWithNoAcquisitionProgress;
  uint64_t writeOperationsWithNoAcquisitionProgress;
  uint64_t maximumEraseDurationUs;
  uint64_t maximumWriteDurationUs;
  uint64_t maximumEraseStartChecksumValid;
  uint64_t maximumEraseEndChecksumValid;
  uint64_t maximumWriteStartChecksumValid;
  uint64_t maximumWriteEndChecksumValid;
  uint64_t lastEraseStartUs;
  uint64_t lastEraseEndUs;
  uint64_t lastEraseStartChecksumValid;
  uint64_t lastEraseEndChecksumValid;
  AcquisitionRuntimePhase lastErasePhase;
  uint64_t lastWriteStartUs;
  uint64_t lastWriteEndUs;
  uint64_t lastWriteStartChecksumValid;
  uint64_t lastWriteEndChecksumValid;
  AcquisitionRuntimePhase lastWritePhase;
};

namespace AcquisitionDiagnostics {

#if SESSION_STORAGE_TEST_CONSOLE || SESSION_SYNC_TEST_CONSOLE

void setPhase(AcquisitionRuntimePhase phase);
void noteChecksumValidUartPacket(uint32_t stm32Sequence);
void noteLoggerSubmissionResult(bool accepted);
void noteLiveTelemetrySubmission();
void notePacketChecksumFailure();
void notePacketSynchronizationLoss();
void noteUartFifoOverflow();
void noteUartRingBufferOverflow();
void resetSessionAdmissionWindow();
void beginSessionAdmissionWindow();
void endSessionAdmissionWindow();
void beginFlushWindow();
void endFlushWindow();
AcquisitionFlashToken beginFlashOperation(
    AcquisitionFlashOperation operation);
void endFlashOperation(const AcquisitionFlashToken& token);
AcquisitionDiagnosticsSnapshot getSnapshot();

#else

inline void setPhase(AcquisitionRuntimePhase) {}
inline void noteChecksumValidUartPacket(uint32_t) {}
inline void noteLoggerSubmissionResult(bool) {}
inline void noteLiveTelemetrySubmission() {}
inline void notePacketChecksumFailure() {}
inline void notePacketSynchronizationLoss() {}
inline void noteUartFifoOverflow() {}
inline void noteUartRingBufferOverflow() {}
inline void resetSessionAdmissionWindow() {}
inline void beginSessionAdmissionWindow() {}
inline void endSessionAdmissionWindow() {}
inline void beginFlushWindow() {}
inline void endFlushWindow() {}
inline AcquisitionFlashToken beginFlashOperation(
    AcquisitionFlashOperation operation) {
  return AcquisitionFlashToken{
      false, operation, AcquisitionRuntimePhase::Idle, 0U, 0U};
}
inline void endFlashOperation(const AcquisitionFlashToken&) {}
inline AcquisitionDiagnosticsSnapshot getSnapshot() {
  return AcquisitionDiagnosticsSnapshot{};
}

#endif

}  // namespace AcquisitionDiagnostics
