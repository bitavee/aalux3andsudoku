#pragma once

// Sim-only stub. Matches the namespace + signatures expected by the
// crosspoint-simulator library's simulator_firmware.cpp. The device build
// never sees this file; the sim provides the implementations.

#include <cstdint>

struct esp_partition_t;

namespace ota_boot {

uint32_t computeSeqCrc(uint32_t seq);
bool switchTo(const esp_partition_t* target);

}  // namespace ota_boot
