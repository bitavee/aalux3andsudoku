#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Lazy session cache for per-book reading progress shown on the home screen hero.
//
// The progress is computed from the on-disk EPUB cache:
//   * `<cacheRoot>/epub_<hash>/progress.bin`        - current spine index
//   * `<cacheRoot>/epub_<hash>/book.bin` (via BookMetadataCache) - total spine count
//
// Progress % is approximated as (currentSpineIndex / spineCount) * 100. This is
// chapter-granular, not byte-granular, but it's free at home-render time once we
// have the spine count cached. We accept the approximation in exchange for not
// touching the recents binary format.
//
// Non-EPUB recents (TXT, XTC) are not supported here yet - getProgress() returns
// HomeProgressCache::Unknown for them and the hero renderer skips the progress
// bar in that case.
class HomeProgressCache {
  static HomeProgressCache instance;

  struct Entry {
    std::string path;
    int8_t progressPercent;  // 0..100, or Unknown
    bool tried;              // true once we've attempted to load - prevents retry storms
  };
  std::vector<Entry> entries;

  HomeProgressCache() = default;

 public:
  static constexpr int8_t Unknown = -1;

  HomeProgressCache(const HomeProgressCache&) = delete;
  HomeProgressCache& operator=(const HomeProgressCache&) = delete;

  static HomeProgressCache& getInstance() { return instance; }

  // Returns the cached progress for `path`, or Unknown if not yet loaded or unavailable.
  int8_t getProgress(const std::string& path) const;

  // Reads progress.bin + book.bin for `path` and caches the result.
  // Returns true if a real % was computed.
  // Safe to call repeatedly - subsequent calls for the same path are no-ops.
  bool loadProgressFor(const std::string& path);

  // Drops all cached entries. Call when leaving the home activity to free memory.
  void clear();
};
