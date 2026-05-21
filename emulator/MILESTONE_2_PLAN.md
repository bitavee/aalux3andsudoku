# Milestone 2 Plan — Logic-only host port (Path C)

Goal: compile `src/` + `lib/` natively against host shims and replace the milestone-1 file browser in `emulator/native/main.cpp` with the real `currentActivity` lifecycle.

Estimated effort: **2–3 days** (see audit below). Order is (a) shims-first: get one library compiling green before touching the rest.

---

## Audit summary

### Arduino API actually called in `lib/Epub` + `lib/EpdFont` + `lib/GfxRenderer`

| Symbol | Sites | Shim |
|---|---|---|
| `millis()` / `micros()` | dozens (timing/profiling) | `std::chrono::steady_clock` |
| `delay(ms)` | 2 (`Section.cpp:154`, `BookMetadataCache.cpp:347` — retry waits) | `std::this_thread::sleep_for` |
| `ESP.getFreeHeap()` | 4 (allocation gates) | return constant (simulate 200 KB or `SIZE_MAX`) |

Confirmed **absent** from these three libs: `Serial.*`, `pinMode`/`digitalRead`/`digitalWrite`, Arduino `String`, `IRAM_ATTR`, `DRAM_ATTR`, `PROGMEM`, `F()`, `pgm_read_*`, `yield()`.

### Image codecs

`JPEGDEC` and `PNGdec` are Arduino libraries but are pure C++ classes with file callbacks. Vendor them as-is into `emulator/native/third_party/` and stub their `<Arduino.h>` include. **Do not** swap for `stb_image` — firmware code is tuned around JPEGDEC's `JPEG_SCALE_EIGHTH` and PNGdec's scanline buffer; replacing the codec changes decode behaviour and breaks cache compatibility.

### HAL seam

`lib/Epub` never touches SdFat directly — it goes through `HalFile` (via `using FsFile = HalFile` in `lib/hal/HalStorage.h:101`). Reimplement `HalFile` against `std::fstream` + `std::filesystem`.

API surface to cover (from `HalStorage.h:61-95`):
- `open` / `close` / `isOpen` / `operator bool`
- `read(buf, count)`, `read()` (single byte), `write(buf, count)`, `write(uint8_t)`
- `seek(pos)`, `seekCur(off)`, `seekSet(off)`, `position()`, `available()`
- `size()`, `fileSize()`
- `getName(buf, len)`, `rename(newPath)`, `isDirectory()`, `rewindDirectory()`, `openNextFile()`
- `flush()`

Plus `HalStorage`: `begin`, `ready`, `listFiles`, `readFile`, `readFileToStream`, `readFileToBuffer`, `writeFile`, `ensureDirectoryExists`, `mkdir`, `exists`, `remove`, `rename`, `rmdir`, `removeDir`, `openFileForRead`/`Write` overloads.

Header also pulls in Arduino `String` + `Print`. Minimal shims needed (~50 lines each).

### FreeRTOS surface across **all** `src` + `lib` (only 12 files total)

| Primitive | Sites | Shim |
|---|---|---|
| `xTaskCreate` | 3 (ActivityManager render, KOReaderSync I/O, TxtReader) | run synchronously (no real thread) — single-core RISC-V semantics aren't reproducible on host |
| `xSemaphoreCreateMutex` / `xSemaphoreTake` / `xSemaphoreGive` | 3 mutexes (ActivityManager, HalStorage, HalPowerManager) | `std::mutex` via macro shim |
| `vTaskDelay` | 3 (KOReaderSync, TxtReader yield, retry waits) | `std::this_thread::sleep_for` |
| `xTaskGetTickCount` / `portTICK_PERIOD_MS` | 1 (`ReadingStatsManager.cpp:152`) | `millis()` / `1` |

### What gets `#ifdef`'d out (`-DEMULATOR=1`)

- `HalDisplay` hardware ops — feed straight into `emulator/native/main.cpp`'s 48 KB `Framebuffer::bytes`.
- `HalGPIO` interrupt setup — WS button events drive `MappedInputManager` directly.
- `HalPowerManager`, `HalSystem` (panic, sleep, brownout).
- Entire `src/network/` subtree (Wifi, OTA, OPDS, WebDAV, HttpDownloader).
- `BootActivity`, `SleepActivity` (skip — jump straight to `HomeActivity`).

---

## Implementation order

Shims-first. Get **one** library compiling green before touching the rest. Aim for a green `lib/EpdFont` target at the half-day mark.

### Day 0.5 — Arduino shim header pack

Create `emulator/native/shims/`:

- `Arduino.h` — `millis()`, `micros()`, `delay()`, `delayMicroseconds()`, types (`uint8_t` etc. via `<cstdint>`), `yield()` (no-op), `ESP` struct with `getFreeHeap()` / `getFreePsram()` / `restart()`.
- `Print.h` — minimal abstract class with `write(uint8_t)` pure virtual and `printf`/`print`/`println` defaults.
- `WString.h` (or inline in `Arduino.h`) — minimal Arduino-compatible `String` class: ctor from `const char*` / `int` / `float`, `+=`, `c_str()`, `length()`, `operator[]`, `substring`, `indexOf`. ~80 lines.
- `freertos/FreeRTOS.h`, `freertos/semphr.h`, `freertos/task.h`, `freertos/queue.h` — typedef `SemaphoreHandle_t = std::mutex*`, macros for `xSemaphoreTake/Give`, `vTaskDelay` → `sleep_for`, `xTaskCreate` → call function inline or skip.
- `esp_*.h` stubs — empty headers so `#include <esp_system.h>` etc. compile.

**Verify**: `g++ -c emulator/native/shims/Arduino.h` parses (sanity check the inline templates).

### Day 0.5 — `HalFile` host impl

`emulator/native/shims/HalFile.host.cpp` — full impl backed by `std::fstream` + `std::filesystem`. Drop-in for `lib/hal/HalStorage.cpp`. The trickiest bits:
- `openNextFile()` — directory iteration with sticky state. Use `std::filesystem::directory_iterator` stored in the impl.
- `available()` returning `int` — clamp at `INT_MAX`.
- `oflag_t` from `<common/FsApiConstants.h>` — replicate the constants (`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`) in the shim.

**Verify**: a 30-line unit test that opens `emulator/sdcard/`, lists files, reads one back.

### Day 0.5 — Vendor JPEGDEC + PNGdec

- Find the upstream URLs in `platformio.ini` `lib_deps` and copy the sources into `emulator/native/third_party/JPEGDEC/` + `PNGdec/`.
- Patch their `<Arduino.h>` include to point at our shim.
- Compile each as a static lib in CMake.

**Risk probe** (do this first, 30 min): `grep -rE '#include <(HardwareSerial|Wire|SPI)\.h>' JPEGDEC PNGdec` — if hits, escalate.

### Day 0.5 — First compile target

`emulator/native/CMakeLists.txt`: add a new target `aalu_libs` that compiles **only `lib/EpdFont`** (~20 .cpp files, smallest of the three target libs, no codec deps). Include dirs:

```cmake
target_include_directories(aalu_libs PRIVATE
  ${CMAKE_SOURCE_DIR}/native/shims
  ${CMAKE_SOURCE_DIR}/../lib/EpdFont
  ${CMAKE_SOURCE_DIR}/../lib/EpdFont/builtinFonts
)
target_compile_definitions(aalu_libs PRIVATE EMULATOR=1)
```

Iterate until it builds clean. Likely fixes: a few missing `<cstring>` includes, possibly a `String` method that's not in our minimal shim — add it.

### Day 1 — Add `lib/GfxRenderer` then `lib/Epub`

Same CMake target, expand source globs. `lib/GfxRenderer` brings in JPEGDEC/PNGdec — link those vendored libs. `lib/Epub` is the biggest (expat, hyphenation, parsers) but should mostly Just Work once the shims are right.

Expected hiccups:
- `lib/Logging` macros (`LOG_DBG`, `LOG_ERR`) — probably wrap `Serial.printf`. Redirect to `fprintf(stderr, ...)` via shim. 10 lines.
- Anonymous structs with `__attribute__((packed))` — already cross-compiler, should be fine.
- `lib/expat`, `lib/picojpeg`, `lib/uzlib` — vendored C libraries, should compile clean.

### Day 1 — `HalDisplay` + `HalGPIO` host backends

`emulator/native/shims/HalDisplay.host.cpp`:
- `displayBuffer()` → memcpy into our `Framebuffer::bytes` then mark dirty.
- `refresh()` / `partialRefresh()` → no-op (canvas repaints instantly).
- `getScreenWidth()` / `getScreenHeight()` → return `fb.logicalW()` / `fb.logicalH()`.

`emulator/native/shims/HalGPIO.host.cpp`:
- Maintain a button-state array updated from WS messages.
- `digitalRead(BTN_*)` → return state.
- ISR registration → no-op; we poll from the main loop instead.

### Day 0.5 — Wire activity loop into `main.cpp`

Replace the milestone-1 `UIState::render()` call with:

```cpp
ActivityManager am(renderer, mappedInput);
am.enter(new HomeActivity());
while (running) {
  pumpWsEvents();           // drains incoming button messages into HalGPIO state
  am.tick();                // calls currentActivity->loop()
  if (renderer.dirty()) {
    flushToClients();       // existing WS broadcast
  }
  std::this_thread::sleep_for(10ms);
}
```

Ifdef-gate `src/main.cpp::setup()` heavily — most of it is hardware bring-up we skip. Keep only: font registration, `SETTINGS` load from `/sdcard`, initial `HomeActivity` push.

### Day 0.5 — Buffer

Real schedule slips. First-compile errors take longer than estimated, especially around lib/Epub. Reserve a half-day before declaring M2 done.

---

## Open risks (resolve early)

1. **JPEGDEC/PNGdec transitive Arduino deps** — 30-min probe before Day 0.5 codec work. If they reach for `HardwareSerial.h`/`Wire.h`/`SPI.h`, escalate to a deeper shim or consider patching the libs.
2. **`Activity.cpp` HAL touches** — won't know the full reach until first compile. Budget Day 1 buffer for this.
3. **`src/main.cpp::setup()` ifdef sprawl** — could grow to >50 ifdef'd lines. If so, factor a `setupForEmulator()` function instead of inline guards.
4. **`lib/Epub` CSS parser** — grep showed no Arduino `String` usage, but worth re-confirming once it's in the build (`-fsyntax-only` pass first).
5. **Cache binary compatibility** — emulator must lock to 800×480 + same font/size/margin/orientation as device for `.crosspoint/` cache interchange. Document in `emulator/README.md` once M2 lands.

---

## Definition of done (M2)

- `make emulator` builds a binary that loads the real `currentActivity` lifecycle.
- `HomeActivity` renders on the emulator canvas — actual UITheme-rendered home, not the milestone-1 stub.
- Front buttons navigate the real home screen.
- Opening an EPUB launches `EpubReaderActivity` and renders at least the first page.
- All four orientations work (server-side rotation already in place from previous session).
- `emulator/README.md` updated: milestone-1 → milestone-2, drop the "What does NOT work yet" → "actual firmware" bullet.

Not in scope for M2: Wifi, OTA, OPDS, dictionary lookup over network, KOReader sync, deep sleep. These stay `#ifdef`'d out.
