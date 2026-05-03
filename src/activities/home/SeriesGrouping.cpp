#include "SeriesGrouping.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "components/HomeRenderer.h"
#include "components/UITheme.h"

namespace SeriesGrouping {

std::string seriesKey(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool prevSpace = true;  // skip leading whitespace
  for (char c : s) {
    const bool isSpace = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (isSpace) {
      if (!prevSpace) out.push_back(' ');
      prevSpace = true;
      continue;
    }
    char lc = c;
    if (lc >= 'A' && lc <= 'Z') lc = static_cast<char>(lc + 32);
    out.push_back(lc);
    prevSpace = false;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool sameSeries(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  return seriesKey(a) == seriesKey(b);
}

bool seriesConflict(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  return seriesKey(a) != seriesKey(b);
}

std::string parentDir(const std::string& path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  return path.substr(0, slash);
}

bool shouldGroup(const RecentBook& a, const RecentBook& b) {
  if (sameSeries(a.seriesName, b.seriesName)) return true;
  if (seriesConflict(a.seriesName, b.seriesName)) return false;
  const std::string da = parentDir(a.path);
  const std::string db = parentDir(b.path);
  if (da.empty() || db.empty()) return false;  // root-level books don't fold by folder
  return da == db;
}

namespace {

// True when `path` already appears in `paths`. Linear scan, fine for the
// small N we deal with (cap is 100).
bool containsPath(const std::vector<std::string>& paths, const std::string& path) {
  return std::any_of(paths.begin(), paths.end(), [&](const std::string& p) { return p == path; });
}

void emitProgress(GfxRenderer& renderer, const Rect& popupRect, int processed, int total) {
  const int denom = total > 0 ? total : 1;
  int pct = (processed * 100) / denom;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  UITheme::getInstance().getTheme().fillPopupProgress(renderer, popupRect, pct);
}

// --- Series count cache ---
// Persisted at /.crosspoint/series_counts.json so the home thumbnail badge
// can show the real total for a series, not just the count of members that
// happen to survive in `recentBooks`. Counts are only written after a viewer
// drill-in actually walks the disk -- so the badge corrects itself the next
// time the user lands on home, and stays correct across cold boots.
constexpr char kCountsFile[] = "/.crosspoint/series_counts.json";
constexpr char kBooksDir[] = "/.crosspoint/series_books";
std::unordered_map<std::string, int> seriesCounts;
bool seriesCountsLoaded = false;

// Hash-encoded filesystem-safe filename for the per-series book cache.
// std::hash<std::string> is plenty: a hypothetical collision overwrites
// the loser, who simply rebuilds on next scan. Cheap, no path-sanitisation
// edge cases.
std::string keyToBooksFile(const std::string& key) {
  const uint64_t h = std::hash<std::string>{}(key);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s/%016llx.json", kBooksDir, static_cast<unsigned long long>(h));
  return buf;
}

bool persistSeriesCounts() {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  for (const auto& [key, count] : seriesCounts) {
    if (count <= 0) continue;
    obj[key] = count;
  }
  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  return Storage.writeFile(kCountsFile, json);
}

}  // namespace

std::string countCacheKey(const RecentBook& seed) {
  if (!seed.seriesName.empty()) return seriesKey(seed.seriesName);
  const std::string dir = parentDir(seed.path);
  if (dir.empty()) return {};
  return std::string("dir:") + dir;
}

void loadSeriesCounts() {
  seriesCountsLoaded = true;
  seriesCounts.clear();
  if (!Storage.exists(kCountsFile)) return;
  String json = Storage.readFile(kCountsFile);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) {
    LOG_ERR("SDISC", "series_counts.json parse failed");
    return;
  }
  JsonObject obj = doc.as<JsonObject>();
  for (JsonPair kv : obj) {
    const int count = kv.value().as<int>();
    if (count > 0) {
      seriesCounts.emplace(kv.key().c_str(), count);
    }
  }
  LOG_DBG("SDISC", "Loaded %d series counts", static_cast<int>(seriesCounts.size()));
}

int lookupSeriesCount(const std::string& key) {
  if (!seriesCountsLoaded) loadSeriesCounts();
  if (key.empty()) return -1;
  auto it = seriesCounts.find(key);
  return (it == seriesCounts.end()) ? -1 : it->second;
}

void recordSeriesCount(const std::string& key, int count) {
  if (key.empty() || count <= 0) return;
  if (!seriesCountsLoaded) loadSeriesCounts();
  auto it = seriesCounts.find(key);
  if (it != seriesCounts.end() && it->second == count) return;  // no-op write avoidance
  seriesCounts[key] = count;
  if (!persistSeriesCounts()) {
    LOG_ERR("SDISC", "Failed to persist series_counts.json");
  }
}

bool loadCachedBooks(const std::string& key, std::vector<RecentBook>& outBooks) {
  if (key.empty()) return false;
  const std::string path = keyToBooksFile(key);
  if (!Storage.exists(path.c_str())) return false;

  String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) {
    LOG_ERR("SDISC", "series_books cache parse failed for %s", path.c_str());
    return false;
  }

  JsonArray arr = doc["books"].as<JsonArray>();
  outBooks.clear();
  outBooks.reserve(arr.size());
  for (JsonObject obj : arr) {
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    book.seriesName = obj["seriesName"] | std::string("");
    book.seriesIndex = obj["seriesIndex"] | std::string("");
    if (book.path.empty()) continue;
    outBooks.push_back(std::move(book));
  }
  return !outBooks.empty();
}

bool saveCachedBooks(const std::string& key, const std::vector<RecentBook>& books) {
  if (key.empty() || books.empty()) return false;

  JsonDocument doc;
  doc["key"] = key;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const RecentBook& book : books) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
    if (!book.seriesName.empty()) obj["seriesName"] = book.seriesName;
    if (!book.seriesIndex.empty()) obj["seriesIndex"] = book.seriesIndex;
  }

  Storage.mkdir(kBooksDir);
  String json;
  serializeJson(doc, json);
  const std::string path = keyToBooksFile(key);
  if (!Storage.writeFile(path.c_str(), json)) {
    LOG_ERR("SDISC", "Failed to write series_books cache: %s", path.c_str());
    return false;
  }
  return true;
}

std::vector<RecentBook> discoverSeriesMembers(std::vector<RecentBook> seedBooks, GfxRenderer& renderer,
                                              const Rect& popupRect, int maxBooks) {
  if (seedBooks.empty()) return seedBooks;
  if (maxBooks <= 0) return seedBooks;

  std::vector<RecentBook> result = std::move(seedBooks);

  // Already-included paths drive dedup. Pre-seeded with the stack so we
  // never re-emit a book the home screen already knew about.
  std::vector<std::string> existingPaths;
  existingPaths.reserve(result.size() + 16);
  for (const auto& b : result) existingPaths.push_back(b.path);

  // Unique parent folders to walk. A series may legitimately span more than
  // one folder if the user reorganised mid-read; mirroring that here means
  // discovery stays consistent with the existing tile-grouping rules.
  std::vector<std::string> folders;
  folders.reserve(result.size());
  for (const auto& b : result) {
    const std::string dir = parentDir(b.path);
    if (dir.empty()) continue;
    if (std::find(folders.begin(), folders.end(), dir) == folders.end()) {
      folders.push_back(dir);
    }
  }
  if (folders.empty()) return result;

  // Reference seed: the first seed book, which is also the lowest-order
  // member after HomeActivity::buildTiles sorts the tile. Any candidate
  // that `shouldGroup` with this reference is part of the stack.
  const RecentBook reference = result[0];

  // First pass: enumerate EPUB filenames in each folder. Cheap (no parsing).
  std::vector<std::pair<std::string, std::string>> candidates;  // (folder, filename)
  candidates.reserve(64);
  for (const std::string& folder : folders) {
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
      if (isDir) continue;
      const std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename)) {
        candidates.emplace_back(folder, std::string(name));
      }
    }
    root.close();
  }

  const int totalCandidates = static_cast<int>(candidates.size());
  emitProgress(renderer, popupRect, 0, totalCandidates);

  // Second pass: parse metadata, apply group predicate, dedup, append.
  // Sequential to keep peak heap low -- one Epub object alive at a time.
  int processed = 0;
  for (const auto& [folder, filename] : candidates) {
    ++processed;
    if (static_cast<int>(result.size()) >= maxBooks) {
      emitProgress(renderer, popupRect, processed, totalCandidates);
      continue;
    }

    const std::string fullPath = folder + "/" + filename;
    if (containsPath(existingPaths, fullPath)) {
      emitProgress(renderer, popupRect, processed, totalCandidates);
      continue;
    }

    Epub epub(fullPath, "/.crosspoint");
    if (!epub.load(false, true)) {
      emitProgress(renderer, popupRect, processed, totalCandidates);
      continue;
    }

    RecentBook candidate{fullPath,
                         epub.getTitle(),
                         epub.getAuthor(),
                         epub.getThumbBmpPath(),
                         epub.getSeriesName(),
                         epub.getSeriesIndex()};

    if (!shouldGroup(reference, candidate)) {
      emitProgress(renderer, popupRect, processed, totalCandidates);
      continue;
    }

    // Generate the thumbnail BMP if we know the cover path but the cached
    // thumb at the home-screen size is missing. The viewer reads from the
    // same `getCoverThumbPath(... kThumbnailCoverHeight)` so a single gen
    // here covers every subsequent open.
    if (!candidate.coverBmpPath.empty()) {
      const std::string thumbPath =
          UITheme::getCoverThumbPath(candidate.coverBmpPath, HomeRenderer::kThumbnailCoverHeight);
      if (!Storage.exists(thumbPath.c_str())) {
        if (!epub.generateThumbBmp(HomeRenderer::kThumbnailCoverHeight)) {
          // Cover unavailable -- viewer will draw the empty-rect fallback.
          candidate.coverBmpPath = "";
        }
      }
    }

    existingPaths.push_back(candidate.path);
    result.push_back(std::move(candidate));

    emitProgress(renderer, popupRect, processed, totalCandidates);
  }

  LOG_DBG("SDISC", "Series discovery: total=%d candidates=%d cap=%d", static_cast<int>(result.size()),
          totalCandidates, maxBooks);

  // Persist the count so the home thumbnail badge can show the real total
  // even before the user re-opens the viewer.
  recordSeriesCount(countCacheKey(reference), static_cast<int>(result.size()));

  return result;
}

}  // namespace SeriesGrouping
