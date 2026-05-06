#include "HomeProgressCache.h"

#include <ArduinoJson.h>
#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <functional>
#include <string>

HomeProgressCache HomeProgressCache::instance;

namespace {
constexpr const char* kCacheRoot = "/.crosspoint";
constexpr const char* kProgressCacheFile = "/.crosspoint/home_progress.json";

// Mirrors the cache path scheme used by Epub::Epub() so that the home screen
// can locate progress.bin without instantiating an Epub object.
std::string cachePathFor(const std::string& bookPath) {
  return std::string(kCacheRoot) + "/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
}

// Snapshot of progress.bin contents. Format (matches EpubReaderActivity::saveProgress):
//   bytes 0..1: spineIndex   (LE uint16)
//   bytes 2..3: chapterPage  (LE uint16)
//   bytes 4..5: chapterTotal (LE uint16, optional)
struct ProgressSnapshot {
  int spineIndex = -1;
  int chapterPage = 0;
  int chapterTotal = 0;
  bool hasPageInfo = false;
};

bool readProgress(const std::string& cachePath, ProgressSnapshot& out) {
  FsFile f;
  if (!Storage.openFileForRead("HPC", cachePath + "/progress.bin", f)) {
    return false;
  }
  uint8_t data[6];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize < 4) {
    return false;
  }
  out.spineIndex = static_cast<int>(data[0]) | (static_cast<int>(data[1]) << 8);
  out.chapterPage = static_cast<int>(data[2]) | (static_cast<int>(data[3]) << 8);
  if (dataSize >= 6) {
    out.chapterTotal = static_cast<int>(data[4]) | (static_cast<int>(data[5]) << 8);
    out.hasPageInfo = (out.chapterTotal > 0);
  }
  return true;
}
}  // namespace

int8_t HomeProgressCache::getProgress(const std::string& path) const {
  for (const auto& entry : entries) {
    if (entry.path == path) {
      return entry.progressPercent;
    }
  }
  return Unknown;
}

void HomeProgressCache::loadFromDisk() {
  diskLoaded = true;
  if (!Storage.exists(kProgressCacheFile)) return;

  String json = Storage.readFile(kProgressCacheFile);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) {
    LOG_ERR("HPC", "home_progress.json parse failed");
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  for (JsonPair kv : obj) {
    JsonObject inner = kv.value().as<JsonObject>();
    Entry entry;
    entry.path = kv.key().c_str();
    entry.spineIndex = inner["spineIndex"] | -1;
    const int p = inner["percent"] | -1;
    entry.progressPercent = (p >= 0 && p <= 100) ? static_cast<int8_t>(p) : Unknown;
    entry.tried = false;  // re-validate against on-disk progress.bin on first lookup
    if (entry.spineIndex >= 0 && entry.progressPercent != Unknown) {
      entries.push_back(std::move(entry));
    }
  }
}

void HomeProgressCache::saveToDisk() const {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  for (const auto& entry : entries) {
    if (entry.spineIndex < 0 || entry.progressPercent == Unknown) continue;
    JsonObject inner = obj[entry.path].to<JsonObject>();
    inner["spineIndex"] = entry.spineIndex;
    inner["percent"] = static_cast<int>(entry.progressPercent);
  }

  Storage.mkdir(kCacheRoot);
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(kProgressCacheFile, json)) {
    LOG_ERR("HPC", "Failed to persist home_progress.json");
  }
}

bool HomeProgressCache::loadProgressFor(const std::string& path) {
  if (!diskLoaded) loadFromDisk();

  // Already validated this session -- short-circuit.
  for (const auto& entry : entries) {
    if (entry.path == path && entry.tried) {
      return entry.progressPercent != Unknown;
    }
  }

  // Only EPUBs are supported -- TXT/XTC use a different progress format.
  if (!FsHelpers::hasEpubExtension(path)) {
    bool updated = false;
    for (auto& e : entries) {
      if (e.path == path) {
        e.progressPercent = Unknown;
        e.tried = true;
        updated = true;
        break;
      }
    }
    if (!updated) entries.push_back({path, Unknown, -1, true});
    return false;
  }

  const std::string cachePath = cachePathFor(path);
  ProgressSnapshot snap;
  if (!readProgress(cachePath, snap)) {
    bool updated = false;
    for (auto& e : entries) {
      if (e.path == path) {
        e.progressPercent = Unknown;
        e.tried = true;
        updated = true;
        break;
      }
    }
    if (!updated) entries.push_back({path, Unknown, -1, true});
    return false;
  }
  const int currentSpineIndex = snap.spineIndex;

  // Slow path resolves spineCount via book.bin. Cache it lazily so we can
  // detect end-of-book on the fast path without paying for a parse.
  auto resolveEndOfBook = [&](int spineCount) {
    return spineCount > 0 && currentSpineIndex == spineCount - 1 && snap.hasPageInfo &&
           snap.chapterPage >= snap.chapterTotal - 1;
  };

  // Fast path: persisted entry's spineIndex matches the on-disk progress.bin
  // -- the cached percent is still valid (no book.bin parsing needed) UNLESS
  // it's a stale 99% from before the end-of-book clamp shipped, in which
  // case fall through to the slow path so we recompute and persist 100%.
  for (auto& e : entries) {
    if (e.path == path && e.spineIndex == currentSpineIndex && e.progressPercent != Unknown) {
      // If we have page info that says we're at end-of-book and the cache
      // says < 100, force a recompute via the slow path. Otherwise trust
      // the cache.
      if (snap.hasPageInfo && e.progressPercent < 100) {
        // Defer the spineCount check to the slow path, which already loads
        // book.bin and can resolve spineCount cheaply.
      } else {
        e.tried = true;
        return true;
      }
      break;
    }
  }

  // Slow path: spineIndex changed (or no cached entry yet, or cache was
  // stale at end-of-book). Parse book.bin to compute the byte-weighted
  // percent, then persist for the next entry.
  BookMetadataCache cache(cachePath);
  if (!cache.load()) {
    LOG_DBG("HPC", "BookMetadataCache load failed for %s", path.c_str());
    bool updated = false;
    for (auto& e : entries) {
      if (e.path == path) {
        e.progressPercent = Unknown;
        e.tried = true;
        updated = true;
        break;
      }
    }
    if (!updated) entries.push_back({path, Unknown, -1, true});
    return false;
  }

  const int spineCount = cache.getSpineCount();
  if (spineCount <= 0) {
    return false;
  }

  // Match Epub::calculateProgress(spineIndex, 0.0f): byte-weighted progress
  // computed from cumulative spine sizes. Without this, the home and the
  // reader status bar disagree about the same book.
  const size_t bookSize = cache.getSpineEntry(spineCount - 1).cumulativeSize;
  if (bookSize == 0) {
    return false;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? cache.getSpineEntry(currentSpineIndex - 1).cumulativeSize : 0;

  int percent;
  if (resolveEndOfBook(spineCount)) {
    // Last page of last chapter: clamp to 100. Mirrors Epub::progressPercent
    // so home, status bar, and stats all agree on "finished" books.
    percent = 100;
  } else {
    percent = static_cast<int>((prevChapterSize * 100) / bookSize);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
  }

  bool updated = false;
  for (auto& e : entries) {
    if (e.path == path) {
      e.progressPercent = static_cast<int8_t>(percent);
      e.spineIndex = currentSpineIndex;
      e.tried = true;
      updated = true;
      break;
    }
  }
  if (!updated) {
    entries.push_back({path, static_cast<int8_t>(percent), currentSpineIndex, true});
  }

  saveToDisk();
  return true;
}

void HomeProgressCache::recordProgress(const std::string& path, int spineIndex, int8_t percent) {
  if (path.empty() || spineIndex < 0) return;
  if (percent < 0) percent = Unknown;
  if (percent > 100) percent = 100;
  if (!diskLoaded) loadFromDisk();

  for (auto& e : entries) {
    if (e.path == path) {
      if (e.spineIndex == spineIndex && e.progressPercent == percent) return;  // no-op write avoidance
      e.spineIndex = spineIndex;
      e.progressPercent = percent;
      e.tried = true;
      saveToDisk();
      return;
    }
  }
  entries.push_back({path, percent, spineIndex, true});
  saveToDisk();
}

void HomeProgressCache::clear() {
  entries.clear();
  // Reset the disk-loaded flag so the next home entry re-reads
  // /.crosspoint/home_progress.json (which may have been updated by another
  // activity, e.g. the reader saving progress.bin).
  diskLoaded = false;
}
