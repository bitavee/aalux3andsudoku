# AALU Backlog

> Source of truth: [`docs/UX_REDESIGN.md`](./UX_REDESIGN.md). This file is the **flat, prioritized backlog** distilled from that doc. Each row links back to the section that contains the full spec.
>
> **Prioritization within each tier**: P0 ship first, P1 ship soon, P2 nice next, P3 low-leverage. Across tiers, P0 of a higher tier doesn't necessarily beat P1 of a lower tier — pick the band, then the priority.
>
> **Effort**: **S** (afternoon) · **M** (1–2 days) · **L** (a week or more).
> **Risk**: heap (RAM) · flash (write frequency) · refresh (E-ink) · scope (feature creep / breakage potential) · legal (3rd-party ToS) · none.

---

## Top of stack — start here

The 5 highest-value-per-effort items (per UX_REDESIGN §5.1). If you do nothing else, do these.

| Rank | ID | Title | Tier | Effort |
| --- | --- | --- | --- | --- |
| 1 | `POLISH-MENU` | Restructure Reader Confirm menu into sections | Polish | M |
| 2 | `POLISH-STATUSBAR` | Status bar: collapse 6 variants → 3, add time-left | Polish | M |
| 3 | `LOCAL-TUTORIAL` | First-launch tutorial | Local QoL | M |
| 4 | `POLISH-HINTS` | State-aware button hints across screens | Polish | M |
| 5 | `T1-ENRICH` | Cover + metadata enrichment from Open Library | Tier 1 | M |

---

## Tier 0 — Polish (audit findings, no new features)

Improvements to existing screens. Mostly bounded effort, low risk. Phase 1 of the roadmap.

| ID | Title | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- |
| `POLISH-MENU` | Restructure Reader Confirm menu into 4 sections (Navigate / Reading / Screen / Book) | P0 | M | none | [§2.3.2](./UX_REDESIGN.md#232-epubreadermenuactivity--the-confirm-menu) |
| `POLISH-STATUSBAR` | Collapse status bar 6 variants → 3 (None / Minimal / Full) and add "12m left" time estimate | P0 | M | scope | [§2.3.1](./UX_REDESIGN.md#231-epubreaderactivity--the-page) |
| `POLISH-HINTS` | State-aware button hint row; surface long-press affordances | P0 | M | none | [§3.3](./UX_REDESIGN.md#33-button-hint-affordance) |
| `POLISH-HERO` | Home hero redesign: typographic hierarchy, linear bar, time-left line | P1 | M | refresh | [§2.2.1](./UX_REDESIGN.md#221-homeactivity) |
| `POLISH-SERIES-VIEWER` | Series viewer: fix footer cutoff, add author/finished count, status glyphs per book, 2-line title wrap | P1 | M | none | [§2.2.2](./UX_REDESIGN.md#222-seriesvieweractivity) |
| `POLISH-STATS-TOP` | Stats: explicit Reading/Finished tab UI, year/month summary, 30-day sparkline | P1 | M | none | [§2.6.1](./UX_REDESIGN.md#261-statsactivity-the-global--list-view) |
| `POLISH-SETTINGS-REBALANCE` | Rebalance Settings tabs: add Network tab, move misplaced rows; document tab-nav rule | P1 | M | none | [§2.8.1](./UX_REDESIGN.md#281-settingsactivity-root) |
| `POLISH-EMPTY-STATES` | Standard empty-state component: headline + next-action across all empty screens | P1 | S | none | [§3.4](./UX_REDESIGN.md#34-empty-states) |
| `POLISH-ERROR-STATES` | Standard error-state component: what failed / what it means / how to recover | P1 | S | none | [§3.5](./UX_REDESIGN.md#35-error-states) |
| `POLISH-OTA` | OTA: version transition, changelog, "do not power off" warning, progress bar | P1 | M | refresh | [§2.8.5](./UX_REDESIGN.md#285-otaupdateactivity) |
| `POLISH-CLEAR-CACHE` | Clear Cache: partial-clear with toggles (regenerable vs progress vs stats) | P1 | M | flash | [§2.8.6](./UX_REDESIGN.md#286-clearcacheactivity) |
| `POLISH-AA-LONGPRESS` | Long-press Confirm in reader → open Aa overlay directly | P1 | S | none | [§2.3.3](./UX_REDESIGN.md#233-the-aa-quick-settings-overlay) |
| `POLISH-AA-HINTS` | Aa overlay: state-aware hint row reflecting TAB_FOCUSED vs ITEM_FOCUSED | P1 | M | none | [§2.3.3](./UX_REDESIGN.md#233-the-aa-quick-settings-overlay) |
| `POLISH-HEADER-3ZONE` | Adopt 3-zone header pattern (left=back, center=title, right=status) everywhere | P1 | M | none | [§3.2](./UX_REDESIGN.md#32-status-bar-consistency) |
| `POLISH-TYPOGRAPHY` | Document type scale (Display / Heading / Subhead / Body / Caption); enforce across screens | P1 | S | none | [§3.1](./UX_REDESIGN.md#31-typography-hierarchy) |
| `POLISH-BATTERY` | Battery icon + percent on every header; optional days-remaining estimate | P1 | S | none | [§2.2.1](./UX_REDESIGN.md#221-homeactivity) |
| `POLISH-BOOT` | Boot screen: post-logo "Resuming Iron Flame · 39% · ~6h left" card | P2 | M | heap, refresh | [§2.1.1](./UX_REDESIGN.md#211-bootactivity) |
| `POLISH-SLEEP-DEPS` | Sleep settings: indent Cover Mode / Filter under parent when applicable | P2 | S | none | [§2.1.2](./UX_REDESIGN.md#212-sleepactivity) |
| `POLISH-FILEBROWSER-META` | File browser: file size + modified date for books, book count for folders | P2 | M | heap | [§2.2.3](./UX_REDESIGN.md#223-filebrowseractivity) |
| `POLISH-FILEBROWSER-EMPTY` | File browser empty state: actionable copy pointing to Transfer | P2 | S | none | [§2.2.3](./UX_REDESIGN.md#223-filebrowseractivity) |
| `POLISH-CHAPTER-SELECT` | Chapter selection: highlight current, scroll position header, long-press jump-10 | P2 | S | none | [§2.4.1](./UX_REDESIGN.md#241-epubreaderchapterselectionactivity) |
| `POLISH-PERCENT-CONTEXT` | Percent selection: show chapter title under selected %  | P2 | S | none | [§2.4.2](./UX_REDESIGN.md#242-epubreaderpercentselectionactivity) |
| `POLISH-DICT-DEFINITION` | Dictionary definition: page indicator, dynamic hint row when not-found | P2 | S | none | [§2.5.3](./UX_REDESIGN.md#253-dictionarydefinitionactivity) |
| `POLISH-DICT-WORDSELECT` | Word selection mode: anti-ghosting refresh heuristic, mode-specific hint row | P2 | M | refresh | [§2.5.1](./UX_REDESIGN.md#251-dictionarywordselectactivity) |
| `POLISH-DICT-SUGGESTIONS` | Suggestions: option to edit the search term inline | P2 | M | none | [§2.5.2](./UX_REDESIGN.md#252-dictionarysuggestionsactivity) |
| `POLISH-DETAILED-STATS` | DetailedStats: reading timeline; per-book stats export to SD | P2 | S | none | [§2.6.2](./UX_REDESIGN.md#262-detailedstatsactivity) |
| `POLISH-NETMODE-COPY` | Network mode selection: explainer beneath each option | P2 | S | none | [§2.7.1](./UX_REDESIGN.md#271-networkmodeselectionactivity) |
| `POLISH-WIFI-FEEDBACK` | WiFi list: elapsed-time scan indicator, signal bars, lock icon, "saved" marker | P2 | M | none | [§2.7.2](./UX_REDESIGN.md#272-wifiselectionactivity) |
| `POLISH-WEBSERVER-LOG` | WebServer screen: live upload activity log + totals | P2 | M | refresh | [§2.7.3](./UX_REDESIGN.md#273-crosspointwebserveractivity) |
| `POLISH-CALIBRE-STATES` | Calibre connect: dynamic waiting state with helpful timeout copy | P2 | M | none | [§2.7.4](./UX_REDESIGN.md#274-calibreconnectactivity) |
| `POLISH-BUTTON-REMAP` | ButtonRemap: explicit reset-to-default + live "press to test" mode | P2 | S | none | [§2.8.3](./UX_REDESIGN.md#283-buttonremapactivity) |
| `POLISH-KOREADER-SYNC` | KOReaderSyncActivity: 5 explicit states (in-flight / no-op / push / pull / conflict / error) | P2 | M | none | [§2.9.1](./UX_REDESIGN.md#291-koreadersyncactivity) |
| `POLISH-FOOTNOTES` | Footnotes: back-jump from footnote to anchor | P3 | M | none | [§2.4.3](./UX_REDESIGN.md#243-epubreaderfootnotesactivity) |
| `POLISH-QR-LABEL` | Disambiguate the "Display QR" menu label | P3 | S | none | [§2.4.4](./UX_REDESIGN.md#244-qrdisplayactivity) |
| `POLISH-LOOKED-UP` | Looked-up words: group by book and date | P3 | M | heap | [§2.5.4](./UX_REDESIGN.md#254-lookeduwordsactivity) |
| `POLISH-LANG-COVERAGE` | Language selector: per-language translation % indicator | P3 | S | none | [§2.8.4](./UX_REDESIGN.md#284-languageselectactivity) |
| `POLISH-LOADING` | Standardize indeterminate waits on elapsed-time counter, not spinner-style | P3 | S | none | [§3.6](./UX_REDESIGN.md#36-loading--wait-states-on-e-ink) |
| `POLISH-THEME-AXES` | Split theme into "UI Style" × "Home Layout" independent settings | P3 | M | scope | [§3.7](./UX_REDESIGN.md#37-theme-system--direction) |
| `POLISH-CONFIRMATION-DEFAULTS` | Audit ConfirmationActivity callers: dangerous ops default to No | P3 | S | none | [§2.11.2](./UX_REDESIGN.md#2112-confirmationactivity) |

---

## Tier 1 — Pull-based fetch (committed)

Reading-adjacent WiFi features that pull data on user-initiated bursts. SCOPE-clean.

| ID | Title | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- |
| `T1-OPDS` | OPDS catalog manager — multi-source, with presets (Standard Ebooks, Gutenberg, Calibre) | P0 | L | heap, scope | [§4.1.1](./UX_REDESIGN.md#411-opds-catalog-manager) |
| `T1-ENRICH` | Cover + metadata enrichment from Open Library / Google Books (ISBN-keyed) | P0 | M | heap, flash | [§4.1.2](./UX_REDESIGN.md#412-cover--metadata-enrichment) |
| `T1-DICT-PACKS` | Dictionary pack manager: download per-language StarDict files | P1 | M | heap, scope | [§4.1.3](./UX_REDESIGN.md#413-dictionary-pack-manager) |
| `T1-CALIBRE-OPDS` | Calibre content-server browse (OPDS) as default preset of `T1-OPDS` | P2 | S | none (subsumed) | [§4.1.4](./UX_REDESIGN.md#414-calibre-content-server-browse-mode) |

---

## Tier 2 — Sync & enhancement (committed)

Bidirectional or enrichment features. Higher complexity, still SCOPE-clean.

| ID | Title | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- |
| `T2-PHONE-PAIRED-ENTRY` | Phone-paired credential entry (kills KeyboardEntryActivity friction) | P0 | L | heap, scope | [§4.2.6](./UX_REDESIGN.md#426-phone-paired-credential-entry-the-keyboardentryactivity-killer) |
| `T2-HIGHLIGHTS-LOCAL` | Highlights capture (offline-first, JSON per book on SD) | P0 | M | heap, flash | [§4.2.1](./UX_REDESIGN.md#421-highlights-capture-offline-first) |
| `T2-WIKIPEDIA` | Wikipedia preview alongside dictionary on word-select | P1 | M | heap | [§4.2.3](./UX_REDESIGN.md#423-wikipedia-preview-on-word-select) |
| `T2-KOREADER-PAIR` | KOReader pairing via 6-digit code (replaces typed credentials) | P1 | M | scope | [§4.2.7](./UX_REDESIGN.md#427-koreader-pairing-via-6-digit-code) |
| `T2-HIGHLIGHTS-SYNC` | Highlights sync to Readwise / Joplin webhook / custom webhook | P2 | M | scope | [§4.2.2](./UX_REDESIGN.md#422-highlights-sync-to-external-services) |
| `T2-TRANSLATE` | Translation API for selected words (DeepL / Google) | P2 | M | scope, heap, flash | [§4.2.4](./UX_REDESIGN.md#424-translation-api-for-selected-words) |
| `T2-EMAIL-GATEWAY` | "Send to AALU" email-to-device gateway (Kindle-like) | P3 | L | scope, security | [§4.2.5](./UX_REDESIGN.md#425-send-to-aalu-email-gateway) |

---

## Tier 3 — Reading-habit hub (decision-gated)

Materially shifts AALU from "personal reader" toward "reading hub." Each item below `T3-GOODREADS` needs a deliberate go/no-go before being adopted.

| ID | Title | Status | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- | --- |
| `T3-GOODREADS-V1` | Goodreads sync v1: read-only via RSS (shelves, Want-to-Read catalog, stats enrichment) | ✓ committed 2026-05-13 | P0 | M-L | scope, heap, legal | [§4.3.1](./UX_REDESIGN.md#431-goodreads-sync--committed-2026-05-13--deep-dive) |
| `T3-GOODREADS-V2` | Goodreads sync v2: write-back via self-hosted bridge service (progress, mark-as-read, rating) | ✓ committed (gated on T2-PHONE-PAIRED-ENTRY) | P1 | L | scope, legal, fragility | [§4.3.1](./UX_REDESIGN.md#431-goodreads-sync--committed-2026-05-13--deep-dive) |
| `T3-LIBBY` | Library borrowing catalog (Libby / OverDrive) — browse only, DRM blocks downloads | ⚠ decision needed | P3 | L | scope | [§4.3.2](./UX_REDESIGN.md#432-library-borrowing-catalog-libby--overdrive-) |
| `T3-READ-LATER` | Read-it-later integration (Wallabag / Omnivore) — articles as EPUB | ⚠ decision needed | P3 | L | scope | [§4.3.3](./UX_REDESIGN.md#433-read-it-later-integration--wallabag--pocket--omnivore) |
| `T3-RSS-EPUB` | RSS-as-EPUB digest (daily/weekly periodic fetch + packaging) | ⚠ decision needed | P3 | L | scope (most controversial vs SCOPE) | [§4.3.4](./UX_REDESIGN.md#434-rss-as-epub-digest-) |

---

## Local quality-of-life (no WiFi, low risk)

The safest, fastest-to-ship features. Most users benefit immediately.

| ID | Title | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- |
| `LOCAL-TUTORIAL` | First-launch tutorial: 4-card guided tour (buttons / menu / Aa / Transfer) | P0 | M | none | [§4.4.8](./UX_REDESIGN.md#448-first-launch-tutorial) |
| `LOCAL-BOOKMARKS` | Bookmarks in-book, multiple per book, durable | P0 | M | flash | [§4.4.1](./UX_REDESIGN.md#441-bookmarks) |
| `LOCAL-GOALS` | Reading goals (daily minutes/pages + yearly books); surfaced on Home + Stats | P0 | M | none | [§4.4.6](./UX_REDESIGN.md#446-reading-goals) |
| `LOCAL-SEARCH` | Library search across all books by title/author/series | P1 | M | heap | [§4.4.4](./UX_REDESIGN.md#444-library-search) |
| `LOCAL-TAGS` | Manual tags/collections (flat, no nesting) | P1 | M | scope | [§4.4.5](./UX_REDESIGN.md#445-manual-tags--collections) |
| `LOCAL-STATS-EXPORT` | Export current Stats view to SD as CSV/JSON | P2 | S | none | [§4.4.7](./UX_REDESIGN.md#447-stats-export-to-sd) |
| `LOCAL-NOTES` | Template-based per-book notes (tags only: 💡 / ❓ / 📌 / ↗) | P2 | M | scope | [§4.4.3](./UX_REDESIGN.md#443-per-book-notes-template-based-no-keyboard) |
| `LOCAL-HIGHLIGHTS` | Highlights (offline-only v1) — covered by `T2-HIGHLIGHTS-LOCAL` | — | — | — | [§4.4.2](./UX_REDESIGN.md#442-highlights-offline-only-no-sync) |

---

## Structural — kill or merge candidates

IA-level moves. High blast radius. Don't tackle alongside other work.

| ID | Title | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- |
| `STRUCT-LIBRARY-MERGE` | Merge Statistics + Series Viewer + Finished list into a unified `Library` tab | P1 | L | scope | [§5.2 #1](./UX_REDESIGN.md#52-kill-or-merge-these-candidates) |
| `STRUCT-STATUSBAR-COLLAPSE` | Collapse `StatusBarSettingsActivity` into Settings → Reading (after `POLISH-STATUSBAR` lands) | P2 | S | scope | [§5.2 #2](./UX_REDESIGN.md#52-kill-or-merge-these-candidates) |
| `STRUCT-XTC-DEPRECATE` | Decide whether to deprecate XTC reader; check telemetry / usage | P3 | S | scope | [§5.2 #3](./UX_REDESIGN.md#52-kill-or-merge-these-candidates) |

---

## SCOPE.md amendment

Not strictly a backlog task, but a prerequisite that should land before any Tier 1+ feature ships.

| ID | Title | Priority | Effort | Risk | Spec § |
| --- | --- | --- | --- | --- | --- |
| `META-SCOPE-AMEND` | Apply the SCOPE.md amendment (reading-adjacent connectivity carve-out) | P0 | S | scope | [Appendix A](./UX_REDESIGN.md#appendix-a--proposed-scopemd-amendment-verbatim) |

---

## Phase mapping (cross-reference to UX_REDESIGN §5.3)

| Phase | Items |
| --- | --- |
| **Phase 1 — Polish (~2-3w)** | `META-SCOPE-AMEND`, `POLISH-MENU`, `POLISH-STATUSBAR`, `POLISH-HINTS`, `POLISH-EMPTY-STATES`, `POLISH-ERROR-STATES`, `POLISH-BATTERY`, `POLISH-SETTINGS-REBALANCE`, `POLISH-CLEAR-CACHE`, `POLISH-OTA`, plus other P1/P2 polish items as time allows |
| **Phase 2 — Tier 1 (~4-6w)** | `T1-OPDS`, `T1-ENRICH`, `T1-DICT-PACKS`, `T1-CALIBRE-OPDS`, `LOCAL-BOOKMARKS`, `LOCAL-TUTORIAL`, `LOCAL-GOALS` |
| **Phase 3 — Tier 2 / structural (~6-8w)** | `T2-HIGHLIGHTS-LOCAL`, `T2-PHONE-PAIRED-ENTRY`, `T2-WIKIPEDIA`, `T2-KOREADER-PAIR`, `STRUCT-LIBRARY-MERGE`, `T3-GOODREADS-V1` |
| **Phase 4 — Goodreads write-back (~5-7w including bridge)** | `T3-GOODREADS-V2` |
| **Unscheduled / decision-needed** | `T2-HIGHLIGHTS-SYNC`, `T2-TRANSLATE`, `T2-EMAIL-GATEWAY`, `T3-LIBBY`, `T3-READ-LATER`, `T3-RSS-EPUB`, `STRUCT-XTC-DEPRECATE`, low-priority polish items |

---

## How to use this backlog

- **Pick the next item by**: filter to current Phase → sort by Priority → tiebreak by Effort (smaller wins build momentum).
- **Mark in-progress / done**: add a `Status` column or strike rows. Don't delete completed items; the historical log is useful for changelogs.
- **Add new items**: assign an ID following the existing convention (`<TIER>-<SLUG>`), pick a tier band, set priority relative to siblings.
- **Re-prioritize**: priorities are not gospel. If a user complains about a P3 item and ignores a P0 item, the priorities are wrong, not the user.
