# AALU Development Guide

Project: Open-source e-reader firmware for the **Xteink X4** (ESP32-C3).
Origin: Originally forked from [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader); now an independent system with its own UI, Dictionary engine, KOReader sync, and Reading Stats.
Mission: Provide a lightweight, high-performance, *personalized* reading experience focused on EPUB rendering on constrained hardware.
Repo root: `/Users/disrael/Projects/personal/xteink/aalu`
Current firmware version: see `[aalu] version` in `platformio.ini` (currently `1.0.7`).

> Note: Many code-level identifiers (e.g. `CrossPointSettings`, `CrossPointState`, the `.crosspoint/` cache directory) intentionally retain the upstream names for backward compatibility with existing on-device caches. **Do not rename them** unless you also ship a binary migration path (see `stats.bin` migration in [Stats refactor](#aalu-specific-features)).

---

## AI Agent Identity and Cognitive Rules
* Role: Senior Embedded Systems Engineer (ESP-IDF / Arduino-ESP32 specialised).
* Primary Constraint: 380 KB RAM is the hard ceiling. Stability is non-negotiable.
* Evidence-Based Reasoning: Before proposing a change, you MUST cite the specific file path and line numbers that justify the modification.
* Anti-Hallucination: Do not assume the existence of libraries or ESP-IDF functions. If unsure of an API's availability for the ESP32-C3 RISC-V target, check `open-x4-sdk/` or official docs first.
* No Unfounded Claims: Do not claim performance gains or memory savings without explaining the technical mechanism (e.g. DRAM vs IRAM usage).
* Resource Justification: Justify any new heap allocation (`new`, `malloc`, `std::vector`) or explain why a stack/static alternative was rejected.
* Verification: After suggesting a fix, instruct the user how to verify it (e.g. monitor heap via Serial, delete a specific cache file, test in all four orientations).
* **Always Build + Test After Changes**: Any code change MUST be followed by a successful `pio run` (default env) AND running every available host test script under `test/`. No exceptions — even a one-line edit can break the build (missing include, unbalanced `#ifdef`, lock-file drift). See [Testing and Verification Workflow](#testing-and-verification-workflow) for the exact commands. Do not declare a task complete, do not propose a commit, and do not hand off to the user until both the build and tests are green.

---

## Development Environment Awareness

**CRITICAL**: Detect the host platform at session start to choose appropriate tools and commands.

```bash
uname -s
# Returns: Darwin (macOS), Linux, MINGW64_NT-* (Windows Git Bash)
```

### Platform-Specific Behaviours
- **macOS (this repo's primary host)**: Unix paths. Serial port is typically `/dev/cu.usbmodem*`. Pass it explicitly to the monitor: `python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101`.
- **Linux/WSL**: Full bash; serial port typically `/dev/ttyUSB*` or `/dev/ttyACM*`.
- **Windows (Git Bash)**: Unix commands but Windows-style paths under the hood; limited glob — use `find ... | xargs` for batch ops.

**Cross-platform code formatting**:
```bash
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

---

## Platform and Hardware Constraints

### Hardware Specs
* MCU: ESP32-C3 (single-core RISC-V @ 160 MHz)
* RAM: ~380 KB usable (VERY LIMITED — primary project constraint)
  * **NO PSRAM** (unlike ESP32-S3)
  * **Single-buffer mode**: only ONE 48 KB framebuffer
* Flash: 16 MB
* Display: 800 × 480 E-Ink (slow refresh, monochrome, 1–2 s full update)
  * Framebuffer: 48,000 bytes (800 × 480 ÷ 8)
* Storage: SD Card (books + aggressive caching)

### The Resource Protocol
1. **Stack Safety**: Limit local function variables to < 256 bytes. The ESP32-C3 default task stack is small; use `std::unique_ptr` or static pools for larger buffers.
2. **Heap Fragmentation**: Avoid repeated `new`/`delete` in loops. Allocate once during `onEnter()` and reuse.
3. **Flash Persistence**: Large constant data (UI strings, lookup tables) MUST be `static const` / `static constexpr` so they live in Flash, not DRAM.
4. **String Policy**: Prohibit `std::string` and Arduino `String` in hot paths. Use `std::string_view` for read-only access; `snprintf` into fixed `char[]` buffers for construction.
5. **UI Strings**: All user-facing text must use the `tr()` macro (e.g. `tr(STR_LOADING)`) for i18n. **Never hardcode UI strings.** Logging messages (`LOG_DBG`/`LOG_ERR`) may be hardcoded.
6. **`constexpr` First**: Compile-time constants and lookup tables must be `constexpr`, not just `static const`. Moves computation to compile time, enables dead-branch elimination, guarantees flash placement.
7. **`std::vector` Pre-allocation**: Always call `.reserve(N)` before any `push_back()` loop. Each growth event = alloc-new + copy + free-old → three heap ops that fragment DRAM. Estimate conservatively when the final size is unknown.
8. **SPIFFS / SD Write Throttling**: Never write a settings file on every user interaction. Guard with a value-change check (`if (newVal == _current) return;`). Progress saves during reading must be debounced — write on activity exit or every N page turns, not every turn. Flash sectors have a finite erase-cycle limit. AALU's Quick Settings (Aa) overlay is built on this principle: "deferred SD card writes to protect flash lifespan".

---

## Project Architecture

### Build System: PlatformIO

PlatformIO is BOTH a VS Code extension AND a CLI tool.

1. **VS Code Extension** (recommended for daily dev):
   * Extension ID: `platformio.platformio-ide`
   * Toolbar: Build (✓), Upload (→), Monitor (🔌)
2. **CLI** (`pio` command):
   * Install: `pip install platformio` (or `pipx install platformio`)
   * Verify: `which pio`

**Configuration files**:
* `platformio.ini` — committed, shared project config
* `platformio.local.ini` — gitignored, personal overrides (serial port, debug flags). Already referenced via `extra_configs = platformio.local.ini`
* `partitions.csv` — ESP32 flash partition layout

### Build Environments (`platformio.ini`)
| Env | Purpose | Log Level | Notes |
|---|---|---|---|
| `default` | Local development | 2 | Serial logging on, fastest iteration |
| `gh_release` | Production release | 0 | Released artefact |
| `gh_release_rc` | Release candidate | 1 | RC builds |
| `slim` | Minimal build | — | No serial logging |

### Critical Build Flags
These flags fundamentally shape firmware behaviour. Do not change them lightly.

```cpp
-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1   // Single 48 KB framebuffer (saves 48 KB)
-DARDUINO_USB_MODE=1                  // Enable USB CDC
-DARDUINO_USB_CDC_ON_BOOT=1           // Serial available immediately at boot
-DXML_CONTEXT_BYTES=1024              // EPUB XML parser memory cap
-DXML_GE=0                            // Disable XML general entities (security)
-DUSE_UTF8_LONG_NAMES=1               // SD card long-filename support
-DPNG_MAX_BUFFERED_PIXELS=16416       // Allow up to 2048px-wide PNG scanlines
-DAALU_VERSION=\"1.0.7\"        // Set from [aalu] version
-fno-exceptions                       // No C++ exceptions
-std=gnu++2a                          // C++20
```

`-Wl,--wrap=panic_print_backtrace,--wrap=panic_abort` is also wired up — see `lib/` for custom panic handling.

**Pre-build scripts** (in `extra_scripts`):
* `scripts/build_html.py` — generates `*.generated.h` from `data/html/`
* `scripts/gen_i18n.py` — generates I18n tables from `lib/I18n/translations/*.yaml`
* `scripts/patch_jpegdec.py` — patches the JPEG decoder library
* `scripts/git_branch.py` — bakes branch info into the build

### `SINGLE_BUFFER_MODE` Implications
- Only ONE framebuffer exists (not double-buffered).
- Grayscale rendering requires temporary buffer allocation via `renderer.storeBwBuffer()` and must be freed with `renderer.restoreBwBuffer()`.
- See [`lib/GfxRenderer/GfxRenderer.cpp:439-440`](../lib/GfxRenderer/GfxRenderer.cpp) for canonical malloc usage in the renderer.

### Directory Structure
```
aalu/
├── src/
│   ├── main.cpp                       # Entry point + global font init
│   ├── CrossPointSettings.h           # User settings (kept original name)
│   ├── MappedInputManager.cpp/.h      # Logical button → hardware mapping
│   ├── fontIds.h                      # Font ID constants
│   ├── activities/                    # UI screens (Activity lifecycle)
│   │   ├── home/                      # HomeActivity, FileBrowser, RecentBooks
│   │   ├── reader/                    # EpubReader, TxtReader, XtcReader,
│   │   │                              # Dictionary*, KOReaderSync, etc.
│   │   ├── settings/                  # Settings UI
│   │   ├── stats/                     # Reading Statistics v2.5
│   │   ├── network/                   # Wifi, OTA, OPDS
│   │   ├── browser/                   # File browser flows
│   │   ├── boot_sleep/                # Boot + sleep screens
│   │   └── util/                      # Keyboard entry, etc.
│   └── network/html/                  # HTML page sources + generated headers
├── lib/                               # Internal libraries
│   ├── hal/                           # HAL: HalDisplay, HalGPIO, HalStorage
│   ├── Epub/                          # EPUB parsing + section caching
│   ├── GfxRenderer/                   # E-ink rendering primitives
│   ├── EpdFont/                       # Font conversion + builtin fonts
│   ├── UITheme/                       # Themed UI (the `GUI` macro)
│   └── I18n/                          # i18n: translations/*.yaml + generated
├── open-x4-sdk/                       # Low-level SDK (git submodule)
├── docs/                              # File formats and other internals
├── scripts/                           # Build + dev tooling (Python)
├── English-Dictionary/                # Default StarDict files
├── partitions.csv                     # Flash partition layout
└── platformio.ini                     # Build config
```

### Hardware Abstraction Layer (HAL)

**CRITICAL**: Always use HAL classes, NOT SDK classes directly.

| HAL Class | Wraps SDK Class | Purpose | Singleton Macro |
|-----------|-----------------|---------|-----------------|
| `HalDisplay` | `EInkDisplay` | E-ink display control | *(none)* |
| `HalGPIO` | `InputManager` | Button input handling | *(none)* |
| `HalStorage` | `SDCardManager` | SD card file I/O | `Storage` |

Why HAL? Consistent error logging per module, abstracts SDK details, centralises resource management.

```cpp
#include <HalStorage.h>

FsFile file;  // SdFat FsFile, NOT Arduino File
if (Storage.openFileForRead("MODULE", "/path/to/file.bin", file)) {
  // Read from file
  file.close();  // Explicit close required
}
```

---

## AALU-Specific Features

These are AALU's additions on top of the inherited CrossPoint engine. When working in these areas, follow the existing patterns rather than introducing new abstractions.

* **Custom UI Themes & Layouts** (`src/activities/home/`, `lib/UITheme/`):
  * Multiple home themes: Classic, Lyra, Recent6 Grid (3×2, memory-safe).
  * Asymmetrical bottom menu and cascading cover-resolution fallbacks to prevent E-ink ghosting.
* **Apps Submenu** (`src/activities/AppsActivity.*`): Centralised hub for utility apps (File Transfer, Stats, OPDS).
* **KOReader Sync** (`src/activities/reader/KOReaderSyncActivity.*`):
  * Asymmetrical heuristic paragraph-level sync — fixes chapter drift, prevents remote-device XML parser crashes.
* **Reading Statistics v2.5** (`src/activities/stats/`):
  * Global dashboard (all-time hours, total finished books).
  * Contextual filtering: Reading vs Finished (toggle with `Right` button).
  * Per-book analytics: Last Session, total time, Avg pages/min.
  * 3-line title support with intelligent wrapping + strict list-view truncation.
  * **Binary migration**: v4/v5 → v6 for `stats.bin`. **If you change `stats.bin` layout, bump version + add migration code**.
* **System Stability**:
  * Deep-sleep protection: forced session save during power-off.
  * Session guarding: 3-minute threshold prevents short wake-cycle stat corruption.
* **Offline English Dictionary** (`src/activities/reader/Dictionary*Activity.*`, `English-Dictionary/`):
  * Pixel-perfect word selection from EPUB text.
  * StarDict format (`dictionary.dict` + `dictionary.idx` on SD).
  * Levenshtein-based "Did you mean?" suggestions.
  * Memory-safe Lookup History with deletion state-machine.
* **In-Reader Quick Settings (Aa) Overlay**:
  * Adjusts fonts/sizes/margins/layouts directly over book text — no full-screen menu reload.
  * Zero-heap formatting, deferred SD writes (flash-life protection), custom display buffering (no E-ink ghosting).

---

## Coding Standards

### Naming Conventions
* Classes: PascalCase (`EpubReaderActivity`)
* Methods/Variables: camelCase (`renderPage()`)
* Constants: UPPER_SNAKE_CASE (`MAX_BUFFER_SIZE`)
* Private Members: `memberVariable` (no prefix)
* File Names: match class names (`EpubReaderActivity.cpp`)
* Header guards: `#pragma once` for all headers.

### Memory Safety and RAII
* Prefer `std::unique_ptr`. Avoid `std::shared_ptr` (atomic overhead is wasted on a single-core RISC-V).
* Use destructors for cleanup, but call `file.close()` / `vTaskDelete()` explicitly for deterministic resource release.

### ESP32-C3 Platform Pitfalls

#### `std::string_view` and Null Termination
`string_view` is *not* null-terminated. Passing `.data()` to any C-style API (`drawText`, `snprintf`, `strcmp`, SdFat file paths) is UB when the view is a substring or a view of a non-null-terminated buffer.

```cpp
// WRONG — UB if view is a substring:
renderer.drawText(font, x, y, myView.data(), true);

// CORRECT:
renderer.drawText(font, x, y, std::string(myView).c_str(), true);

// CORRECT, zero-alloc for short strings:
char buf[64];
snprintf(buf, sizeof(buf), "%.*s", (int)myView.size(), myView.data());
```

#### `IRAM_ATTR` and Flash Cache Safety
All code runs from flash via the instruction cache. During SPI flash ops (OTA write, SPIFFS commit, NVS update) the cache is briefly suspended. Any code that can execute during this window — ISRs in particular — must reside in IRAM, or it will silently crash.

```cpp
void IRAM_ATTR gpioISR() { ... }
static DRAM_ATTR uint32_t isrEventFlags = 0;  // never a flash const
```

Rules:
- All ISR handlers: `IRAM_ATTR`
- Data read by `IRAM_ATTR` code: `DRAM_ATTR` (a flash-resident `static const` will fault)
- Normal task code does NOT need `IRAM_ATTR`

#### ISR vs Task Shared State
`xSemaphoreTake()` (mutex) **cannot** be called from ISR context — it will crash.

| Direction | Correct primitive |
|---|---|
| ISR → task (data) | `xQueueSendFromISR()` + `portYIELD_FROM_ISR()` |
| ISR → task (signal) | `xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()` |
| Task → task | `xSemaphoreTake()` / mutex |
| Simple flag (single writer ISR) | `volatile bool` + `portENTER_CRITICAL_ISR()` |

#### RISC-V Alignment
ESP32-C3 faults on unaligned multi-byte loads. Never cast a `uint8_t*` buffer to a wider pointer type and dereference. Use `memcpy`:

```cpp
// WRONG — faults if buf is not 4-byte aligned:
uint32_t val = *reinterpret_cast<const uint32_t*>(buf);

// CORRECT:
uint32_t val;
memcpy(&val, buf, sizeof(val));
```

This applies to all cache deserialisation and any raw buffer-to-struct casting. `__attribute__((packed))` structs have the same hazard when accessed via member reference.

#### Template and `std::function` Bloat
Each template instantiation generates a separate binary copy. `std::function<void()>` adds ~2–4 KB per unique signature and heap-allocates its closure. Avoid both in library code and any path called from the render loop:

```cpp
// Avoid:
std::function<void()> callback;

// Prefer:
void (*callback)() = nullptr;

// Member function + context:
struct Callback { void* ctx; void (*fn)(void*); };
```

When a template is necessary, limit instantiations: use explicit template instantiation in a `.cpp` to prevent duplicates across translation units.

### Error Handling Philosophy
Source: [`src/main.cpp:132-143`](../src/main.cpp), [`lib/GfxRenderer/GfxRenderer.cpp:10`](../lib/GfxRenderer/GfxRenderer.cpp)

Pattern hierarchy:
1. **`LOG_ERR` + return false** (90% case): `LOG_ERR("MOD", "Failed: %s", reason); return false;`
2. **`LOG_ERR` + fallback**: `LOG_ERR("MOD", "Unavailable"); useDefault();`
3. **`assert(false)`**: only for fatal "impossible" states (e.g. missing framebuffer).
4. **`ESP.restart()`**: only for recovery (e.g. OTA complete).

Rules: **NO exceptions**, **NO `abort()`**, **ALWAYS log before error return**.

### Acceptable malloc/free Patterns
Despite "prefer stack allocation," `malloc` is acceptable for:
1. Large temporary buffers (> 256 bytes — won't fit on stack).
2. One-time allocations during activity initialisation.
3. Bitmap rendering buffers (variable size, used briefly).

```cpp
auto* buffer = static_cast<uint8_t*>(malloc(bufferSize));
if (!buffer) {
  LOG_ERR("MODULE", "malloc failed: %d bytes", bufferSize);
  return false;
}
processData(buffer, bufferSize);
free(buffer);
buffer = nullptr;
```

Rules: ALWAYS check for nullptr; free immediately after use; set to nullptr; document why stack allocation was rejected.

Examples in codebase:
- Cover image buffers: [`HomeActivity.cpp:166`](../src/activities/home/HomeActivity.cpp)
- Text chunk buffers: [`TxtReaderActivity.cpp:259`](../src/activities/reader/TxtReaderActivity.cpp)
- Bitmap rendering: [`GfxRenderer.cpp:439-440`](../lib/GfxRenderer/GfxRenderer.cpp)
- OTA update buffer: [`OtaUpdater.cpp:40`](../src/network/OtaUpdater.cpp)

---

## UI and Orientation Guidelines

### Orientation-Aware Logic
* **No hardcoding**: Never assume 800 or 480. Use `renderer.getScreenWidth()` / `renderer.getScreenHeight()`.
* **Viewable area**: Use `renderer.getOrientedViewableTRBL()` to stay within physical bezel margins.
* **Test in all four orientations**: Portrait, Inverted Portrait, Landscape CW, Landscape CCW. Many bugs only appear in one.

### Logical Button Mapping
Source: [`src/MappedInputManager.cpp:20-55`](../src/MappedInputManager.cpp)

Physical button positions are fixed; logical functions change with user settings and orientation.

* **Physical fixed**: `Button::Up` → `HalGPIO::BTN_UP`, `Button::Down` → `HalGPIO::BTN_DOWN`.
* **User-remappable** (front buttons): `Button::Back/Confirm/Left/Right` ↔ `SETTINGS.frontButton*`.
* **Reader-specific** (page nav, swappable): `Button::PageBack` / `Button::PageForward` via `SETTINGS.sideButtonLayout`.

**Rule**: Always use `MappedInputManager::Button::*` enums, never raw `HalGPIO::BTN_*` indices (except in `ButtonRemapActivity`).

### UITheme (the `GUI` macro)
All UI rendering must go through the `GUI` macro (UITheme). Do not hardcode fonts, colours, or positioning — this is what keeps AALU's themes (Classic, Lyra, Recent6) working consistently across orientations.

---

## Common Patterns

### Singleton Access
```cpp
#define SETTINGS CrossPointSettings::getInstance()  // User settings (name retained)
#define APP_STATE CrossPointState::getInstance()    // Runtime state (name retained)
#define GUI UITheme::getInstance()                  // Current theme
#define Storage HalStorage::getInstance()           // SD card I/O
#define I18N I18n::getInstance()                    // Internationalisation
```

### Activity Lifecycle and Memory Management
Source: [`src/main.cpp:132-143`](../src/main.cpp)

Activities are **heap-allocated** and **deleted on exit**.

```cpp
void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}
```

Memory implications:
- Any memory allocated in `onEnter()` MUST be freed in `onExit()`.
- FreeRTOS tasks MUST be `vTaskDelete()`d in `onExit()` BEFORE activity destruction.
- File handles MUST be closed in `onExit()`.

```cpp
void onEnter() { Activity::onEnter(); /* alloc: buffer, tasks */ render(); }
void loop()    { mappedInput.update(); /* handle input */ }
void onExit()  { /* vTaskDelete, free, close */ Activity::onExit(); }
```

### FreeRTOS Task Guidelines
Pattern: `xTaskCreate(&taskTrampoline, "Name", stackSize, this, 1, &handle)`

Stack sizing (BYTES, not words):
- **2048**: simple rendering (most activities)
- **4096**: network, EPUB parsing
- Monitor with `uxTaskGetStackHighWaterMark()` if crashes appear.

Always `vTaskDelete()` in `onExit()` before destruction. Use a mutex for shared state.

### Global Font Loading
Source: [`src/main.cpp:40-115`](../src/main.cpp)

All fonts are loaded as global static objects at startup:
- Bookerly, Noto Sans, OpenDyslexic — multiple sizes × 4 styles each.
- Ubuntu UI fonts — 10, 12 pt × 2 styles.

Total: ~80+ global `EpdFont` / `EpdFontFamily` objects. Stored in Flash (marked `static const` in `lib/EpdFont/builtinFonts/`); rendering data caches in DRAM on first use. `OMIT_FONTS` reduces binary size for minimal builds. Font IDs in [`src/fontIds.h`](../src/fontIds.h).

```cpp
#include "fontIds.h"
renderer.insertFont(FONT_UI_MEDIUM, ui12FontFamily);
renderer.drawText(FONT_UI_MEDIUM, x, y, "Hello", true);
```

---

## Building, Flashing, Debugging

### Prerequisites
* PlatformIO Core (`pio`) or VS Code + PlatformIO IDE
* Python 3.8+
* USB-C cable
* For the debug monitor: `python3 -m pip install pyserial colorama matplotlib`

### Build Commands

```bash
# Build (default env)
pio run

# Build + flash to device
pio run -t upload

# Build a specific environment
pio run -e gh_release          # production
pio run -e gh_release_rc       # release candidate
pio run -e slim                # minimal, no serial logging

# Clean
pio run -t clean

# Upload + monitor in one go
pio run -t upload && pio device monitor
```

VS Code: Build (✓), Upload (→), Monitor (🔌) on the PlatformIO toolbar.

### Submodules
This repo uses `open-x4-sdk` as a git submodule. After cloning:
```bash
git submodule update --init --recursive
```
Or clone with `git clone --recursive ...`.

### Monitoring and Debugging

```bash
# Enhanced colour-coded monitor (recommended)
python3 scripts/debugging_monitor.py

# macOS — pass the port explicitly:
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101

# Standard PlatformIO monitor (basic)
pio device monitor
```

**Port detection**:
- macOS: `ls /dev/cu.usbmodem*`
- Linux: `ls /dev/ttyUSB* /dev/ttyACM*` or `dmesg | grep tty`
- Windows: `mode` (cmd) — ports appear as `COMx`

### Code Quality

```bash
# Static analysis (cppcheck via PlatformIO)
pio check

# Format (clang-format) — works on all platforms
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

### Debugging Crashes
Common causes:

1. **Out of memory** (most common):
   ```cpp
   LOG_DBG("MEM", "Free heap: %d bytes", ESP.getFreeHeap());
   ```
   Verify buffers are freed in `onExit()`. Look for >10 KB allocations near the crash.

2. **Stack overflow**:
   ```cpp
   LOG_DBG("TASK", "Stack high water: %d", uxTaskGetStackHighWaterMark(taskHandle));
   ```
   Bump task stack 2048 → 4096; move large locals to heap.

3. **Use-after-free**: Activity deleted but task still running. Always `vTaskDelete()` in `onExit()` BEFORE destruction. Set pointers to `nullptr` after `free()`.

4. **Corrupt cache files**: Delete `.crosspoint/` on the SD card and let the device re-parse. Check format versions in `docs/file-formats.md`.

5. **Watchdog timeout**: Loop/task blocked > 5 s. Add `vTaskDelay(1)` in tight loops; check for blocking I/O.

Verification:
1. Capture serial output for stack traces (`debugging_monitor.py` decodes addresses).
2. Monitor heap before/after suspicious operations.
3. Use `vTaskList()` to verify task deletion.
4. Test with `LOG_LEVEL=2` (debug logging on, default env).

### Reverting to Stock Firmware
If you need to roll back to Xteink's official firmware: <https://xteink.dve.al/>.

---

## Generated Files and Build Artefacts

**NEVER manually edit these — they are regenerated**:

1. **HTML headers** (generated by `scripts/build_html.py`):
   - `src/network/html/*.generated.h`
   - Source: HTML in `data/html/` (or wherever the script reads from)
   - To modify: edit source HTML, not the generated `.h`.
2. **I18n headers** (generated by `scripts/gen_i18n.py`):
   - `lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`
   - Source: `lib/I18n/translations/*.yaml`
   - To modify: edit YAML, then `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`.
   - **Commit YAML only.** All three generated files are gitignored and regenerated each build.
3. **Build artefacts** (gitignored):
   - `.pio/`, `build/`, `*.generated.h`, `compile_commands.json`

### Modifying generated content

**HTML pages**:
1. Edit source: `data/html/<pagename>.html`
2. `pio run` (auto-runs `build_html.py`)
3. Generated header updates automatically.
4. Commit ONLY the source HTML.

**Translations (i18n)**:
1. Edit/add YAML in `lib/I18n/translations/<language>.yaml`. Required keys: `_language_name`, `_language_code`, `_order`, then `STR_*` entries. English (`english.yaml`) is the reference — missing keys fall back to English.
2. Run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` (or just `pio run` to trigger it via `extra_scripts`).
3. Commit YAML only.

In code:
```cpp
#include <I18n.h>
renderer.drawText(FONT_UI, x, y, tr(STR_LOADING), true);
```

**Fonts**:
1. Source fonts → `lib/EpdFont/fontsrc/` (gitignored)
2. Run conversion script (see `lib/EpdFont/README`)
3. Add global font object in `src/main.cpp`
4. Add font ID in `src/fontIds.h`

---

## Local Development Configuration

### `platformio.local.ini` (gitignored, personal overrides)

`platformio.ini` already includes it via `extra_configs = platformio.local.ini`. Use it for:
- Serial port (`upload_port`, `monitor_port`)
- Personal debug flags
- Local build optimisations

```ini
# platformio.local.ini (gitignored)
[env:default]
upload_port = /dev/cu.usbmodem2101    # macOS
monitor_port = /dev/cu.usbmodem2101

build_flags =
  ${base.build_flags}
  -DMY_DEBUG_FLAG=1
```

Rules:
- **NEVER commit** `platformio.local.ini`.
- **NEVER put** personal info (serial ports, secrets) in `platformio.ini`.
- Use `${base.build_flags}` to extend base flags, not replace them.

---

## Cache Management and Invalidation

### Cache Structure on SD Card
**Location**: `.crosspoint/` directory on SD card root (name retained for backward compatibility).

```
.crosspoint/
├── epub_<hash>/
│   ├── progress.bin     # Reading position
│   ├── cover.bmp        # Multiple resolutions generated
│   ├── book.bin         # Metadata (title, author, spine, ToC)
│   └── sections/
│       ├── 0.bin
│       └── ...
├── stats.bin            # Global + per-book reading statistics (AALU v2.5)
└── system/
    └── BasicCover.bmp   # Fallback for missing covers
```

Hash: `std::hash<std::string>{}(filepath)`. Moving/renaming the EPUB → new hash → lost progress.

### Cache Invalidation Triggers
The cache is auto-invalidated when:
1. **File format version changes** (see `docs/file-formats.md`) — `book.bin`, `section.bin`, `stats.bin` version bumps.
2. **Render settings change**: font family/size, line spacing, paragraph spacing, screen margin.
3. **Viewport changes**: orientation or display resolution.
4. **Book file modified**: moved, renamed, or content changed (new hash).

### Manual Cache Clear
```bash
# Nuke everything (full regen)
rm -rf /path/to/sd/.crosspoint/

# One book
rm -rf /path/to/sd/.crosspoint/epub_<hash>/

# Keep progress, drop rendered sections only
rm -rf /path/to/sd/.crosspoint/epub_<hash>/sections/
```

When to clear:
- EPUB parsing errors after changes to `lib/Epub/`
- Corrupt rendering (missing text, wrong layout)
- Testing cache generation logic
- After modifying `Section.cpp`, `BookMetadataCache.cpp`, or `CrossPointSettings` render fields.

### File Format Versioning
Source: `lib/Epub/Epub/Section.cpp`, `lib/Epub/Epub/BookMetadataCache.cpp`, plus `stats.bin` in the stats activities.

Approximate current versions (verify against the source before changing):
- `book.bin`: v5
- `section.bin`: v12
- `stats.bin`: v6 (with v4/v5 → v6 migration engine — AALU addition)

Rules:
1. ALWAYS bump the version constant BEFORE changing on-disk layout.
2. Version mismatch → cache auto-invalidated and regenerated (or migrated for stats).
3. Document the change in `docs/file-formats.md`.

```cpp
// Section.cpp — example bump
static constexpr uint8_t SECTION_FILE_VERSION = 13;  // was 12
```

---

## Git Workflow

### Repository Detection
**ALWAYS verify repo context before git operations.** AALU is typically a fork — `origin` is your fork, `upstream` is the AALU repo.

```bash
git branch --show-current
git remote -v
git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null | sed 's@^refs/remotes/origin/@@'
git status --short
```

### Operation Rules
1. Never assume branch names: `git push origin $(git branch --show-current)`.
2. Never assume remote permissions:
   - Forked: push to `origin`, PR to `upstream`.
   - Direct: may push feature branches to `upstream`.
   - When in doubt, ASK.
3. Sync before starting work:
   ```bash
   git fetch upstream
   git merge upstream/main   # or upstream/master
   ```

### Branch Naming
```
feature/<short-description>       # New features
fix/<issue-number>-<description>  # Bug fixes
refactor/<component-name>         # Refactoring
docs/<topic>                      # Documentation
```

### Commit Message Format
```
<type>: <short summary, 50 chars max>

<optional body>
```
Types: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`.

Example:
```
feat: add real-time SD download progress bar

Implements progress tracking for book downloads using
UITheme progress bar component with heap-safe updates.

Tested in all 4 orientations with 5MB+ files.
```

### When to Commit
DO commit when:
- User explicitly requests it.
- Feature is complete + tested on hardware.
- Bug fix is verified.
- Refactor preserves all behaviour.
- `pio run` succeeds (no errors/warnings).

DO NOT commit when:
- Untested on hardware.
- Build fails or warns.
- Mid-experiment.
- User has not asked.
- `.gitignore`-excluded files would be staged. ALWAYS run `git status` first and cross-check (e.g. `*.generated.h`, `.pio/`, `compile_commands.json`, `platformio.local.ini`).

**If uncertain, ASK before committing.**

---

## Testing and Verification Workflow

### MANDATORY after every code change
Before declaring a task complete, proposing a commit, or handing off to the user, run:

```bash
# 1. Build (default env)
pio run

# 2. Host tests — these compile + run on macOS/Linux, no device required
bash test/run_differential_rounding_test.sh
bash test/run_hyphenation_eval.sh
```

If `pio run` warns or errors, fix it — do not paper over warnings. If a host test fails, treat it as blocking. If your change touches a path covered by neither test (most UI work), say so explicitly when reporting status.

### AI-agent scope (you can verify)
1. ✅ **Build**: `pio run -t clean && pio run` (0 errors, 0 warnings) — required after every change
2. ✅ **Host tests**: `bash test/run_differential_rounding_test.sh` and `bash test/run_hyphenation_eval.sh` — required after every change
3. ✅ **Quality**: `pio check` + `find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i`
4. ✅ **Format**: commit messages (`feat:` / `fix:`); no gitignored files staged
5. ✅ **CI**: fix GitHub Actions failures before review
6. ✅ **Code review**: orientation-aware logic correct in all four modes (inspect switch/case coverage)

### Human-tester scope (flag for the user)
6. 🔲 **Device**: test on actual hardware
7. 🔲 **Orientations**: verify all four (Portrait, Inverted, Landscape CW, CCW)
8. 🔲 **Heap**: `ESP.getFreeHeap()` > 50 KB; no leaks across activity transitions
9. 🔲 **Cache**: if EPUB code changed, delete `.crosspoint/` and verify clean re-parse
10. 🔲 **AALU-specific flows**: Quick Settings (Aa) overlay, Dictionary lookup, KOReader sync, Stats v2.5 dashboard

### CI/CD
GitHub Actions (`.github/workflows/`):

| Workflow | File | Purpose |
|---|---|---|
| Build Check | `ci.yml` | Compile verification |
| Format Check | `pr-formatting-check.yml` | clang-format compliance |
| Release | `release.yml` | Production releases |
| RC | `release_candidate.yml` | Release candidates |

Fix CI failures BEFORE requesting review. Format failure → run `clang-format` locally.

---

## Serial Monitoring and Live Debugging

```bash
# Enhanced (recommended) — colour-coded, decoded backtraces
python3 scripts/debugging_monitor.py [/dev/cu.usbmodemXXXX]

# Standard
pio device monitor
```

Live debugging snippets:
```cpp
LOG_DBG("MEM", "Free: %d", ESP.getFreeHeap());                       // every 5s
LOG_DBG("TASK", "Stack hi-water: %d", uxTaskGetStackHighWaterMark(nullptr));
logSerial.flush();   // force output before suspected crash
```

`< 512 bytes` stack high-water → bump task stack.

---

Philosophy: **We are building a dedicated e-reader, not a Swiss Army knife.** If a feature adds RAM pressure without significantly improving the reading experience, it is Out of Scope. AALU's added features (Dictionary, KOReader sync, Stats v2.5, Quick Settings overlay) all earn their RAM cost — anything new should clear the same bar.
