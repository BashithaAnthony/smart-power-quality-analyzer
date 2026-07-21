# Smart Power Quality Analyzer — Development Rules

## Existing firmware

The current ESP32-S3 firmware is already verified and working.

The following existing behaviours must not be changed unless explicitly requested:

- TFT display screens and layouts
- Colours, fonts and coordinates
- Touch-screen behaviour
- Rotary encoder behaviour
- Button positions and navigation
- UART configuration
- STM32 packet structure
- Packet start marker detection
- Checksum validation
- Waveform graph rendering
- Existing measurement calculations
- Existing display refresh intervals
- Wi-Fi configuration screens
- Date and time display behaviour

## New development

New functionality must be added through separate modules wherever possible.

Planned modules include:

- FirebaseBridge
- PacketLogger
- RamPacketQueue
- FlashRingBuffer
- CsvSyncManager

## Allowed edits to final_display.ino

Only minimal integration changes are permitted:

1. Add required header includes.
2. Initialize new services inside setup().
3. Pass checksum-validated STM32 packets to the new modules.
4. Call non-blocking service functions from loop().
5. Connect the existing Start Log button.
6. Connect the existing Stop Log button.
7. Connect the existing Push Sync button.
8. Replace placeholder storage values with real logger values while preserving the existing display layout.

## Performance rules

- UART receiving must never wait for Firebase.
- UART receiving must never wait for flash writes.
- Display rendering must never wait for network uploads.
- Firebase operations must run outside the UART receive path.
- Flash writes must be performed in batches.
- Complete STM32 packets must remain in chronological order.
- No packet may be partially written to flash.
- Avoid large temporary String objects.
- Do not store the active log as CSV text.
- Store fixed-size binary packet records during logging.
- Convert binary records to CSV only during synchronisation.

## Logging requirements

One logged record contains:

- ESP32 timestamp
- Complete validated WaveformPacket_t
- All 12 calculated metrics
- Voltage waveform samples
- Current waveform samples
- Voltage harmonics
- Current harmonics
- Packet sequence number
- Record integrity information

Logging flow:

STM32 packet → RAM FIFO → periodic batch write → flash circular FIFO

When flash storage becomes full:

- Remove the oldest complete record.
- Preserve newer records.
- Append the newest record at the logical rear.
- Maintain chronological reading order.

When logging stops:

- Stop accepting records for that session.
- Flush remaining RAM records to flash.
- Finalise the session metadata.

When Push Sync is selected:

- Read flash records from oldest to newest.
- Generate one CSV stream.
- Upload the CSV.
- Confirm successful upload.
- Store session metadata in the database.
- Preserve local data if upload fails.

## Live telemetry

Live website telemetry is separate from historical logging.

The latest 12 scalar measurements and device status may be published continuously, but complete waveform packets must only be uploaded as CSV logging sessions.