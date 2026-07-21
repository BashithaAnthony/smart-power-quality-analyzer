#pragma once

#include <stdint.h>

#ifndef ACQUISITION_POLICY_HOST_TEST
#define ACQUISITION_POLICY_HOST_TEST 0
#endif

constexpr uint32_t ACQUISITION_UART_BAUD = 921600U;
constexpr uint32_t ACQUISITION_UART_BITS_PER_BYTE = 10U;
constexpr uint32_t ACQUISITION_UART_RX_BUFFER_BYTES = 32768U;
constexpr uint32_t ACQUISITION_UART_FIFO_BYTES = 128U;
constexpr uint32_t ACQUISITION_UART_FIFO_THRESHOLD_BYTES = 32U;
constexpr uint32_t ACQUISITION_TARGET_BUFFERED_PAUSE_US = 250000U;

constexpr uint32_t ACQUISITION_FLASH_RECORDS_PER_SERVICE_SLICE = 1U;
constexpr uint32_t ACQUISITION_FLASH_WRITE_SLICE_BYTES = 4096U;
constexpr uint32_t ACQUISITION_FLASH_ERASE_SLICE_BYTES = 4096U;
constexpr uint32_t ACQUISITION_FLASH_SERVICE_YIELD_TICKS = 1U;

enum class AcquisitionStoragePhase : uint8_t {
  Idle = 0,
  PreparingStorage,
  Active,
  Stopping,
  Finalizing,
  Retained,
  Error
};

namespace AcquisitionPolicy {

constexpr uint32_t uartBytesForPauseUs(uint64_t pauseUs) {
  return static_cast<uint32_t>(
      (pauseUs * ACQUISITION_UART_BAUD +
       static_cast<uint64_t>(ACQUISITION_UART_BITS_PER_BYTE) * 1000000ULL -
       1ULL) /
      (static_cast<uint64_t>(ACQUISITION_UART_BITS_PER_BYTE) * 1000000ULL));
}

constexpr uint32_t uartBufferedPauseCapacityUs() {
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(ACQUISITION_UART_RX_BUFFER_BYTES) *
       ACQUISITION_UART_BITS_PER_BYTE * 1000000ULL) /
      ACQUISITION_UART_BAUD);
}

constexpr uint32_t uartFifoInterruptHeadroomUs() {
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(ACQUISITION_UART_FIFO_BYTES -
                             ACQUISITION_UART_FIFO_THRESHOLD_BYTES) *
       ACQUISITION_UART_BITS_PER_BYTE * 1000000ULL) /
      ACQUISITION_UART_BAUD);
}

constexpr uint32_t recordsForInterval(uint32_t seconds,
                                      uint32_t packetsPerSecond = 10U) {
  return seconds * packetsPerSecond;
}

constexpr uint32_t serviceSlicesForRecords(uint32_t recordCount) {
  return recordCount == 0U
             ? 0U
             : (recordCount +
                ACQUISITION_FLASH_RECORDS_PER_SERVICE_SLICE - 1U) /
                   ACQUISITION_FLASH_RECORDS_PER_SERVICE_SLICE;
}

constexpr uint32_t eraseSlicesForSegment(uint32_t segmentBytes) {
  return (segmentBytes + ACQUISITION_FLASH_ERASE_SLICE_BYTES - 1U) /
         ACQUISITION_FLASH_ERASE_SLICE_BYTES;
}

constexpr uint32_t writeSlicesForBytes(uint32_t byteCount) {
  return byteCount == 0U
             ? 0U
             : (byteCount + ACQUISITION_FLASH_WRITE_SLICE_BYTES - 1U) /
                   ACQUISITION_FLASH_WRITE_SLICE_BYTES;
}

constexpr uint32_t segmentsTouched(uint32_t recordCount,
                                   uint32_t recordsPerSegment) {
  return recordCount == 0U
             ? 0U
             : (recordCount + recordsPerSegment - 1U) /
                   recordsPerSegment;
}

constexpr bool acceptedRecordsAccounted(uint64_t accepted,
                                        uint64_t successfullyWritten,
                                        uint64_t fifoPending,
                                        uint64_t workerInFlight) {
  return accepted >= successfullyWritten &&
         accepted - successfullyWritten == fifoPending + workerInFlight;
}

constexpr bool offeredRecordsAccounted(uint64_t offered,
                                       uint64_t successfullyWritten,
                                       uint64_t explicitlyDropped,
                                       uint64_t fifoPending,
                                       uint64_t workerInFlight) {
  return offered >= successfullyWritten + explicitlyDropped &&
         offered - successfullyWritten - explicitlyDropped ==
             fifoPending + workerInFlight;
}

constexpr bool packetAdmissionAllowed(AcquisitionStoragePhase phase,
                                      bool dataAreaPrepared) {
  return phase == AcquisitionStoragePhase::Active && dataAreaPrepared;
}

constexpr bool dataSegmentEraseAllowed(AcquisitionStoragePhase phase,
                                       bool persistentStorageEmpty) {
  return phase == AcquisitionStoragePhase::PreparingStorage &&
         persistentStorageEmpty;
}

constexpr bool preparedCapacityReached(uint64_t acceptedRecords,
                                       uint32_t maximumRecords) {
  return maximumRecords > 0U && acceptedRecords >= maximumRecords;
}

constexpr uint32_t sequenceGapMagnitude(uint32_t previousSequence,
                                        uint32_t currentSequence) {
  return currentSequence - previousSequence == 1U
             ? 0U
             : ((currentSequence - previousSequence > 1U &&
                 currentSequence - previousSequence < 0x80000000U)
                    ? currentSequence - previousSequence - 1U
                    : 1U);
}

}  // namespace AcquisitionPolicy

static_assert(ACQUISITION_UART_RX_BUFFER_BYTES > 8192U,
              "The bounded flash service requires more than two packets of UART buffering");
static_assert(
    AcquisitionPolicy::uartBytesForPauseUs(
        ACQUISITION_TARGET_BUFFERED_PAUSE_US) <
        ACQUISITION_UART_RX_BUFFER_BYTES,
    "The internal UART ring must cover the target bounded scheduling pause");
static_assert(ACQUISITION_UART_FIFO_THRESHOLD_BYTES <
                  ACQUISITION_UART_FIFO_BYTES,
              "The FIFO threshold must leave hardware headroom");
static_assert(ACQUISITION_FLASH_RECORDS_PER_SERVICE_SLICE == 1U,
              "A flash service slice must never drain a large backlog");
