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

# Sentinel changes per patch revision so an old patched copy doesn't satisfy
# the idempotency check and silently miss the new fix. If we ever ship a
# v3 of this patch, bump the sentinel string again.
HAL_GPIO_SENTINEL = "long-press v2: preserve heldTime across the release tick"

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
        pass  # already on latest patch, no-op
    else:
        # If an older patch revision is present, the upstream-original
        # strings won't match and _apply will hard-fail with a clear msg.
        body = _apply("HalGPIO::update", _HAL_GPIO_UPDATE_ORIGINAL, _HAL_GPIO_UPDATE_REPLACEMENT, body)
        body = _apply("HalGPIO::getHeldTime", _HAL_GPIO_HELDTIME_ORIGINAL, _HAL_GPIO_HELDTIME_REPLACEMENT, body)
        with open(HAL_GPIO_PATH, "w", encoding="utf-8") as fh:
            fh.write(body)
