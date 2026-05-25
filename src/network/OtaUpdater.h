#pragma once

#include <functional>
#include <string>

#ifdef SIMULATOR
#include <atomic>
#endif

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  bool render = false;

 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
#ifdef SIMULATOR
    // Required by the crosspoint-simulator lib's simulator_ota.cpp shim. The
    // device build never produces this value — installUpdate() can't be
    // cancelled there.
    CANCELLED_ERROR,
#endif
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  bool getRender() const { return render; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate();

#ifdef SIMULATOR
  // Simulator-only overload. Implementation is provided by the
  // crosspoint-simulator library (simulator_ota.cpp) — it does nothing
  // destructive and is wired up so the OTA UI loop can still be exercised.
  using ProgressCallback = void (*)(void* ctx);
  OtaUpdaterError installUpdate(ProgressCallback onProgress, void* ctx, std::atomic<bool>* cancelRequested);
#endif
};
