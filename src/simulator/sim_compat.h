#pragma once

// Tiny shim header injected via `-include` into the simulator build only.
// Provides ESP-IDF / FreeRTOS APIs that AALU references but the host
// toolchain has no equivalent for. Keep this file minimal — every entry is
// a place AALU code is leaking platform details into nominally portable
// modules. Must remain valid C as well as C++ because PlatformIO injects it
// into both .c and .cpp translation units.

#ifdef SIMULATOR

#include <stdint.h>

// FreeRTOS tick type and tick rate. The crosspoint-simulator lib provides
// `portTICK_PERIOD_MS` via <freertos/FreeRTOS.h> but does NOT define
// `TickType_t` or `xTaskGetTickCount`, both of which AALU references in
// src/stats/ReadingStatsManager.{h,cpp}. We model 1 tick = 1 ms so AALU's
// `(now - start) * portTICK_PERIOD_MS` math keeps producing milliseconds.
typedef uint32_t TickType_t;
// Match the literal the sim's <freertos/FreeRTOS.h> uses (1, not 1U) so an
// identical redefinition is silent rather than -Wmacro-redefined-loud.
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif

#ifdef __cplusplus
#include <chrono>

// Monotonic millisecond counter, started lazily on first call. Mirrors
// FreeRTOS semantics closely enough for session/dwell timing on the host.
inline TickType_t xTaskGetTickCount() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  const auto elapsed = clock::now() - start;
  return static_cast<TickType_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}
#endif  // __cplusplus

#endif  // SIMULATOR
