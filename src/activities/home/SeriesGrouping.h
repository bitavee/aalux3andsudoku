#pragma once

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"  // for Rect

class GfxRenderer;

// Predicates and helpers used to decide whether two books belong to the same
// series stack on the home screen and inside the SeriesViewer drill-in. Lives
// in its own translation unit so HomeActivity (which builds the home tiles
// from `recentBooks`) and the SD-card discovery walker (which augments a
// stack with series members the user hasn't opened recently) share one
// definition of "same series" and never drift apart.
namespace SeriesGrouping {

// Normalised key used for matching: lowercased ASCII, internal whitespace
// runs collapsed, outer whitespace trimmed. Catches the common reasons two
// books in the same series fail to group despite "looking" identical -- one
// has a trailing space, two spaces between words, or differs in capitalisation.
std::string seriesKey(const std::string& s);

// True when both names are non-empty and share the same normalised key.
bool sameSeries(const std::string& a, const std::string& b);

// True iff both books *declare* a series and the names disagree. This is
// the only case where folder-based grouping should be vetoed -- two books
// that explicitly belong to different series shouldn't merge just because
// they happen to share a parent directory.
bool seriesConflict(const std::string& a, const std::string& b);

// Strip the filename component, returning the directory portion of a path.
// Returns empty for root-level entries.
std::string parentDir(const std::string& path);

// Two books group iff (a) they share a series name (matched tolerantly), or
// (b) they live in the same non-root parent folder AND don't have explicitly
// different series names. Same-folder is treated as the user's strong intent
// signal: when metadata is inconsistent or missing, the folder wins.
bool shouldGroup(const RecentBook& a, const RecentBook& b);

// Cache key used by `lookup/recordSeriesCount`: normalised series name when
// the seed has one, else `dir:<parentDir>` for folder-grouped tiles. Returns
// empty when no stable key can be derived (root-level book with no series).
std::string countCacheKey(const RecentBook& seed);

// Read the persisted count cache from SD. Cheap; safe to call on every
// HomeActivity entry. No-op when the file is missing.
void loadSeriesCounts();

// Returns the cached total for `key`, or -1 when not present. Counts are
// only recorded once a viewer drill-in has actually walked the disk, so a
// missing entry simply means "we haven't discovered yet" -- callers should
// fall back to whatever they can derive locally.
int lookupSeriesCount(const std::string& key);

// Persist `count` for `key` and update the in-memory cache. Skipped when
// `key` is empty or `count <= 0`.
void recordSeriesCount(const std::string& key, int count);

// Load the persisted book list for `key` into `outBooks`. Returns true on
// cache hit. Populated by `saveCachedBooks` after discoverSeriesMembers
// runs, so callers can skip the SD-card walk when the cache exists.
bool loadCachedBooks(const std::string& key, std::vector<RecentBook>& outBooks);

// Persist `books` as the canonical member list for `key`. Overwrites any
// previous file. No-op when `key` is empty.
bool saveCachedBooks(const std::string& key, const std::vector<RecentBook>& books);

// Augment the seed stack with every series member found on the SD card.
// Walks each unique parent folder of a seed book one level deep, parses each
// EPUB's metadata via the cheap `Epub::load(false, true)` path, and includes
// any book that would `shouldGroup()` with the first seed (the lowest-order
// series member, matching how HomeActivity builds the tile). Generates
// missing thumbnail BMPs synchronously so the viewer renders a complete
// grid on first open.
//
// Drives the popup progress bar drawn by the caller. The popup itself is
// the caller's responsibility -- this function only fills it.
//
// `maxBooks` caps the returned vector size so a folder containing hundreds
// of unrelated EPUBs doesn't stall the UI for tens of seconds. The seed
// stack is preserved verbatim and counts toward the cap, so the worst-case
// wall time scales with `maxBooks - seedBooks.size()`.
std::vector<RecentBook> discoverSeriesMembers(std::vector<RecentBook> seedBooks, GfxRenderer& renderer,
                                              const Rect& popupRect, int maxBooks = 100);

}  // namespace SeriesGrouping
