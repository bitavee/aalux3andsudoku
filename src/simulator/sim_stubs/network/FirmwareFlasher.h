#pragma once

// Sim-only stub. AALU has no firmware-flashing UI, but the crosspoint-
// simulator library compiles simulator_firmware.cpp which expects this
// declaration to exist in `network/FirmwareFlasher.h` of the consuming
// firmware. We provide the contract so the sim's stub implementation links;
// the device build never sees this file.

#include <cstddef>
#include <cstdint>

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,
  BAD_CHECKSUM,
  BAD_SHA,
  BAD_SIZE,
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

using ProgressCb = void (*)(size_t bytesDone, size_t bytesTotal, void* ctx);

Result flashFromSdPath(const char* path, ProgressCb onProgress, void* ctx, bool dryRun = false);
Result validateImageFile(const char* path, size_t expectedSize);
const char* resultName(Result r);

}  // namespace firmware_flash
