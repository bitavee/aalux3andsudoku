# AALU

> An open-source e-reader firmware for the **Xteink X4**, focused on organizing the books you actually read.

AALU is a custom firmware for the Xteink X4 e-paper reader, built for people who want a *personal* library experience — not just a viewer that opens EPUBs. Series get grouped automatically, finished books move out of the way, reading stats track what's worth tracking, and a long-press is all it takes to clean up the home screen. It runs on a 380KB-RAM ESP32-C3 and lives entirely on your device — no accounts, no telemetry, no cloud.

---

## Screenshots

| Home | Series viewer |
| :---: | :---: |
| ![Home screen — current read with progress ring, recent-reads grid with series-count badges, and the bottom menu](./docs/images/screenshots/home.jpeg) | ![Series viewer — all books in a series shown in reading order](./docs/images/screenshots/series-viewer.jpeg) |
| Hero book with a circular progress ring, "Recent Reads" grid with series-count badges, and the bottom menu (Files / Statistics / Transfer / Settings). | Drill-in series viewer reached from any stack tile — every member of the series shown in reading order. |

---

## Why this fork

AALU is forked from [**Seek Reader**](https://github.com/seek-reader/seek-reader), which itself builds on the excellent [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) project — the EPUB engine, SD-caching layer, WiFi book upload, and OTA updates all come from that lineage.

I started this fork because I wanted my reader to help me *organize a reading life*, not just render pages:

- **Series, not files.** If a book belongs to a series, I want to see the series as a single tile on home and drill in to pick the next entry — not scroll through eight individual covers.
- **Reading vs. finished, separated.** A book at 100% shouldn't crowd the "what am I currently reading" view, and the stats screen shouldn't pretend a book I tapped once but never read is part of my reading history.
- **A clean home.** When I'm done with a book, I want it gone from the recents grid with a long-press, not stuck there forever.
- **Honest stats.** Hours, sessions, and pages-per-minute that survive deep-sleep cycles and don't get corrupted by a 5-second peek.

If any of that sounds like how you want your reader to behave, AALU is for you.

> *This project is **not affiliated with Xteink**. It's a community / personal project, built independently.*

---

## What's new in AALU

Everything below is on top of what Seek Reader / CrossPoint already do.

### Organizing your library
- **Automatic series grouping.** Books that share `calibre:series` or EPUB 3 collection metadata are bundled into a single home tile with a count badge. Folder-name fallback handles non-tagged collections.
- **Series viewer.** Open a stack tile to see every member of the series in reading order, with the most-recently-read book pre-focused for "continue reading" in one tap.
- **Remove from recents.** Long-press Confirm (≥1 second) on a recents tile to clear it from home without deleting the file or losing your progress cache.

### Reading statistics, redone
- **All-time dashboard** — total reading hours and finished-books count.
- **Reading vs. Finished** views, toggled with the Right button.
- **Per-book analytics** — last session duration, total reading time, average pages-per-minute.
- **Stale-book filter** — books at 0% with 0 reading time stay hidden until you actually read them.
- **Deep-sleep protection** — sessions are saved on power-off, not lost.
- **Session guarding** — 3-minute minimum prevents short peeks from polluting the stats.
- **Self-healing progress** — finished books correctly read 100% (not 99%) across home, status bar, and stats.
- Binary migration engine for `stats.bin` (v4/v5 → v6) on firmware upgrade.

### Reader experience
- **In-reader Quick Settings overlay (Aa)** — fonts, sizes, margins, line spacing, layout — all adjusted *over* the book text. No full-screen settings round-trip, no flash hammering (writes are deferred), no E-ink ghosting.
- **Offline English dictionary** — pixel-perfect word selection from EPUB text, StarDict format, Levenshtein-based "did you mean?" suggestions, memory-safe lookup history. *(Drop `dictionary.dict` and `dictionary.idx` onto the SD card — sample files in [`English-Dictionary/`](./English-Dictionary).)*
- **KOReader sync** — heuristic paragraph-level synchronization that fixes chapter drift and avoids crashing remote-device XML parsers.

### UI
- **Multiple home themes** — Classic, Lyra, Recent6 Grid (3×2, memory-safe).
- **Custom boot/sleep screens** — including a cat boot logo because why not.
- **Configurable button layout** — front button mapping plus page-nav swap.
- **Four orientations** — portrait, inverted portrait, landscape CW/CCW.

### Stability
- **Heap-aware activity transitions** — the home screen's 48KB framebuffer cache is dropped before launching any sub-activity, so heap-hungry features (File Transfer's WiFi + WebServer + WebSockets) get the room they need.
- **Cascading cover fallbacks** — when a cover thumb isn't on disk at the resolution stats wants, we render from the home page's pregenerated thumb so the page never shows a blank cover.

---

## Hardware

- **Device:** Xteink X4
- **MCU:** ESP32-C3 (single-core RISC-V, 160 MHz)
- **RAM:** ~380KB usable, no PSRAM (this is the primary design constraint — every feature pays an explicit memory budget)
- **Storage:** 16MB flash + microSD card (for books and aggressive caching)
- **Display:** 800×480 monochrome E-ink, single 48KB framebuffer

The official Xteink firmware can always be restored via their web flasher: <https://xteink.dve.al/>.

---

## Installing

AALU is built with PlatformIO. The only way to get it on a device today is to compile and flash it yourself over USB-C.

### Prerequisites

- **PlatformIO Core** (`pio`) — install with `pip install platformio` or use the VS Code PlatformIO IDE extension
- **Python 3.8+**
- **USB-C cable**
- **Xteink X4 device**

### Clone

```bash
git clone --recursive https://github.com/dawsonfi/aalu.git

# If you already cloned without --recursive:
git submodule update --init --recursive
```

### Flash

Connect the X4 over USB-C, then:

```bash
pio run --target upload
```

The default build is the development environment with serial logging on. For a slimmer release build:

```bash
pio run -e gh_release --target upload
```

### Monitor (optional, for debugging)

```bash
python3 -m pip install pyserial colorama matplotlib

# macOS — explicit port:
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101

# Linux / Windows (Git Bash) — auto-detects:
python3 scripts/debugging_monitor.py
```

---

## Using it

See [USER_GUIDE.md](./USER_GUIDE.md) for the day-to-day operation reference. The short version:

- **Confirm** on a tile to open a book, a series viewer, or a menu item.
- **Long-press Confirm** on a recents tile to remove it from home.
- **Back** to go up a level; **long-press Back** in the reader to jump home.
- **Right** in stats to toggle between Reading and Finished views.
- **Aa** while reading to open the Quick Settings overlay.

---

## Internals — the 380KB constraint

The ESP32-C3 has **~380KB of usable RAM**, of which the E-ink framebuffer alone consumes 48KB. AALU is aggressive about caching to the SD card so the working set stays small.

### Cache layout (on the SD card)

```
.crosspoint/                      # name retained for backward-compat with existing caches
├── epub_<hash>/
│   ├── progress.bin              # spine index + page within chapter
│   ├── book.bin                  # metadata: title, author, spine, ToC, series
│   ├── thumb_<height>.bmp        # cover at one or more pre-rendered resolutions
│   └── sections/
│       ├── 0.bin                 # per-chapter render cache (page LUT, layout, images)
│       └── ...
├── stats.bin                     # global + per-book reading statistics
├── home_progress.json            # fast-path home progress cache
└── recent.json                   # the recents list (the home grid)
```

Cache is keyed by file path. Moving or renaming a book gives it a new hash and a fresh cache; the old cache becomes an orphan you can ignore or sweep.

Deleting `.crosspoint/` clears everything — every book gets re-parsed on next open, every cover regenerated. Use sparingly; chapter cache rebuilds are the slow path.

### When does the cache invalidate?

- **Cache format version changes** — `book.bin`, `section.bin`, `stats.bin` all have version constants that trigger rebuild on mismatch.
- **Render settings change** — font, size, margins, line spacing, paragraph spacing, screen margin.
- **Viewport changes** — orientation or display resolution.
- **Book file moved or renamed** — different path → different hash → new cache.

For the gory format details, see [`docs/file-formats.md`](./docs/file-formats.md).

---

## Roadmap / status

AALU is **actively developed** — I'm using it as my daily reader and shaping it as I go. Recent work has landed:

- ✅ Long-press to remove from recents
- ✅ Series stacks on home + drill-in series viewer
- ✅ Statistics overhaul (Reading / Finished views, stale-book filter)
- ✅ 100%-not-99% progress fix across home / stats / reader
- ✅ Heap fix for File Transfer launched from home
- ✅ Cover rendering consistency across home, groups, and stats

On the radar:

- 📋 Remove individual books from inside the series viewer
- 📋 Mtime-based EPUB cache invalidation (so editing an EPUB in place refreshes its metadata without manual cache clear)
- 📋 More UI themes
- 📋 Per-book notes / highlights

If any of those would matter to you, open an issue or PR.

---

## Contributing

Contributions are very welcome. The constraints to keep in mind:

1. **The 380KB RAM ceiling is non-negotiable.** Justify any new heap allocation or explain why a stack/static alternative was rejected.
2. **Use the HAL** (`HalDisplay`, `HalGPIO`, `HalStorage`) — don't reach into the SDK directly.
3. **i18n everything user-facing.** All UI strings go through `tr(STR_*)` and live in `lib/I18n/translations/*.yaml`.
4. **Test all four orientations.** Many bugs hide in just one.
5. **No emojis in code, no comments that just describe what the code does.** Comment only the *why* when it's non-obvious.

### Workflow

1. Fork
2. Branch: `feature/your-cool-idea`
3. Make changes, follow the project's coding guidelines in `CLAUDE.md` (or `GEMINI.md` for the equivalent Gemini-flavored doc)
4. `pio run` must pass cleanly; run the host tests under `test/`
5. Open a PR

---

## Credits

- [**Seek Reader**](https://github.com/seek-reader/seek-reader) — the direct upstream this fork builds on.
- [**CrossPoint**](https://github.com/crosspoint-reader/crosspoint-reader) — the original EPUB engine and SD-caching architecture that all of this stands on.
- [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader) — inspiration for the original architecture.
- Everyone who has flashed this firmware, filed an issue, or shared a fix.

## License

See [LICENSE](./LICENSE).
