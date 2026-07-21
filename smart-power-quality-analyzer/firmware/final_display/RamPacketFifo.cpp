#include "RamPacketFifo.h"

#include <cstring>
#include <esp_heap_caps.h>

static_assert(SESSION_PACKET_BYTES == 4354,
              "The RAM logger packet format must remain 4354 bytes");
static_assert(sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8,
              "FIFO indices and metadata require fixed-width integers");

RamPacketFifo::~RamPacketFifo() {
  release();
}

bool RamPacketFifo::begin(uint32_t capacity) {
  if (slots_ != nullptr) {
    return capacity_ == capacity;
  }

  if (capacity == 0 ||
      capacity > (SIZE_MAX / sizeof(RamPacketSlot))) {
    return false;
  }

  const size_t bytes =
      static_cast<size_t>(capacity) * sizeof(RamPacketSlot);

  slots_ = static_cast<RamPacketSlot*>(heap_caps_malloc(
      bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (slots_ == nullptr) {
    return false;
  }

  capacity_ = capacity;
  if (!reset()) {
    release();
    return false;
  }
  return true;
}

bool RamPacketFifo::reset() {
  portENTER_CRITICAL(&mux_);

  if (reservationActive_ || consumerReservationActive_ ||
      inspectionReservationActive_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  readIndex_ = 0;
  writeIndex_ = 0;
  occupancy_ = 0;
  highWaterMark_ = 0;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool RamPacketFifo::isAllocated() const {
  portENTER_CRITICAL(&mux_);
  const bool allocated = slots_ != nullptr;
  portEXIT_CRITICAL(&mux_);
  return allocated;
}

size_t RamPacketFifo::allocatedBytes() const {
  portENTER_CRITICAL(&mux_);
  const size_t bytes =
      static_cast<size_t>(capacity_) * sizeof(RamPacketSlot);
  portEXIT_CRITICAL(&mux_);
  return bytes;
}

RamPacketFifo::PushResult RamPacketFifo::push(
    const uint8_t* packetBytes,
    size_t packetLength,
    uint64_t captureTimestampUs,
    uint64_t logicalRecordIndex,
    uint32_t stm32Sequence) {
  if (packetBytes == nullptr || packetLength != SESSION_PACKET_BYTES) {
    return PushResult::INVALID_ARGUMENT;
  }

  uint32_t reservedIndex = 0;

  portENTER_CRITICAL(&mux_);

  if (slots_ == nullptr || capacity_ == 0) {
    portEXIT_CRITICAL(&mux_);
    return PushResult::INVALID_ARGUMENT;
  }

  if (occupancy_ >= capacity_) {
    portEXIT_CRITICAL(&mux_);
    return PushResult::FULL;
  }

  if (reservationActive_) {
    portEXIT_CRITICAL(&mux_);
    return PushResult::BUSY;
  }

  reservationActive_ = true;
  reservedIndex = writeIndex_;
  slots_[reservedIndex].state = SlotState::RESERVED;
  portEXIT_CRITICAL(&mux_);

  RamPacketSlot& slot = slots_[reservedIndex];
  slot.captureTimestampUs = captureTimestampUs;
  slot.logicalRecordIndex = logicalRecordIndex;
  slot.stm32Sequence = stm32Sequence;
  memcpy(slot.packetBytes, packetBytes, SESSION_PACKET_BYTES);

  portENTER_CRITICAL(&mux_);
  slot.state = SlotState::PUBLISHED;
  writeIndex_ = (writeIndex_ + 1U) % capacity_;
  ++occupancy_;
  if (occupancy_ > highWaterMark_) {
    highWaterMark_ = occupancy_;
  }
  reservationActive_ = false;
  portEXIT_CRITICAL(&mux_);

  return PushResult::ACCEPTED;
}

RamPacketFifo::ConsumeResult RamPacketFifo::copyOldest(
    uint8_t* destination,
    size_t destinationLength,
    RamPacketRecordMetadata& metadata,
    ConsumerToken& token) {
  if (destination == nullptr ||
      destinationLength != SESSION_PACKET_BYTES) {
    return ConsumeResult::InvalidArgument;
  }

  portENTER_CRITICAL(&mux_);

  if (slots_ == nullptr || capacity_ == 0) {
    portEXIT_CRITICAL(&mux_);
    return ConsumeResult::InvalidArgument;
  }
  if (occupancy_ == 0) {
    portEXIT_CRITICAL(&mux_);
    return ConsumeResult::Empty;
  }
  if (consumerReservationActive_) {
    portEXIT_CRITICAL(&mux_);
    return ConsumeResult::Busy;
  }

  RamPacketSlot& slot = slots_[readIndex_];
  if (slot.state != SlotState::PUBLISHED) {
    portEXIT_CRITICAL(&mux_);
    return ConsumeResult::Busy;
  }

  consumerReservationActive_ = true;
  slot.state = SlotState::CONSUMING;
  token.slotIndex = readIndex_;
  token.logicalRecordIndex = slot.logicalRecordIndex;
  metadata.captureTimestampUs = slot.captureTimestampUs;
  metadata.logicalRecordIndex = slot.logicalRecordIndex;
  metadata.stm32Sequence = slot.stm32Sequence;
  portEXIT_CRITICAL(&mux_);

  memcpy(destination, slot.packetBytes, SESSION_PACKET_BYTES);

  portENTER_CRITICAL(&mux_);
  const bool unchanged =
      consumerReservationActive_ && token.slotIndex == readIndex_ &&
      slot.state == SlotState::CONSUMING &&
      slot.logicalRecordIndex == token.logicalRecordIndex;
  if (!unchanged) {
    if (slot.state == SlotState::CONSUMING) {
      slot.state = SlotState::PUBLISHED;
    }
    consumerReservationActive_ = false;
  }
  portEXIT_CRITICAL(&mux_);

  return unchanged ? ConsumeResult::Copied : ConsumeResult::Changed;
}

bool RamPacketFifo::commitOldest(const ConsumerToken& token) {
  portENTER_CRITICAL(&mux_);

  if (!consumerReservationActive_ || occupancy_ == 0 ||
      token.slotIndex != readIndex_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  RamPacketSlot& slot = slots_[readIndex_];
  if (slot.state != SlotState::CONSUMING ||
      slot.logicalRecordIndex != token.logicalRecordIndex) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  slot.state = SlotState::EMPTY;
  readIndex_ = (readIndex_ + 1U) % capacity_;
  --occupancy_;
  consumerReservationActive_ = false;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool RamPacketFifo::cancelOldest(const ConsumerToken& token) {
  portENTER_CRITICAL(&mux_);

  if (!consumerReservationActive_ || occupancy_ == 0 ||
      token.slotIndex != readIndex_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  RamPacketSlot& slot = slots_[readIndex_];
  if (slot.state != SlotState::CONSUMING ||
      slot.logicalRecordIndex != token.logicalRecordIndex) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  slot.state = SlotState::PUBLISHED;
  consumerReservationActive_ = false;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool RamPacketFifo::recoverOldestLease() {
  portENTER_CRITICAL(&mux_);

  if (!consumerReservationActive_ || slots_ == nullptr ||
      occupancy_ == 0U || capacity_ == 0U) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  RamPacketSlot& slot = slots_[readIndex_];
  if (slot.state != SlotState::CONSUMING) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  slot.state = SlotState::PUBLISHED;
  consumerReservationActive_ = false;
  portEXIT_CRITICAL(&mux_);
  return true;
}

RamPacketFifo::Status RamPacketFifo::getStatus() const {
  Status status{};
  portENTER_CRITICAL(&mux_);
  status.capacity = capacity_;
  status.occupancy = occupancy_;
  status.highWaterMark = highWaterMark_;
  status.consumerInFlight = consumerReservationActive_ ? 1U : 0U;
  portEXIT_CRITICAL(&mux_);
  return status;
}

bool RamPacketFifo::getRecordMetadata(
    uint32_t ordinal,
    RamPacketRecordMetadata& metadata) const {
  portENTER_CRITICAL(&mux_);
  if (slots_ == nullptr || capacity_ == 0U || ordinal >= occupancy_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint32_t slotIndex = (readIndex_ + ordinal) % capacity_;
  const RamPacketSlot& slot = slots_[slotIndex];
  if (slot.state != SlotState::PUBLISHED) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  metadata.captureTimestampUs = slot.captureTimestampUs;
  metadata.logicalRecordIndex = slot.logicalRecordIndex;
  metadata.stm32Sequence = slot.stm32Sequence;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool RamPacketFifo::copyPacketBytes(
    uint32_t ordinal,
    uint8_t* destination,
    size_t destinationLength) const {
  if (destination == nullptr ||
      destinationLength != SESSION_PACKET_BYTES) {
    return false;
  }

  portENTER_CRITICAL(&mux_);
  if (slots_ == nullptr || capacity_ == 0U || ordinal >= occupancy_ ||
      inspectionReservationActive_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint32_t slotIndex = (readIndex_ + ordinal) % capacity_;
  RamPacketSlot& slot = slots_[slotIndex];
  if (slot.state != SlotState::PUBLISHED) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint64_t logicalRecordIndex = slot.logicalRecordIndex;
  slot.state = SlotState::INSPECTING;
  inspectionReservationActive_ = true;
  portEXIT_CRITICAL(&mux_);

  memcpy(destination, slot.packetBytes, SESSION_PACKET_BYTES);

  portENTER_CRITICAL(&mux_);
  const bool unchanged =
      inspectionReservationActive_ &&
      slot.state == SlotState::INSPECTING &&
      slot.logicalRecordIndex == logicalRecordIndex;
  if (slot.state == SlotState::INSPECTING) {
    slot.state = SlotState::PUBLISHED;
  }
  inspectionReservationActive_ = false;
  portEXIT_CRITICAL(&mux_);
  return unchanged;
}

bool RamPacketFifo::packetBytesEqual(
    uint32_t ordinal,
    const uint8_t* expected,
    size_t expectedLength) const {
  if (expected == nullptr || expectedLength != SESSION_PACKET_BYTES) {
    return false;
  }

  portENTER_CRITICAL(&mux_);
  if (slots_ == nullptr || capacity_ == 0U || ordinal >= occupancy_ ||
      inspectionReservationActive_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint32_t slotIndex = (readIndex_ + ordinal) % capacity_;
  RamPacketSlot& slot = slots_[slotIndex];
  if (slot.state != SlotState::PUBLISHED) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint64_t logicalRecordIndex = slot.logicalRecordIndex;
  slot.state = SlotState::INSPECTING;
  inspectionReservationActive_ = true;
  portEXIT_CRITICAL(&mux_);

  const bool equal =
      memcmp(slot.packetBytes, expected, SESSION_PACKET_BYTES) == 0;

  portENTER_CRITICAL(&mux_);
  const bool unchanged =
      inspectionReservationActive_ &&
      slot.state == SlotState::INSPECTING &&
      slot.logicalRecordIndex == logicalRecordIndex;
  if (slot.state == SlotState::INSPECTING) {
    slot.state = SlotState::PUBLISHED;
  }
  inspectionReservationActive_ = false;
  portEXIT_CRITICAL(&mux_);
  return equal && unchanged;
}

void RamPacketFifo::release() {
  RamPacketSlot* slots = nullptr;

  portENTER_CRITICAL(&mux_);
  slots = slots_;
  slots_ = nullptr;
  capacity_ = 0;
  readIndex_ = 0;
  writeIndex_ = 0;
  occupancy_ = 0;
  highWaterMark_ = 0;
  reservationActive_ = false;
  consumerReservationActive_ = false;
  inspectionReservationActive_ = false;
  portEXIT_CRITICAL(&mux_);

  if (slots != nullptr) {
    heap_caps_free(slots);
  }
}
