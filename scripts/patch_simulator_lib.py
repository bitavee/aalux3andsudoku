"""Patch the fetched crosspoint-simulator library for AALU's needs.

PlatformIO runs this as a pre-build extra_script in the [env:simulator]
environment. It checks each known-fragile file in the sim lib, applies a
local fix if not already present, and is fully idempotent — safe to run on
every build. The whole script is a no-op when the sim lib hasn't been
fetched yet (PIO usually fetches before pre-scripts run, but the order is
not strictly guaranteed across versions).

Patches applied:
  - HalGPIO.cpp: long-press tracking.
      * Upstream zeroes `buttonPressTime` on KEYUP, and `getHeldTime()`
        filters by `SDL_GetKeyboardState`. So the moment KEYUP fires, the
        keyboard state goes false and `getHeldTime()` returns 0.
        AALU's reader / viewer activities use the pattern
            if (wasReleased(Back)) {
              if (getHeldTime() >= LONG_MS) long_press();
              else                          short_press();
            }
        which therefore always sees `heldTime == 0` on the sim, making
        every press look short. The real device's InputManager preserves
        the held duration through the release tick, which is what AALU
        relies on.
      * Fix: preserve `buttonPressTime` for exactly one update tick after
        KEYUP (the same tick `wasReleased()` reports true), and have
        `getHeldTime()` honour `releasedThisFrame` in addition to current
        keyboard state. Also stamp `buttonPressTime` only on a real
        "not-held -> held" edge to ignore spurious double-KEYDOWNs from
        focus races, and defensively reconcile against SDL's authoritative
        keyboard state at the end of update().

Each patch carries a sentinel string so re-runs detect "already patched"
and skip.
"""

from __future__ import annotations

import os
import sys

Import("env")  # type: ignore  # PIO injects this builtin in pre-scripts.

PIOENV = env.subst("$PIOENV")
LIBDEPS_DIR = env.subst("$PROJECT_LIBDEPS_DIR")
SIM_SRC = os.path.join(LIBDEPS_DIR, PIOENV, "simulator", "src")
HAL_GPIO_PATH = os.path.join(SIM_SRC, "HalGPIO.cpp")
HAL_DISPLAY_PATH = os.path.join(SIM_SRC, "HalDisplay.h")
HAL_DISPLAY_CPP_PATH = os.path.join(SIM_SRC, "HalDisplay.cpp")
HAL_CLOCK_PATH = os.path.join(SIM_SRC, "HalClock.cpp")
FREERTOS_PATH = os.path.join(SIM_SRC, "freertos", "FreeRTOS.h")

# Sentinel changes per patch revision so an old patched copy doesn't satisfy
# the idempotency check and silently miss the new fix. If we ever ship a
# v3 of this patch, bump the sentinel string again.
HAL_GPIO_SENTINEL = "long-press v2: also report duration on the release tick"

_HAL_GPIO_UPDATE_ORIGINAL = """void HalGPIO::update() {
  // Reset per-frame state
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      if (e.key.keysym.scancode == SIMULATOR_SLEEP_SCANCODE) {
        simulatorSleepRequested = true;
        continue;
      }
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        pressedThisFrame[btn] = true;
        buttonPressTime[btn] = SDL_GetTicks();
      }
    } else if (e.type == SDL_KEYUP) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        releasedThisFrame[btn] = true;
      }
    }
  }
}"""

_HAL_GPIO_UPDATE_REPLACEMENT = """void HalGPIO::update() {
  // long-press v2: preserve heldTime across the release tick.
  // Reset per-frame state
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      if (e.key.keysym.scancode == SIMULATOR_SLEEP_SCANCODE) {
        simulatorSleepRequested = true;
        continue;
      }
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        pressedThisFrame[btn] = true;
        // Stamp the press time only on a real "not held -> held" edge.
        // A spurious second KEYDOWN (focus race on macOS) would otherwise
        // restart the hold timer mid-hold.
        if (buttonPressTime[btn] == 0) {
          buttonPressTime[btn] = SDL_GetTicks();
        }
      }
    } else if (e.type == SDL_KEYUP) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        releasedThisFrame[btn] = true;
        // Do NOT clear buttonPressTime here. getHeldTime() needs to report
        // the full press duration for this one update tick so the
        // "if (wasReleased(b)) { ... }" branch can distinguish long from
        // short press. The reconciliation below clears it on the next
        // tick, once releasedThisFrame has been reset.
      }
    }
  }

  // Defensive reconciliation against SDL's authoritative keyboard state.
  // - kbd[btn] true, buttonPressTime == 0    -> newly held; stamp now (we
  //   missed the KEYDOWN, e.g. window opened with the key already down).
  // - kbd[btn] false, releasedThisFrame      -> just released; keep
  //   buttonPressTime so getHeldTime() can report the final duration.
  // - kbd[btn] false, !releasedThisFrame,
  //   buttonPressTime != 0                   -> stale from a previous
  //   release tick (or KEYUP we lost to a focus drop); clear it so the
  //   next press starts fresh.
  const Uint8 *kbd = SDL_GetKeyboardState(NULL);
  const unsigned long now = SDL_GetTicks();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (kbd[buttonScancode[i]]) {
      if (buttonPressTime[i] == 0) {
        buttonPressTime[i] = now;
      }
    } else if (!releasedThisFrame[i] && buttonPressTime[i] != 0) {
      buttonPressTime[i] = 0;
    }
  }
}"""

_HAL_GPIO_HELDTIME_ORIGINAL = """unsigned long HalGPIO::getHeldTime() const {
  // Return the longest held time among all currently pressed buttons
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (state[buttonScancode[i]] && buttonPressTime[i] > 0) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld)
        maxHeld = held;
    }
  }
  return maxHeld;
}"""

_HAL_GPIO_HELDTIME_REPLACEMENT = """unsigned long HalGPIO::getHeldTime() const {
  // long-press v2: also report duration on the release tick so callers
  // doing  `if (wasReleased(b)) { if (getHeldTime() >= LONG) ... }`  can
  // distinguish long from short press. update() clears buttonPressTime on
  // the *next* tick, so this window is exactly one update cycle wide.
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonPressTime[i] == 0) {
      continue;
    }
    if (state[buttonScancode[i]] || releasedThisFrame[i]) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld) {
        maxHeld = held;
      }
    }
  }
  return maxHeld;
}"""


# HalDisplay.h: AALU's merged GfxRenderer calls the device differential-refresh
# and tiled-grayscale API, which the sim lib's HalDisplay predates. Inject the
# missing declarations. supportsStripGrayscale() returns true so the renderer
# takes its 4-level grayscale path; the actual compositing is implemented in
# HalDisplay.cpp (see the HalDisplay.cpp patch below) so the emulator renders
# real 4-level gray like the device instead of a black-and-white fallback.
HAL_DISPLAY_SENTINEL = "AALU: device differential-refresh / tiled-grayscale API v2 (grayscale enabled)"

_HAL_DISPLAY_ORIGINAL = """  void cleanupGrayscaleBuffers(const uint8_t *bwBuffer);"""

_HAL_DISPLAY_REPLACEMENT = """  void cleanupGrayscaleBuffers(const uint8_t *bwBuffer);

  // AALU: device differential-refresh / tiled-grayscale API v2 (grayscale enabled).
  // supportsStripGrayscale() returns true so covers take the 4-level grayscale
  // path; copyGrayscaleLsb/MsbBuffers + displayGrayBuffer (patched into
  // HalDisplay.cpp) composite the two 1-bit planes onto the BW base so the
  // emulator shows the same 4 gray levels as the device.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false) {
    displayBuffer(fallback, turnOffScreen);
  }
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void writeGrayscalePlaneStrip(bool, const uint8_t *, uint16_t, uint16_t) {}
  bool supportsStripGrayscale() const { return true; }"""


# HalDisplay.cpp: implement the grayscale composite for the simulator. The sim
# lib ships these as no-ops. GfxRenderer renders two 1-bit planes into the
# framebuffer (LSB bit set for gray value 1, MSB bit set for values 1 or 2) and
# hands each to copyGrayscale*Buffers; displayGrayBuffer merges them onto the
# already-presented BW base (value 0 -> black, value 3 -> white).
HAL_DISPLAY_CPP_SENTINEL = "AALU: composite 4-level grayscale onto the BW base"

_HAL_DISPLAY_CPP_ORIGINAL = """void HalDisplay::copyGrayscaleBuffers(const uint8_t *, const uint8_t *) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *) {}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *) {}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *) {}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {}"""

_HAL_DISPLAY_CPP_REPLACEMENT = """// AALU: composite 4-level grayscale onto the BW base. GfxRenderer renders the
// gray image as two 1-bit planes (LSB bit set for gray value 1, MSB bit set for
// values 1 or 2) and hands each framebuffer here. displayGrayBuffer merges them
// onto pixelBuf, which already holds the BW base from the prior displayBuffer
// (value 0 -> black, value 3 -> white). Same bit layout as refreshDisplay.
static uint8_t aaluGrayLsb[HalDisplay::BUFFER_SIZE];
static uint8_t aaluGrayMsb[HalDisplay::BUFFER_SIZE];
void HalDisplay::copyGrayscaleBuffers(const uint8_t *, const uint8_t *) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *lsb) {
  if (lsb) memcpy(aaluGrayLsb, lsb, BUFFER_SIZE);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *msb) {
  if (msb) memcpy(aaluGrayMsb, msb, BUFFER_SIZE);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *) {}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {
  for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
    const int byteIdx = i / 8;
    const int bitIdx = 7 - (i % 8);
    if (((aaluGrayMsb[byteIdx] >> bitIdx) & 1) == 0) continue;
    const bool lsb = ((aaluGrayLsb[byteIdx] >> bitIdx) & 1) != 0;
    pixelBuf[i] = lsb ? 0xFF555555u : 0xFFAAAAAAu;
  }
  pendingPresent.store(true);
}"""


# HalClock.cpp: the sim has no RTC and no real WiFi/NTP, so syncFromNTP() is a
# no-op that leaves the clock unavailable and it never renders. Fake a
# successful sync so ClockSyncActivity can demo the clock on the host
# (getTime() already returns the host wall clock).
HAL_CLOCK_SENTINEL = "sim: fake a successful NTP sync"

_HAL_CLOCK_ORIGINAL = """bool HalClock::syncFromNTP() { return _available; }"""

_HAL_CLOCK_REPLACEMENT = """bool HalClock::syncFromNTP() {
  // sim: fake a successful NTP sync so the clock becomes available + renders.
  _available = true;
  return true;
}"""


def _apply(label: str, original: str, replacement: str, src: str) -> str:
    if replacement in src:
        return src  # already patched (handled by caller via sentinel, but defensive)
    if original not in src:
        print(
            f"[sim-patch] {label}: upstream source does not match expected — "
            f"the sim lib likely changed. Manual review required."
        )
        sys.exit(1)
    print(f"[sim-patch] {label}: applied")
    return src.replace(original, replacement)


if not os.path.exists(HAL_GPIO_PATH):
    print(f"[sim-patch] {HAL_GPIO_PATH} missing — will retry next build")
else:
    with open(HAL_GPIO_PATH, "r", encoding="utf-8") as fh:
        body = fh.read()

    if HAL_GPIO_SENTINEL in body:
        pass  # already patched
    elif _HAL_GPIO_HELDTIME_ORIGINAL not in body:
        print("[sim-patch] HalGPIO::getHeldTime: upstream changed — long-press patch skipped (re-derive for new sim lib)")
    else:
        # The pinned sim lib's update() already keeps buttonPressTime across the
        # KEYUP frame, so only the getHeldTime fix is needed: honor
        # releasedThisFrame[i] so wasReleased()+getHeldTime() reports the hold
        # duration on release (SDL_GetKeyboardState is already false by then).
        body = _apply("HalGPIO::getHeldTime long-press", _HAL_GPIO_HELDTIME_ORIGINAL, _HAL_GPIO_HELDTIME_REPLACEMENT, body)
        with open(HAL_GPIO_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)

if not os.path.exists(HAL_DISPLAY_PATH):
    print(f"[sim-patch] {HAL_DISPLAY_PATH} missing — will retry next build")
else:
    with open(HAL_DISPLAY_PATH, "r", encoding="utf-8") as fh:
        body = fh.read()

    if HAL_DISPLAY_SENTINEL in body:
        pass  # already patched
    elif "supportsStripGrayscale" in body:
        # Newer sim lib declares the grayscale/differential-refresh API itself
        # (including native 4-level grayscale). Adding our stubs would redeclare
        # those members. Skip — the upstream API is a superset of what we need.
        print("[sim-patch] HalDisplay.h: sim lib already declares the grayscale API — patch not needed")
    else:
        body = _apply("HalDisplay grayscale stubs", _HAL_DISPLAY_ORIGINAL, _HAL_DISPLAY_REPLACEMENT, body)
        with open(HAL_DISPLAY_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)

# HalDisplay.h: the newer sim lib reads orientation via renderer.getOrientation()
# and dropped setSimulatorOrientation(int). AALU's GfxRenderer.h still calls it
# under #ifdef SIMULATOR, so add a no-op sink when the lib doesn't provide one.
if os.path.exists(HAL_DISPLAY_PATH):
    with open(HAL_DISPLAY_PATH, "r", encoding="utf-8") as fh:
        body = fh.read()
    if "setSimulatorOrientation" in body:
        pass  # provided by the lib (older sim) or already added
    else:
        body = _apply(
            "HalDisplay setSimulatorOrientation sink",
            "  void presentIfNeeded();",
            "  void presentIfNeeded();\n  void setSimulatorOrientation(int) {}",
            body,
        )
        with open(HAL_DISPLAY_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)

if not os.path.exists(HAL_DISPLAY_CPP_PATH):
    print(f"[sim-patch] {HAL_DISPLAY_CPP_PATH} missing — will retry next build")
else:
    with open(HAL_DISPLAY_CPP_PATH, "r", encoding="utf-8") as fh:
        body = fh.read()

    if HAL_DISPLAY_CPP_SENTINEL in body:
        pass  # already patched
    elif _HAL_DISPLAY_CPP_ORIGINAL not in body:
        # Newer sim lib implements 4-level grayscale natively (GrayscalePreviewState
        # + composeGrayscalePreview), so the no-op stubs we replace aren't present.
        # Skip — the emulator already composites gray like the device.
        print("[sim-patch] HalDisplay.cpp: sim lib already implements grayscale — patch not needed")
    else:
        body = _apply("HalDisplay grayscale composite", _HAL_DISPLAY_CPP_ORIGINAL, _HAL_DISPLAY_CPP_REPLACEMENT, body)
        with open(HAL_DISPLAY_CPP_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)

if not os.path.exists(HAL_CLOCK_PATH):
    print(f"[sim-patch] {HAL_CLOCK_PATH} missing — will retry next build")
else:
    with open(HAL_CLOCK_PATH, "r", encoding="utf-8") as fh:
        body = fh.read()

    if HAL_CLOCK_SENTINEL in body:
        pass  # already patched
    elif _HAL_CLOCK_ORIGINAL not in body:
        print("[sim-patch] HalClock::syncFromNTP: upstream changed — fake-sync patch skipped (clock demo may be unavailable in sim)")
    else:
        body = _apply("HalClock fake sync", _HAL_CLOCK_ORIGINAL, _HAL_CLOCK_REPLACEMENT, body)
        with open(HAL_CLOCK_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)

# freertos/FreeRTOS.h: null-guard the critical-section shims. AALU (like ESP-IDF
# on the single-core ESP32-C3, where the portMUX is unused) calls
# taskENTER_CRITICAL(nullptr) / taskEXIT_CRITICAL(nullptr). The newer sim shim
# dereferences the mux unconditionally (mux->mtx.lock()), so a null mux crashes
# the render task with a null-mutex EXC_BAD_ACCESS. Guard the deref.
_FREERTOS_ENTER_ORIGINAL = "inline void taskENTER_CRITICAL(portMUX_TYPE *mux) { mux->mtx.lock(); }"
_FREERTOS_ENTER_REPLACEMENT = "inline void taskENTER_CRITICAL(portMUX_TYPE *mux) { if (mux) mux->mtx.lock(); }"
_FREERTOS_EXIT_ORIGINAL = "inline void taskEXIT_CRITICAL(portMUX_TYPE *mux) { mux->mtx.unlock(); }"
_FREERTOS_EXIT_REPLACEMENT = "inline void taskEXIT_CRITICAL(portMUX_TYPE *mux) { if (mux) mux->mtx.unlock(); }"

if not os.path.exists(FREERTOS_PATH):
    print(f"[sim-patch] {FREERTOS_PATH} missing — will retry next build")
else:
    with open(FREERTOS_PATH, "r", encoding="utf-8") as fh:
        body = fh.read()
    if _FREERTOS_ENTER_REPLACEMENT in body:
        pass  # already patched
    elif _FREERTOS_ENTER_ORIGINAL not in body:
        print("[sim-patch] FreeRTOS.h taskENTER_CRITICAL: upstream changed — null-guard skipped (review taskENTER_CRITICAL(nullptr) callers)")
    else:
        body = body.replace(_FREERTOS_ENTER_ORIGINAL, _FREERTOS_ENTER_REPLACEMENT)
        body = body.replace(_FREERTOS_EXIT_ORIGINAL, _FREERTOS_EXIT_REPLACEMENT)
        with open(FREERTOS_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)
        print("[sim-patch] FreeRTOS.h: null-guarded taskENTER_CRITICAL / taskEXIT_CRITICAL")
