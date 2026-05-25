#include "BookshelfCache.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "components/HomeRenderer.h"
#include "components/UITheme.h"

namespace BookshelfCache {

namespace {

constexpr const char* kCacheFile = "/.crosspoint/bookshelf.bin";
constexpr uint8_t kVersion = 1;
constexpr int kProgressUpdateInterval = 2;  // redraw popup every N books

// Pre-sort working record. `author` is consumed by the sort comparator and
// dropped before persisting -- the cache file holds the slim shape.
struct ScanRecord {
  std::string path;
  std::string title;
  std::string author;
  std::string seriesName;
  std::string seriesIndex;
};

void emitProgress(GfxRenderer& renderer, const Rect& popupRect, int processed, int total) {
  const int denom = total > 0 ? total : 1;
  int pct = (processed * 100) / denom;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  UITheme::getInstance().getTheme().fillPopupProgress(renderer, popupRect, pct);
}

bool writeU8(HalFile& file, uint8_t value) { return file.write(&value, 1) == 1; }

bool writeU16(HalFile& file, uint16_t value) {
  const uint8_t buf[2] = {static_cast<uint8_t>(value & 0xff), static_cast<uint8_t>(value >> 8)};
  return file.write(buf, 2) == 2;
}

bool writeString(HalFile& file, const std::string& s) {
  const size_t len = s.size();
  if (len > 0xffff) {
    LOG_ERR("BSC", "String too long to persist (%zu bytes)", len);
    return false;
  }
  if (!writeU16(file, static_cast<uint16_t>(len))) return false;
  if (len == 0) return true;
  return file.write(s.data(), len) == static_cast<int>(len);
}

bool readU8(HalFile& file, uint8_t& out) { return file.read(&out, 1) == 1; }

bool readU16(HalFile& file, uint16_t& out) {
  uint8_t buf[2];
  if (file.read(buf, 2) != 2) return false;
  out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
  return true;
}

bool readString(HalFile& file, std::string& out) {
  uint16_t len = 0;
  if (!readU16(file, len)) return false;
  out.clear();
  if (len == 0) return true;
  out.resize(len);
  return file.read(out.data(), len) == static_cast<int>(len);
}

// Sort by author (empty last) -> series name (empty last) -> series index
// (numeric) -> title. Mirrors the user-facing rule: series cluster together
// under their author, ordered by reading sequence; standalones follow within
// the same author.
bool compareRecords(const ScanRecord& a, const ScanRecord& b) {
  const std::string& aa = a.author.empty() ? std::string("~~~") : a.author;
  const std::string& ba = b.author.empty() ? std::string("~~~") : b.author;
  if (aa != ba) return aa < ba;

  const std::string& as = a.seriesName.empty() ? std::string("~~~") : a.seriesName;
  const std::string& bs = b.seriesName.empty() ? std::string("~~~") : b.seriesName;
  if (as != bs) return as < bs;

  const float ai = a.seriesIndex.empty() ? 0.0f : std::strtof(a.seriesIndex.c_str(), nullptr);
  const float bi = b.seriesIndex.empty() ? 0.0f : std::strtof(b.seriesIndex.c_str(), nullptr);
  if (ai != bi) return ai < bi;

  return a.title < b.title;
}

// Iterative recursive directory walk. Iterative (worklist of folder paths)
// rather than recursive function calls so deeply nested SD layouts cannot
// blow the main task stack. Collects every .epub/.xtc full path.
void enumerateBooks(std::vector<std::string>& outPaths, int maxBooks) {
  std::vector<std::string> worklist;
  worklist.reserve(8);
  worklist.emplace_back("/");

  while (!worklist.empty() && static_cast<int>(outPaths.size()) < maxBooks * 4) {
    const std::string folder = std::move(worklist.back());
    worklist.pop_back();

    HalFile root = Storage.open(folder.c_str());
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      continue;
    }
    root.rewindDirectory();

    char name[500];
    for (HalFile file = root.openNextFile(); file; file = root.openNextFile()) {
      file.getName(name, sizeof(name));
      const bool isDir = file.isDirectory();
      file.close();

      // Skip dot-prefixed entries (covers `.crosspoint`, `.sleep`, `.git`,
      // etc.) plus the well-known Windows volume metadata directory.
      if (name[0] == '.' || std::strcmp(name, "System Volume Information") == 0) continue;

      std::string childPath = folder;
      if (childPath.empty() || childPath.back() != '/') childPath.push_back('/');
      childPath.append(name);

      if (isDir) {
        worklist.push_back(std::move(childPath));
        continue;
      }

      const std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
        outPaths.push_back(std::move(childPath));
      }
    }
    root.close();
  }
}

}  // namespace

bool exists() { return Storage.exists(kCacheFile); }

bool load(std::vector<Entry>& out) {
  out.clear();

  HalFile file;
  if (!Storage.openFileForRead("BSC", kCacheFile, file)) return false;

  uint8_t version = 0;
  if (!readU8(file, version) || version != kVersion) {
    LOG_ERR("BSC", "Unknown bookshelf cache version %u (expected %u)", version, kVersion);
    file.close();
    return false;
  }

  uint16_t count = 0;
  if (!readU16(file, count)) {
    file.close();
    return false;
  }

  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Entry entry;
    if (!readString(file, entry.path) || !readString(file, entry.title) || !readString(file, entry.seriesName) ||
        !readString(file, entry.seriesIndex)) {
      LOG_ERR("BSC", "Truncated cache file at entry %u/%u", i, count);
      file.close();
      out.clear();
      return false;
    }
    if (entry.path.empty()) continue;
    out.push_back(std::move(entry));
  }
  file.close();
  LOG_DBG("BSC", "Loaded %d entries from cache", static_cast<int>(out.size()));
  return true;
}

bool save(const std::vector<Entry>& entries) {
  Storage.mkdir("/.crosspoint");

  HalFile file;
  if (!Storage.openFileForWrite("BSC", kCacheFile, file)) {
    LOG_ERR("BSC", "Failed to open cache file for write");
    return false;
  }

  const size_t count = std::min<size_t>(entries.size(), 0xffff);
  if (!writeU8(file, kVersion) || !writeU16(file, static_cast<uint16_t>(count))) {
    LOG_ERR("BSC", "Failed to write cache header");
    file.close();
    return false;
  }

  for (size_t i = 0; i < count; ++i) {
    const Entry& e = entries[i];
    if (!writeString(file, e.path) || !writeString(file, e.title) || !writeString(file, e.seriesName) ||
        !writeString(file, e.seriesIndex)) {
      LOG_ERR("BSC", "Failed to write entry %zu", i);
      file.close();
      return false;
    }
  }

  file.close();
  LOG_DBG("BSC", "Saved %zu entries to cache", count);
  return true;
}

void invalidate() {
  if (!Storage.exists(kCacheFile)) return;
  if (Storage.remove(kCacheFile)) {
    LOG_DBG("BSC", "Invalidated bookshelf cache");
  } else {
    LOG_ERR("BSC", "Failed to remove cache file");
  }
}

bool removeBook(const std::string& path) {
  std::vector<Entry> entries;
  if (!load(entries)) return false;

  auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.path == path; });
  if (it == entries.end()) return false;

  entries.erase(it);
  return save(entries);
}

bool scan(GfxRenderer& renderer, const Rect& popupRect, std::vector<Entry>& out, int maxBooks) {
  out.clear();
  if (maxBooks <= 0) return true;

  std::vector<std::string> paths;
  paths.reserve(64);
  enumerateBooks(paths, maxBooks);

  const int totalCandidates = static_cast<int>(paths.size());
  emitProgress(renderer, popupRect, 0, totalCandidates);

  std::vector<ScanRecord> records;
  records.reserve(std::min<int>(totalCandidates, maxBooks));

  int processed = 0;
  for (const std::string& fullPath : paths) {
    ++processed;
    if (static_cast<int>(records.size()) >= maxBooks) {
      if (processed % kProgressUpdateInterval == 0) emitProgress(renderer, popupRect, processed, totalCandidates);
      continue;
    }

    Epub epub(fullPath, "/.crosspoint");
    // buildIfMissing=true: on a cold full-library scan most books have
    // never been opened, so their per-book metadata cache doesn't exist
    // yet. We need to parse content.opf and build the cache here -- the
    // load(false, true) path silently dropped every unseen book, which is
    // why first-scan only surfaced the books already in Home's recents.
    // skipLoadingCss=true keeps the parse cheap by avoiding the CSS pass
    // we don't need for a metadata-only scan.
    if (!epub.load(true, true)) {
      LOG_DBG("BSC", "Skip unparseable: %s", fullPath.c_str());
      if (processed % kProgressUpdateInterval == 0) emitProgress(renderer, popupRect, processed, totalCandidates);
      continue;
    }

    ScanRecord rec;
    rec.path = fullPath;
    rec.title = epub.getTitle();
    rec.author = epub.getAuthor();
    rec.seriesName = epub.getSeriesName();
    rec.seriesIndex = epub.getSeriesIndex();
    if (rec.title.empty()) {
      // Fall back to filename minus extension so the tile is at least
      // recognisable when an EPUB lacks <dc:title>.
      const auto slash = fullPath.find_last_of('/');
      const auto dot = fullPath.find_last_of('.');
      const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
      const size_t end = (dot == std::string::npos || dot < start) ? fullPath.size() : dot;
      rec.title = fullPath.substr(start, end - start);
    }

    // Ensure the cover thumbnail exists at the bookshelf cell height.
    // generateThumbBmp short-circuits when the file already exists, so this
    // is safe to call unconditionally. The earlier version of this code
    // checked existence using `getCoverBmpPath()` -- which returns the
    // un-resized cover.bmp path, not the thumb -- so books that already had
    // a cover.bmp would skip thumb generation entirely and render as empty
    // rect placeholders in the grid.
    epub.generateThumbBmp(HomeRenderer::kThumbnailCoverHeight);

    records.push_back(std::move(rec));

    if (processed % kProgressUpdateInterval == 0) emitProgress(renderer, popupRect, processed, totalCandidates);
  }
  emitProgress(renderer, popupRect, totalCandidates, totalCandidates);

  std::sort(records.begin(), records.end(), compareRecords);

  out.reserve(records.size());
  for (auto& rec : records) {
    Entry entry;
    entry.path = std::move(rec.path);
    entry.title = std::move(rec.title);
    entry.seriesName = std::move(rec.seriesName);
    entry.seriesIndex = std::move(rec.seriesIndex);
    out.push_back(std::move(entry));
  }

  const bool persisted = save(out);
  LOG_DBG("BSC", "Scan complete: %d books (of %d candidates), persisted=%d", static_cast<int>(out.size()),
          totalCandidates, persisted ? 1 : 0);
  return persisted;
}

}  // namespace BookshelfCache
