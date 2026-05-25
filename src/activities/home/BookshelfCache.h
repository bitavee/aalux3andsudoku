#pragma once

#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"  // for Rect

class GfxRenderer;

// Persistent cache for the Bookshelf activity. The cache is the source of
// truth for the on-device library view -- the SD card is only walked on cold
// open, manual refresh, or after an inbound transfer invalidates the cache.
//
// Storage layout:
//   /.crosspoint/bookshelf.bin v1
//     u8  version (== 1)
//     u16 count
//     repeat `count` records:
//       u16 + bytes : path           (required, non-empty)
//       u16 + bytes : title          (display label for single-book tiles)
//       u16 + bytes : seriesName     (empty for standalone books)
//       u16 + bytes : seriesIndex    (empty when book has no parsed index)
//
// Records are written in display order (author -> seriesIndex -> title), so
// loaders never have to re-sort. Author is consumed at sort time and dropped
// from the on-device representation to keep RAM tight (Q15 design).
namespace BookshelfCache {

// Slim in-RAM record. Authoritative cover thumbnail path is computed on
// demand from `path` -- it's a pure hash, no need to store it.
struct Entry {
  std::string path;
  std::string title;
  std::string seriesName;
  std::string seriesIndex;
};

// True iff the cache file exists on disk. Drives the cold-cache auto-scan
// path: when this returns false, the activity must scan before rendering.
bool exists();

// Read every record from the cache file into `out`. Returns true on success
// (file present, version matches, all records parsed). On any failure the
// caller should treat the cache as cold and trigger `scan`.
bool load(std::vector<Entry>& out);

// Overwrite the cache file with `entries` in their current order. Callers
// must sort first; this function does not reorder.
bool save(const std::vector<Entry>& entries);

// Delete the cache file. Used after File Transfer / OPDS download so the
// next bookshelf open re-scans the SD card. Idempotent.
void invalidate();

// Remove the entry whose `path` matches and rewrite the cache file. Used
// after the user deletes a book via long-press. Returns true if the path
// was found and removed.
bool removeBook(const std::string& path);

// Full SD walk: enumerate every .epub/.xtc under the card, parse metadata
// via the cheap `Epub::load(false, true)` path, regenerate any missing
// cover thumbnails at `HomeRenderer::kThumbnailCoverHeight`, sort, and
// persist to the cache file. `out` receives the slimmed, sorted list.
//
// Progress is reported by filling `popupRect` -- the caller is responsible
// for drawing the surrounding popup chrome before invoking. `maxBooks`
// caps the result so a card with thousands of EPUBs doesn't hang the UI
// for minutes; books beyond the cap are skipped silently with a LOG_DBG.
bool scan(GfxRenderer& renderer, const Rect& popupRect, std::vector<Entry>& out, int maxBooks = 300);

}  // namespace BookshelfCache
