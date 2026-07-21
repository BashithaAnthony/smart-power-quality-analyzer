/*
 * ESP32-S3 read-only storage diagnostic.
 *
 * Uploading this sketch temporarily replaces the running application firmware.
 * Upload the production firmware again after collecting the diagnostic output.
 *
 * The running sketch only reads chip, heap, OTA, and partition metadata. It
 * never erases or writes flash, mounts or formats a filesystem, modifies NVS or
 * OTA state, creates files, or restarts the device.
 */

#include <Arduino.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <ctype.h>

namespace {

constexpr size_t kMaxPartitions = 96;
constexpr uint64_t kKiB = 1024ULL;
constexpr uint64_t kMiB = 1024ULL * 1024ULL;

esp_partition_t partitions[kMaxPartitions] = {};
size_t partitionCount = 0;
bool partitionListTruncated = false;

const char* safeText(const char* text, const char* fallback = "unknown") {
  if (text == nullptr || text[0] == '\0') {
    return fallback != nullptr ? fallback : "unknown";
  }
  return text;
}

const char* partitionTypeName(esp_partition_type_t type) {
  switch (type) {
    case ESP_PARTITION_TYPE_APP: return "application";
    case ESP_PARTITION_TYPE_DATA: return "data";
    default: return "custom/unknown";
  }
}

const char* partitionSubtypeName(const esp_partition_t& partition) {
  if (partition.type == ESP_PARTITION_TYPE_APP) {
    if (partition.subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
      return "factory";
    }
    if (partition.subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
        partition.subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
      return "OTA application";
    }
    if (partition.subtype == ESP_PARTITION_SUBTYPE_APP_TEST) {
      return "test application";
    }
    return "custom/unknown application";
  }

  if (partition.type == ESP_PARTITION_TYPE_DATA) {
    switch (partition.subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA: return "OTA data";
      case ESP_PARTITION_SUBTYPE_DATA_PHY: return "PHY initialization";
      case ESP_PARTITION_SUBTYPE_DATA_NVS: return "NVS";
      case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "NVS keys";
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
      case ESP_PARTITION_SUBTYPE_DATA_FAT: return "FAT/FFat";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
        return "SPIFFS/LittleFS-compatible";
      default: return "custom/unknown data";
    }
  }

  return "custom/unknown";
}

bool containsIgnoreCase(const char* text, const char* needle) {
  if (text == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }

  for (const char* start = text; *start != '\0'; ++start) {
    const char* left = start;
    const char* right = needle;

    while (*left != '\0' && *right != '\0' &&
           tolower(static_cast<unsigned char>(*left)) ==
               tolower(static_cast<unsigned char>(*right))) {
      ++left;
      ++right;
    }

    if (*right == '\0') {
      return true;
    }
  }

  return false;
}

bool samePartition(const esp_partition_t* left,
                   const esp_partition_t* right) {
  return left != nullptr && right != nullptr &&
         left->type == right->type &&
         left->subtype == right->subtype &&
         left->address == right->address &&
         left->size == right->size;
}

void printHumanSize(uint64_t bytes) {
  if (bytes >= kMiB) {
    Serial.printf("%.2f MiB", static_cast<double>(bytes) / kMiB);
  } else {
    Serial.printf("%.2f KiB", static_cast<double>(bytes) / kKiB);
  }
}

void printByteCount(const char* label, uint64_t bytes) {
  Serial.print(safeText(label));
  Serial.printf(" %llu bytes (", static_cast<unsigned long long>(bytes));
  printHumanSize(bytes);
  Serial.println(")");
}

void addRole(const char* role, bool& hasRole) {
  if (hasRole) {
    Serial.print(", ");
  }
  Serial.print(safeText(role));
  hasRole = true;
}

void printPartitionRoles(const esp_partition_t& partition,
                         const esp_partition_t* running,
                         const esp_partition_t* nextOta) {
  bool hasRole = false;
  Serial.print("    identification: ");

  if (samePartition(&partition, running)) {
    addRole("CURRENTLY RUNNING APPLICATION", hasRole);
  }
  if (samePartition(&partition, nextOta)) {
    addRole("NEXT OTA APPLICATION", hasRole);
  }

  if (partition.type == ESP_PARTITION_TYPE_APP) {
    addRole("application partition", hasRole);
  } else if (partition.type == ESP_PARTITION_TYPE_DATA) {
    switch (partition.subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA:
        addRole("OTA data partition", hasRole);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_NVS:
        addRole("NVS partition", hasRole);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
        addRole("NVS keys partition", hasRole);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_PHY:
        addRole("PHY partition", hasRole);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
        addRole("SPIFFS partition", hasRole);
        addRole("LittleFS-compatible data subtype (contents not inspected)",
                hasRole);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_FAT:
        addRole("FAT/FFat partition", hasRole);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
        addRole("coredump partition", hasRole);
        break;
      default:
        addRole("custom or unknown data partition", hasRole);
        break;
    }

    if (containsIgnoreCase(partition.label, "littlefs")) {
      addRole("LittleFS label detected", hasRole);
    }
  } else {
    addRole("custom or unknown partition type", hasRole);
  }

  if (!hasRole) {
    Serial.print("none");
  }
  Serial.println();
}

void collectPartitions() {
  partitionCount = 0;
  partitionListTruncated = false;

  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_ANY,
      ESP_PARTITION_SUBTYPE_ANY,
      nullptr);

  if (iterator == nullptr) {
    Serial.println("No partition iterator was returned.");
    return;
  }

  while (iterator != nullptr) {
    const esp_partition_t* partition = esp_partition_get(iterator);

    if (partition == nullptr) {
      Serial.println("Partition iterator returned a null entry; enumeration stopped.");
      esp_partition_iterator_release(iterator);
      iterator = nullptr;
      break;
    }

    if (partitionCount >= kMaxPartitions) {
      partitionListTruncated = true;
      Serial.println("Partition buffer is full; remaining entries were not collected.");
      esp_partition_iterator_release(iterator);
      iterator = nullptr;
      break;
    }

    partitions[partitionCount++] = *partition;

    // esp_partition_next() advances and invalidates the current iterator.
    // Assign its result immediately and never use the previous value again.
    esp_partition_iterator_t nextIterator = esp_partition_next(iterator);
    if (nextIterator == nullptr) {
      iterator = nullptr;
      break;
    }
    iterator = nextIterator;
  }

  for (size_t i = 1; i < partitionCount; ++i) {
    const esp_partition_t value = partitions[i];
    size_t position = i;

    while (position > 0 &&
           partitions[position - 1].address > value.address) {
      partitions[position] = partitions[position - 1];
      --position;
    }
    partitions[position] = value;
  }
}

void printBasicChipInformation() {
  Serial.println("=== ESP32-S3 chip and flash ===");
  Serial.print("Chip model:                      ");
  Serial.println(safeText(ESP.getChipModel()));
  Serial.printf("Chip revision:                   %u\n",
                static_cast<unsigned>(ESP.getChipRevision()));
  Serial.printf("CPU frequency:                   %u MHz\n",
                static_cast<unsigned>(ESP.getCpuFreqMHz()));

  const uint32_t flashSize = ESP.getFlashChipSize();
  printByteCount("Flash chip size:", flashSize);

  const uint32_t flashSpeed = ESP.getFlashChipSpeed();
  Serial.printf("Flash speed:                     %u Hz (%.2f MHz)\n",
                static_cast<unsigned>(flashSpeed),
                static_cast<double>(flashSpeed) / 1000000.0);

  Serial.println("[probe] reading flash mode");
  Serial.println("Flash mode runtime query: skipped");
  Serial.println(
      "Reason: ESP.getFlashChipMode() crashes on this ESP32-S3/core 2.0.17 build");
  Serial.println("[probe] flash mode query skipped");
  Serial.println("[probe] flash mode complete");
  Serial.println();
}

void printHeapInformation() {
  Serial.println("=== Heap and PSRAM ===");

  const bool hasPsram = psramFound();
  Serial.print("PSRAM detected:                  ");
  Serial.println(hasPsram ? "yes" : "no");
  printByteCount("Total PSRAM:", ESP.getPsramSize());
  printByteCount("Free PSRAM:", ESP.getFreePsram());
  printByteCount(
      "Largest PSRAM block:",
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  printByteCount(
      "Total internal heap:",
      heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  printByteCount(
      "Free internal heap:",
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  printByteCount(
      "Largest internal heap block:",
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.println();
}

void printOtaPartitionInformation(const esp_partition_t*& running,
                                  const esp_partition_t*& nextOta) {
  running = esp_ota_get_running_partition();
  nextOta = esp_ota_get_next_update_partition(nullptr);

  Serial.println("=== Runtime application partitions ===");
  if (running != nullptr) {
    Serial.print("Currently running: ");
    Serial.print(safeText(running->label, "<empty>"));
    Serial.printf(" at 0x%08lX (%lu bytes)\n",
                  static_cast<unsigned long>(running->address),
                  static_cast<unsigned long>(running->size));
  } else {
    Serial.println("Currently running: not reported");
  }

  if (nextOta != nullptr) {
    Serial.print("Next OTA target:  ");
    Serial.print(safeText(nextOta->label, "<empty>"));
    Serial.printf(" at 0x%08lX (%lu bytes)\n",
                  static_cast<unsigned long>(nextOta->address),
                  static_cast<unsigned long>(nextOta->size));
  } else {
    Serial.println("Next OTA target:  none");
  }
  Serial.println();
}

void printPartitionInformation(const esp_partition_t* running,
                               const esp_partition_t* nextOta) {
  Serial.printf("=== Partition table entries (%u) ===\n",
                static_cast<unsigned>(partitionCount));

  for (size_t i = 0; i < partitionCount; ++i) {
    const esp_partition_t& partition = partitions[i];

    Serial.printf("[%u] label: ", static_cast<unsigned>(i));
    Serial.println(safeText(partition.label, "<empty>"));
    Serial.print("    type: ");
    Serial.print(safeText(partitionTypeName(partition.type)));
    Serial.printf(" (0x%02X)\n", static_cast<unsigned>(partition.type));
    Serial.print("    subtype: ");
    Serial.print(safeText(partitionSubtypeName(partition)));
    Serial.printf(" (0x%02X)\n", static_cast<unsigned>(partition.subtype));
    Serial.printf("    address: 0x%08lX\n",
                  static_cast<unsigned long>(partition.address));
    Serial.printf("    size: %lu bytes (",
                  static_cast<unsigned long>(partition.size));
    printHumanSize(partition.size);
    Serial.println(")");
    Serial.print("    encrypted: ");
    Serial.println(partition.encrypted ? "yes" : "no");
    printPartitionRoles(partition, running, nextOta);
  }

  if (partitionListTruncated) {
    Serial.println("WARNING: more partition entries exist than the probe buffer can store.");
  }
  Serial.println();
}

void printCapacityAndGapSummary() {
  uint64_t applicationCapacity = 0;
  uint64_t dataCapacity = 0;
  uint64_t coveredCapacity = 0;
  const esp_partition_t* largestData = nullptr;

  for (size_t i = 0; i < partitionCount; ++i) {
    const esp_partition_t& partition = partitions[i];
    if (partition.type == ESP_PARTITION_TYPE_APP) {
      applicationCapacity += partition.size;
    } else if (partition.type == ESP_PARTITION_TYPE_DATA) {
      dataCapacity += partition.size;
      if (largestData == nullptr || partition.size > largestData->size) {
        largestData = &partition;
      }
    }
  }

  if (partitionCount > 0) {
    uint64_t coveredEnd =
        static_cast<uint64_t>(partitions[0].address) + partitions[0].size;
    coveredCapacity = partitions[0].size;

    for (size_t i = 1; i < partitionCount; ++i) {
      const uint64_t start = partitions[i].address;
      const uint64_t end = start + partitions[i].size;

      if (start >= coveredEnd) {
        coveredCapacity += end - start;
      } else if (end > coveredEnd) {
        coveredCapacity += end - coveredEnd;
      }

      if (end > coveredEnd) {
        coveredEnd = end;
      }
    }
  }

  Serial.println("=== Capacity summary ===");
  printByteCount("Flash covered by partitions:", coveredCapacity);
  printByteCount("Application capacity:", applicationCapacity);
  printByteCount("Data capacity:", dataCapacity);

  if (largestData != nullptr) {
    Serial.print("Largest data partition:          ");
    Serial.print(safeText(largestData->label, "<empty>"));
    Serial.printf(", %lu bytes (", static_cast<unsigned long>(largestData->size));
    printHumanSize(largestData->size);
    Serial.println(")");
  } else {
    Serial.println("Largest data partition:          none");
  }
  Serial.println();

  Serial.println("=== Apparent unused flash gaps ===");
  Serial.println("Note: bootloader and partition-table regions are not partition entries.");

  bool foundGap = false;
  uint64_t finalEnd = 0;

  if (partitionCount > 0) {
    finalEnd =
        static_cast<uint64_t>(partitions[0].address) + partitions[0].size;

    for (size_t i = 1; i < partitionCount; ++i) {
      const uint64_t start = partitions[i].address;
      const uint64_t end = start + partitions[i].size;

      if (start > finalEnd) {
        const uint64_t gapSize = start - finalEnd;
        Serial.printf("Gap: 0x%08llX - 0x%08llX, %llu bytes (",
                      static_cast<unsigned long long>(finalEnd),
                      static_cast<unsigned long long>(start - 1),
                      static_cast<unsigned long long>(gapSize));
        printHumanSize(gapSize);
        Serial.println(")");
        foundGap = true;
      }

      if (end > finalEnd) {
        finalEnd = end;
      }
    }
  }

  const uint64_t flashSize = ESP.getFlashChipSize();
  if (finalEnd < flashSize) {
    const uint64_t gapSize = flashSize - finalEnd;
    Serial.printf("After final partition: 0x%08llX - 0x%08llX, %llu bytes (",
                  static_cast<unsigned long long>(finalEnd),
                  static_cast<unsigned long long>(flashSize - 1),
                  static_cast<unsigned long long>(gapSize));
    printHumanSize(gapSize);
    Serial.println(")");
    foundGap = true;
  } else if (finalEnd > flashSize) {
    Serial.printf("WARNING: final partition end 0x%08llX exceeds runtime flash size 0x%08llX.\n",
                  static_cast<unsigned long long>(finalEnd),
                  static_cast<unsigned long long>(flashSize));
  }

  if (!foundGap) {
    Serial.println("No gaps found between partition entries or after the final partition.");
  }
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);

  const uint32_t waitStartedMs = millis();
  while (!Serial && millis() - waitStartedMs < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println("ESP32-S3 READ-ONLY STORAGE PROBE");
  Serial.println("This diagnostic does not write, erase, mount, format, or restart.");
  Serial.println("Re-upload the production firmware after recording this output.");
  Serial.println();

  Serial.println("[probe] basic chip information");
  printBasicChipInformation();

  Serial.println("[probe] heap information");
  printHeapInformation();

  const esp_partition_t* runningPartition = nullptr;
  const esp_partition_t* nextOtaPartition = nullptr;
  Serial.println("[probe] OTA partition information");
  printOtaPartitionInformation(runningPartition, nextOtaPartition);

  Serial.println("[probe] partition enumeration");
  collectPartitions();
  printPartitionInformation(runningPartition, nextOtaPartition);

  Serial.println("[probe] gap calculations");
  printCapacityAndGapSummary();

  Serial.println("[probe] completed");
  Serial.println("Storage probe complete. The device will remain idle.");
}

void loop() {
  delay(1000);
}
