# AALU Emulator

A browser-based, Docker-packaged emulator for AALU.

**Honest status: milestone 1.** The emulator now runs a real native C++ server (no Python stub) that owns a 1bpp framebuffer matching the device layout and renders an interactive file browser over a hand-rolled WebSocket. It does **not** yet compile and run the actual `src/` + `lib/` activity loop — that is milestone 2.

## What works today

- Web UI at <http://localhost:8080>, served from the container.
- 800×480 canvas decoding a 1bpp framebuffer over WebSocket (exact same layout as the device).
- All AALU logical buttons (Up/Down/Left/Right/Back/Confirm/PageBack/PageForward) plus keyboard shortcuts.
- SD card directory mounted at `/sdcard` inside the container, mapped to `emulator/sdcard/` on the host.
- Native C++ server (`emulator/native/main.cpp`) rendering:
  - Title bar with the emulator banner.
  - Scrollable file list of `/sdcard` contents (directories first, files second, dotfiles and `.crosspoint/` hidden).
  - Selection highlight, scroll indicator, current working directory label.
  - Live log panel showing the last few server events.
  - Footer hint bar describing the active button mapping.
- Buttons drive the UI: Up/Down move the selection; PageBack/PageForward scroll a full page; Confirm/Right enters a directory; Back/Left navigates up; Rescan re-enumerates the SD card.

## What does NOT work yet

- The actual AALU firmware. The HAL is too entangled with Arduino + FreeRTOS + ESP-IDF + `open-x4-sdk/` to compile cleanly on the host in one shot. Milestone 2 is:
  1. Implement `HalDisplayHost`, `HalGPIOHost`, `HalStorageHost` as host-targeted backends.
  2. Compile `src/` + `lib/` natively with CMake under an `EMULATOR=1` flag — no Arduino, no FreeRTOS, no ESP-IDF.
  3. Replace the milestone-1 file browser with the real activity loop running against those HAL shims.
- `lib/EpdFont` integration. Milestone 1 uses an embedded 8×16 ASCII font, not the real Bookerly/Noto/Ubuntu families.
- Wi-Fi, OTA, battery, deep-sleep — out of scope for the emulator entirely.

## Run it

From the project root:

```bash
make emulator           # foreground (Ctrl-C to stop)
make emulator-detached  # background; logs via `make emulator-logs`
make emulator-stop      # stop container
make emulator-rebuild   # force clean rebuild (after C++ changes)
make emulator-clean     # nuke containers, volumes, build cache
```

Then open <http://localhost:8080>. The `emulator/sdcard/` directory is created automatically on first run.

## Putting books on it

Drop `.epub` files into `emulator/sdcard/`. That folder is bind-mounted into the container at `/sdcard` and is gitignored — you cannot accidentally commit your library. Subdirectories work; the browser navigates into them.

When the real activity loop lands in milestone 2, this is exactly where it will read from. The `.crosspoint/` cache, `stats.bin`, and per-book `progress.bin` will live here too — same layout as a real device. You can copy a real SD card's contents in and the emulator will pick it up (caveat below on cache compatibility).

## Protocol

WebSocket at `/ws`. The server speaks RFC 6455 directly (no library) and accepts both browser handshakes and plain `websockets`-library Python clients.

**Server → Client**:

| Form     | Meaning |
|----------|---------|
| JSON     | `{"type":"config","width":800,"height":480}` once on connect |
| JSON     | `{"type":"log","message":"..."}` debug log lines |
| Binary   | 48,000 bytes — packed 1bpp framebuffer, MSB = leftmost pixel, 0 = black, 1 = white |

**Client → Server**:

| Form | Meaning |
|------|---------|
| JSON | `{"type":"button","button":"UP\|DOWN\|LEFT\|RIGHT\|BACK\|CONFIRM\|PAGE_BACK\|PAGE_FORWARD","action":"press\|release"}` |
| JSON | `{"type":"rescan"}` — re-enumerate SD card contents |

Button names map 1:1 to `MappedInputManager::Button` values in `src/MappedInputManager.h`.

## Fidelity caveats — read this before you trust it

| Aspect | Device | Emulator |
|---|---|---|
| E-ink ghosting / refresh latency | Real, 1–2 s for full refresh | None — canvas repaints instantly |
| RAM ceiling | 380 KB hard | Host has GB; leaks will pass silently |
| CPU clock | 160 MHz RISC-V | GHz x86/ARM — timing is wrong |
| Wi-Fi / OTA / OPDS / battery | Real | Absent |
| FreeRTOS scheduling | Real | Host threads, different semantics |
| Cache binary layout | Tied to font/size/margin/orientation | Lock emulator to 800×480 + same render settings for cache interchange |
| Font rendering | `lib/EpdFont` (Bookerly etc.) | Embedded 8×16 ASCII (milestone 1) |

This is a **functional emulator for UI protocol, layout, and file-browser work** today. The four-orientation hardware checklist in `CLAUDE.md` is still required for visual changes.

## Layout

```
emulator/
├── README.md               this file
├── Dockerfile              multi-stage: build C++ binary, ship slim runtime
├── docker-compose.yml      mounts sdcard/ and web/ into the container
├── .gitignore              ignores sdcard/, caches
├── sdcard/                 your EPUBs (gitignored, created by Makefile)
├── native/
│   ├── CMakeLists.txt      links OpenSSL crypto + pthreads
│   └── main.cpp            the entire native server (single TU)
└── web/
    ├── index.html
    ├── style.css
    └── app.js
```

## Next concrete step (milestone 2)

`native/hal_shims/` containing host backends for `HalDisplay`, `HalGPIO`, `HalStorage`, plus a CMake target that compiles `src/` + `lib/` against them. At that point the milestone-1 file browser in `main.cpp` is replaced by the real `currentActivity` lifecycle and the emulator becomes useful for iterating on actual reader code.
