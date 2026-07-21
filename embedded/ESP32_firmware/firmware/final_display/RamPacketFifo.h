#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "SessionTypes.h"

class RamPacketFifo {
 public:
  enum class PushResult : uint8_t {
    ACCEPTED = 0,
    FULL,
    BUSY,
    INVALID_ARGUMENT
  };

  enum class ConsumeResult : uint8_t {
    Copied = 0,
    Empty,
    Busy,
    InvalidArgument,
    Changed
  };

  struct ConsumerToken {
    uint32_t slotIndex;
    uint64_t logicalRecordIndex;
  };

  struct Status {
    uint32_t capacity;
    uint32_t occupancy;
    uint32_t highWaterMark;
    uint32_t consumerInFlight;
  };

  RamPacketFifo() = default;
  ~RamPacketFifo();

  RamPacketFifo(const RamPacketFifo&) = delete;
  RamPacketFifo& operator=(const RamPacketFifo&) = delete;

  bool begin(uint32_t capacity);
  bool reset();
  bool isAllocated() const;
  size_t allocatedBytes() const;

  PushResult push(const uint8_t* packetBytes,
                  size_t packetLength,
                  uint64_t captureTimestampUs,
                  uint64_t logicalRecordIndex,
                  uint32_t stm32Sequence);

  // Reserves and copies the oldest published record without exposing PSRAM.
  // commitOldest() removes it only after durable storage succeeds;
  // cancelOldest() republishes it unchanged for a later retry.
  ConsumeResult copyOldest(uint8_t* destination,
                           size_t destinationLength,
                           RamPacketRecordMetadata& metadata,
                           ConsumerToken& token);
  bool commitOldest(const ConsumerToken& token);
  bool cancelOldest(const ConsumerToken& token);

  // Fatal-path recovery only: republish the currently leased oldest slot
  // without advancing the FIFO. This prevents an invariant failure from
  // leaving the ring permanently locked in CONSUMING state.
  bool recoverOldestLease();

  Status getStatus() const;

  // Read-only Stage 1 test helpers. Published slots are immutable until reset.
  // Do not call these concurrently with reset(). No internal storage pointer is
  // ever returned.
  bool getRecordMetadata(uint32_t ordinal,
                         RamPacketRecordMetadata& metadata) const;
  bool copyPacketBytes(uint32_t ordinal,
                       uint8_t* destination,
                       size_t destinationLength) const;
  bool packetBytesEqual(uint32_t ordinal,
                        const uint8_t* expected,
                        size_t expectedLength) const;

 private:
  enum class SlotState : uint8_t {
    EMPTY = 0,
    RESERVED,
    PUBLISHED,
    CONSUMING,
    INSPECTING
  };

  struct RamPacketSlot {
    uint64_t captureTimestampUs;
    uint64_t logicalRecordIndex;
    uint32_t stm32Sequence;
    uint8_t packetBytes[SESSION_PACKET_BYTES];
    SlotState state;
    uint8_t reserved;
  };

  static_assert(sizeof(RamPacketSlot) == SESSION_FIFO_SLOT_BYTES,
                "A Stage 1 PSRAM FIFO slot must remain 4376 bytes");

  void release();

  RamPacketSlot* slots_ = nullptr;
  uint32_t capacity_ = 0;
  uint32_t readIndex_ = 0;
  uint32_t writeIndex_ = 0;
  uint32_t occupancy_ = 0;
  uint32_t highWaterMark_ = 0;
  bool reservationActive_ = false;
  bool consumerReservationActive_ = false;
  mutable bool inspectionReservationActive_ = false;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
