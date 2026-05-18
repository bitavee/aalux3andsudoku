# Hardcover Sync — Implementation Plan

Status: **Draft / Pending implementation**
Owner: TBD
Target firmware: 1.2.0 (post-1.1.0)
Last updated: 2026-05-15

---

## 1. Goal

Add **one-way (push) reading-progress sync** from AALU to [Hardcover](https://hardcover.app) — a modern, API-friendly alternative to the now-defunct Goodreads. Coexists with the existing KOReader sync; users can enable either, both, or neither.

Hardcover gets:
- `user_book.status_id` flips: `2 = Currently Reading` on first sync, `3 = Read` when the book hits 100%.
- A `user_book_read` entry per book that AALU owns and updates over the lifetime of that read.

AALU does **not** pull progress back from Hardcover. Hardcover stores per-page integers tied to a printed edition; AALU stores `(spineIndex, page)` tied to live render settings. The two cannot be reliably reconciled, so AALU is the source of truth and Hardcover is the log.

## 2. Non-goals

- Two-way sync / pulling progress from Hardcover.
- Ratings, reviews, shelves, lists, tags.
- Multi-user / multi-account per device.
- OAuth flow (Hardcover's long-lived JWT model sidesteps this).
- Auto-sync at high frequency (rate limit + flash-wear).
- On-device JWT entry via the on-screen keyboard (token is 600+ chars).

## 3. Design summary (resolved during grill)

| Question | Resolution |
|---|---|
| Per-book Hardcover state | New file `.crosspoint/epub_<hash>/hardcover.bin` |
| Multi-device read entries | AALU always creates and updates *its own* `user_book_read` — never touches reads from other devices |
| 100% detection | Auto-set `status_id = 3` and `finished_at = now()`; clear stashed `read_id` so re-reads start fresh |
| Book matching | ISBN from OPF first; fallback to title+author search with on-device confirmation screen; zero-match dismiss |
| Token entry | Web-UI only, server validates via `me` query, stores token + Hardcover username on success |
| On-device Settings activity | Read-only status (`Connected as @username` / `Not configured`) + "Clear token" + IP-address instructions |
| HTTPS | `WiFiClientSecure::setInsecure()` — matches KOReader sync and OTA |
| Menu placement | Flat: new `Hardcover Sync` entry in the reader's menu alongside `Sync` (KOReader); new `Hardcover Sync` entry in system Settings alongside `KOReader Sync` |
| Progress metric | Percentage-based; AALU computes `globalPageIndex / totalPages`, multiplies by `edition.pages` on push |
| Sync trigger | Manual only (no per-page or timed auto-sync) |

## 4. Hardcover API reference (what we actually use)

- **Endpoint**: `https://api.hardcover.app/v1/graphql`
- **Auth header**: `Authorization: Bearer <JWT>`
- **Rate limit**: ~60 requests/minute per token (well within manual sync usage).
- **GraphQL operations needed** (4 total):

```graphql
# 1. Token validation + username display
query Me { me { id username } }

# 2. Edition lookup by ISBN (preferred path)
query EditionByIsbn($isbn: String!) {
  editions(where: { _or: [
    { isbn_13: { _eq: $isbn } }, { isbn_10: { _eq: $isbn } }
  ]}, limit: 1) { id book_id pages }
}

# 3. Title+author fallback
query BookSearch($q: String!) {
  search(query: $q, query_type: "books", per_page: 1) {
    results  # contains top hit's id, title, author, default_physical_edition.id, pages
  }
}

# 4. Upsert user_book + user_book_read in one round-trip per sync
mutation UpsertProgress(
  $book_id: Int!, $edition_id: Int!,
  $status_id: Int!, $progress_pages: Int!,
  $finished_at: date
) {
  insert_user_book_one(
    object: { book_id: $book_id, status_id: $status_id, edition_id: $edition_id }
    on_conflict: { constraint: user_books_user_id_book_id_key,
                   update_columns: [status_id] }
  ) { id }
  # Then a second mutation in the same HTTP call inserts/updates
  # user_book_read using the user_book id from above; exact shape
  # depends on whether we have a cached read_id (see §6.3).
}
```

> The exact GraphQL strings live as `static constexpr char[]` in `lib/Hardcover/HardcoverQueries.h` so they sit in flash, not DRAM. The variables go in an ArduinoJson `JsonDocument` built fresh per call. Total query text per file is <2 KB.

## 5. User-facing setup flow (instructions to ship in `docs/` and the web UI)

Document this in:
- `docs/hardcover-sync-plan.md` (this file — for engineering reference)
- The Hardcover settings panel on the web UI itself (visible at paste time)
- An "About Hardcover sync" entry in the on-device Settings → Hardcover Sync activity ("Visit http://<device-ip>/settings to enter your token")

User instructions:

1. Visit https://hardcover.app/account/api in a desktop browser, logged into the Hardcover account you want to sync with.
2. Copy the API token shown on that page (a long string starting with `eyJ...`).
3. On AALU, open **Settings → Hardcover Sync**. The screen shows the device's IP address (e.g. `192.168.1.42`).
4. From the same WiFi network on a laptop, open `http://192.168.1.42/settings` in a browser.
5. Find the **Hardcover Sync** section, paste the token, click **Save**.
6. AALU validates the token by calling Hardcover's `me` query. On success, the page updates to show "Connected as @your-hardcover-username".
7. Back on AALU, the Settings → Hardcover Sync screen now shows your username. You're done.

After setup, open any EPUB and use the reader menu → **Sync to Hardcover**.

## 6. File-by-file plan

### 6.1 New library: `lib/Hardcover/`

| File | Purpose |
|---|---|
| `HardcoverCredentialStore.h/cpp` | Singleton holding (`token`, `hardcoverUserId`, `hardcoverUsername`). XOR-obfuscated against MAC, base64-encoded to JSON on SD — same scheme as `KOReaderCredentialStore`. |
| `HardcoverClient.h/cpp` | One thin GraphQL POST helper (`gqlQuery(query, variables, outDoc)`) plus four high-level ops: `validateAndFetchUser`, `lookupEditionByIsbn`, `searchBookByTitle`, `pushProgress`. Error enum mirrors `KOReaderSyncClient::Error`. |
| `HardcoverQueries.h` | `static constexpr char[]` GraphQL query strings (flash-resident). |
| `HardcoverBookMatcher.h/cpp` | (a) extract ISBN by re-parsing OPF zip entry (no change to shared `ContentOpfParser`); (b) call `lookupEditionByIsbn` or fall back to `searchBookByTitle`; (c) return resolved `{book_id, edition_id, pages}`. |
| `HardcoverBookState.h/cpp` | Read/write `.crosspoint/epub_<hash>/hardcover.bin` (see §6.3 for format). |

### 6.2 New activities

| File | Purpose |
|---|---|
| `src/activities/reader/HardcoverSyncActivity.h/cpp` | Activity launched from the reader menu. State machine: `WIFI_SELECTION → CONNECTING → RESOLVING_BOOK → CONFIRM_TITLE_MATCH? → PUSHING → DONE / FAILED / NO_TOKEN / NO_MATCH`. Mirror `KOReaderSyncActivity` structure but no two-way result data. |
| `src/activities/settings/HardcoverSettingsActivity.h/cpp` | Read-only status screen. Shows: connected username OR "Not configured", device IP, "Clear token" button. No on-device token entry. |

### 6.3 New on-disk format: `hardcover.bin`

Per-book file at `.crosspoint/epub_<hash>/hardcover.bin`. Endianness little-endian (matches existing AALU files).

```
struct HardcoverBookState {
  uint8_t  version;          // 1
  uint8_t  reserved[3];
  uint32_t hardcoverUserId;  // tied to current token; invalidates cache if user changes
  uint32_t bookId;           // Hardcover book.id
  uint32_t editionId;        // Hardcover edition.id
  uint32_t editionPages;     // for percentage→pages conversion
  uint32_t userBookId;       // Hardcover user_books.id (our row for this book)
  uint32_t readId;           // Hardcover user_book_reads.id we own; 0 if finished/cleared
  uint32_t isbnLen;          // length of ISBN string, 0 if title-matched
  char     isbn[20];         // ISBN-13 or ISBN-10 (null-padded)
  int64_t  lastSyncedUnixMs;
  int32_t  lastSyncedPercent_x100;  // 0..10000
};
```

Total: ~64 bytes. No tighter packing needed.

Versioning: bump `version` on layout change; old files read → ignored, refetched on next sync. Document under `docs/file-formats.md` when shipped.

**Cache invalidation**: when user clears or replaces token, also wipe all `hardcover.bin` files (or compare `hardcoverUserId` lazily on read and discard mismatches). Simplest: lazy compare on read, no proactive sweep.

### 6.4 Web UI: `src/network/html/SettingsPage.html` + `CrossPointWebServer`

- Add a **Hardcover Sync** section to `SettingsPage.html`:
  - Status display: "Connected as @username" or "Not configured".
  - Token paste field (`type=password` so the JWT is masked after paste).
  - **Save** button.
  - **Clear** button.
  - Inline instructions: link to `https://hardcover.app/account/api`, explain that the token is long-lived and stored locally.
- New endpoints on `CrossPointWebServer`:
  - `POST /api/hardcover/token` — body `{ "token": "..." }`. Server calls `HardcoverClient::validateAndFetchUser`. On 200, save token + username + userId via `HardcoverCredentialStore`, return `{ "ok": true, "username": "..." }`. On failure, return `{ "ok": false, "error": "..." }` and do **not** persist.
  - `POST /api/hardcover/clear` — wipe credentials and per-book Hardcover state files.
  - `GET /api/hardcover/status` — `{ "configured": bool, "username": "..." }` for the UI to render.
- Never echo the saved token back to the browser. The UI shows status only.

### 6.5 Reader menu: `src/activities/reader/EpubReaderMenuActivity.cpp`

Add a new entry adjacent to the existing `SYNC` (KOReader) entry:

```cpp
items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});                     // existing
items.push_back({MenuAction::HARDCOVER_SYNC, StrId::STR_HARDCOVER_SYNC});          // new
```

Add `HARDCOVER_SYNC` to `EpubReaderMenuActivity::MenuAction`. Wire the dispatch in `EpubReaderActivity` to launch `HardcoverSyncActivity`. Mirror the existing KOReader `case MenuAction::SYNC` handler shape.

The entry is always visible (no gating). If the user has no token configured, the activity opens, shows the `NO_TOKEN` state with instructions, and waits for dismiss. Same UX pattern as KOReader's `NO_CREDENTIALS`.

### 6.6 System settings menu: `src/activities/settings/SettingsActivity.cpp`

Add a new entry next to the existing KOReader Sync entry:

```cpp
systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));     // existing
systemSettings.push_back(SettingInfo::Action(StrId::STR_HARDCOVER_SYNC, SettingAction::HardcoverSync));   // new
```

Add `HardcoverSync` to the `SettingAction` enum and dispatch to `HardcoverSettingsActivity`.

### 6.7 i18n

New YAML keys under `lib/I18n/translations/english.yaml` (then propagate stubs to other languages — translators will fill in):

```yaml
STR_HARDCOVER_SYNC: "Hardcover Sync"
STR_HARDCOVER_NOT_CONFIGURED: "Not configured"
STR_HARDCOVER_CONNECTED_AS: "Connected as %s"
STR_HARDCOVER_VISIT_TO_SETUP: "Visit http://%s/settings to enter your token"
STR_HARDCOVER_NO_TOKEN: "No Hardcover token. Configure in Settings."
STR_HARDCOVER_RESOLVING_BOOK: "Looking up book..."
STR_HARDCOVER_CONFIRM_MATCH: "Found: %s by %s"
STR_HARDCOVER_NO_MATCH: "Couldn't find this book on Hardcover"
STR_HARDCOVER_PUSHING: "Syncing to Hardcover..."
STR_HARDCOVER_DONE: "Synced %d%%"
STR_HARDCOVER_AUTH_FAILED: "Token rejected. Re-enter in Settings."
STR_HARDCOVER_NETWORK_ERROR: "Network error"
STR_HARDCOVER_CLEAR_TOKEN: "Clear token"
```

### 6.8 Memory & flash budget

Estimated cost on the `default` env:

| Item | Flash | DRAM (transient) |
|---|---|---|
| New `lib/Hardcover/` (5 files) | ~9 KB | — |
| New activities (sync + settings) | ~5 KB | ~2 KB during sync |
| GraphQL queries as `constexpr` | ~2 KB | 0 |
| ArduinoJson docs during sync | 0 | ~4 KB peak |
| HTTPS handshake | 0 | ~6 KB peak (already paid for KOReader) |
| New i18n strings (English) | <1 KB | 0 |
| **Total estimated** | **~17 KB flash** | **~6–8 KB peak during sync** |

Current `default` firmware: **4,646,000 bytes**, ceiling **6,553,600** → ~1.9 MB headroom. Comfortable fit. Final number measured per `wc -c .pio/build/default/firmware.bin` after each phase.

## 7. Implementation phases (tracer-bullet vertical slices)

Each phase ends with `pio run` clean (default + gh_release), both host tests passing (`test/run_differential_rounding_test.sh`, `test/run_hyphenation_eval.sh`), and a manual smoke step.

### Phase 1 — Credential store + web-UI token entry + validation

Smallest possible end-to-end slice: user can paste a token and see "Connected as @username" on the web UI.

- [ ] `lib/Hardcover/HardcoverCredentialStore.{h,cpp}` (load/save JSON, XOR-obfuscate, expose `token` / `userId` / `username` / `hasCredentials` / `clear`).
- [ ] `lib/Hardcover/HardcoverClient.{h,cpp}` skeleton + `validateAndFetchUser` only.
- [ ] `lib/Hardcover/HardcoverQueries.h` with the `Me` query.
- [ ] `CrossPointWebServer`: `POST /api/hardcover/token`, `POST /api/hardcover/clear`, `GET /api/hardcover/status`.
- [ ] `SettingsPage.html`: Hardcover section with paste field, status display, save/clear.
- [ ] Smoke: paste a real token from `hardcover.app/account/api`, see username appear; paste a bad token, see error; clear, status flips back.

### Phase 2 — On-device Settings activity + reader menu wiring

Adds the on-device surface without yet doing real sync work — clicking "Sync" launches the activity, which lands on `NO_TOKEN` or a stub `DONE` screen.

- [ ] `HardcoverSettingsActivity.{h,cpp}` showing username/IP/clear button.
- [ ] `SettingsActivity` entry + `SettingAction::HardcoverSync`.
- [ ] `EpubReaderMenuActivity` adds `MenuAction::HARDCOVER_SYNC` + `STR_HARDCOVER_SYNC`.
- [ ] `EpubReaderActivity` dispatches to a stub `HardcoverSyncActivity` that only renders `NO_TOKEN` or "stub done" then exits.
- [ ] i18n YAML stubs for all new keys.
- [ ] Smoke: open a book, find the new menu entry, see it land correctly in both configured and not-configured states; verify in all 4 orientations.

### Phase 3 — Book matching + ISBN extraction + per-book cache

- [ ] `HardcoverBookMatcher.{h,cpp}`: re-parse OPF zip entry to extract first `<dc:identifier>` of scheme ISBN (or with `isbn` prefix in the value). Use existing `ZipFile` + a one-shot expat parse — do **not** modify the shared `ContentOpfParser`.
- [ ] `HardcoverClient`: `lookupEditionByIsbn`, `searchBookByTitle`.
- [ ] `HardcoverBookState.{h,cpp}`: read/write `hardcover.bin` with lazy `hardcoverUserId` check.
- [ ] Wire into `HardcoverSyncActivity`: `RESOLVING_BOOK` state → either cached state, or ISBN lookup, or title fallback → `CONFIRM_TITLE_MATCH` state on title fallback.
- [ ] Smoke: try a book with ISBN (Bookerly novel), one without (Project Gutenberg HTML), one with totally unmatchable title.

### Phase 4 — Push progress + status transitions + finished-book handling

- [ ] `HardcoverClient::pushProgress(state, percentage, completed) -> Error`. Inserts/updates `user_book_read`; updates `user_book.status_id` when first creating or when `completed=true`.
- [ ] In sync activity: compute `percentage` from `(currentSpine, currentPage, totalPagesAcrossSpines)`; convert to `progress_pages = percentage * editionPages`.
- [ ] Detect "finished" (current position == last spine + last page). When true, send `finished_at=today` and `status_id=3`, then clear `readId` in `hardcover.bin`.
- [ ] `DONE` state shows "Synced X%" or "Marked as Read".
- [ ] Smoke: sync a book at 0%, at 50%, at 100%; re-open finished book, advance, sync → new read entry should be created.

After Phase 4, ship as 1.2.0.

## 8. Pre-mortem

Assume this shipped and broke something. Most likely failure modes:

| Risk | Mitigation |
|---|---|
| Token entered with trailing whitespace from copy-paste → silent auth failures | Trim whitespace server-side before validation; trim again before storage. |
| GraphQL response includes an `errors` array with HTTP 200 — easy to miss | `HardcoverClient::gqlQuery` checks for `data` key presence AND `errors` absence; any `errors` element → return `SERVER_ERROR`. |
| `std::string_view` into ArduinoJson response handed to `drawText` after the JSON doc goes out of scope → use-after-free | All response values copied into `std::string` immediately on extraction. |
| Hardcover JWT exceeds expected length, overflows a fixed buffer | Use `std::string` for the token throughout; never copy into a sized `char[]`. |
| ISBN regex too loose — picks up an internal identifier like a UUID | Match by `opf:scheme="ISBN"` attribute first, then by value starting with `urn:isbn:`, then by 10/13-digit numeric form. |
| Re-parsing OPF on every first-time book sync is slow on large EPUBs | Acceptable — happens once per book ever. Falls back to filename-only title if OPF read fails. |
| Mismatched edition (paperback vs hardback) causes wrong `editionPages`, so percentage→pages math is misleading | Document that AALU's "page" on Hardcover is approximate; users who care can edit the edition manually. |
| User on AALU has `setInsecure()` HTTPS and is on hostile WiFi → token leak | Document the risk in `docs/hardcover-sync-plan.md` and the web UI; same posture as KOReader/OTA already. Out of scope to fix here. |
| Web UI shows the saved token back to the browser → screenshot/leak risk | Server never returns the token. Only username + `configured: true` flag. |
| Pre-finished detection wrong: a multi-chapter book's "last page" trigger fires mid-book due to spine numbering quirks | Use the same end-of-book detection that Stats v2.5 already uses; consolidate the check into one helper if needed. |
| Token revoked by user on Hardcover web → all syncs fail silently | Sync activity surfaces `AUTH_FAILED` with `STR_HARDCOVER_AUTH_FAILED` ("Token rejected. Re-enter in Settings."). |
| Multiple concurrent syncs (user spam-presses Sync) | Activity stack model prevents this — only one activity runs at a time. |
| Flash-write storm on every page turn | We **do not** write `hardcover.bin` on page turns. Only on completed sync events. CLAUDE.md flash-life rule honored. |
| `hardcover.bin` left orphaned after user deletes a book from SD | Same as KOReader's `progress.bin` — happens, no harm; cleaned up next time user wipes `.crosspoint/`. |

## 9. Test plan

### Host-side (must pass before any PR)

```bash
pio run                                       # default env clean
pio run -e gh_release                         # release env clean
bash test/run_differential_rounding_test.sh
bash test/run_hyphenation_eval.sh
```

### On-device manual smoke (human-tester required)

1. **Setup**: paste valid token via web UI; verify username appears. Paste invalid; verify error.
2. **Reader menu**: open a book, find "Hardcover Sync" entry. In all 4 orientations.
3. **ISBN match**: sync a commercial EPUB with an ISBN. Verify in Hardcover web UI that:
   - The book moves to "Currently Reading".
   - A new `user_book_read` is created with the correct page count.
4. **Title fallback**: sync a Project Gutenberg EPUB. Verify confirmation screen appears. Cancel — no Hardcover state. Try again, confirm — sync succeeds.
5. **No-match**: sync an EPUB with garbage title. Verify dismiss screen.
6. **Progress update**: sync at 0%, read for 10 min, sync again. Verify same `user_book_read` updated (not duplicated).
7. **Finished**: read to last page, sync. Verify status → "Read", read entry closed.
8. **Re-read**: open finished book, advance a page, sync. Verify new `user_book_read` entry created.
9. **Token revoke**: revoke token on hardcover.app, sync. Verify `AUTH_FAILED` shown.
10. **Network down**: disable WiFi mid-sync. Verify graceful error, no crash, no corrupted `hardcover.bin`.
11. **Heap check**: `ESP.getFreeHeap()` before sync vs after activity exit. Verify no leak (within 1 KB).
12. **Cache file**: inspect `hardcover.bin` on SD card; verify size ~64 bytes; verify it's regenerated if deleted.
13. **Coexistence with KOReader**: configure both, sync to both in succession on the same book. Verify neither breaks.

## 10. Future enhancements (explicitly out of scope here)

- mDNS / `aalu.local` hostname so setup instructions don't need an IP.
- Auto-sync on book close (debounced).
- Webhook from Hardcover → AALU for two-way sync (would require Hardcover-side work).
- Multi-account / family device support.
- Mapping AALU's existing "finished books" history into Hardcover backfill (one-time import).
- Pinned TLS cert for hardcover.app.

## 11. References

- KOReader sync (pattern source): `lib/KOReaderSync/`, `src/activities/reader/KOReaderSyncActivity.{h,cpp}`, `src/activities/settings/KOReaderSettingsActivity.{h,cpp}`.
- Web server pattern: `src/network/CrossPointWebServer.cpp`, `src/network/html/SettingsPage.html`.
- EPUB OPF parsing: `lib/Epub/Epub/parsers/ContentOpfParser.{h,cpp}`.
- Reader menu wiring: `src/activities/reader/EpubReaderMenuActivity.{h,cpp}`, `src/activities/reader/EpubReaderActivity.cpp:524`.
- AALU constraints: `CLAUDE.md` (flash ceiling, RAM rules, ISR safety, RISC-V alignment).
