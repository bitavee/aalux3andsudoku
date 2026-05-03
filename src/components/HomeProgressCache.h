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
    int spineIndex;          // spineIndex that produced `progressPercent`, or -1
    bool tried;              // true once we've attempted to load - prevents retry storms
  };
  std::vector<Entry> entries;
  bool diskLoaded = false;  // lazy: loadFromDisk runs on first loadProgressFor call

  HomeProgressCache() = default;

  // Internal: read/write the persistent cache at /.crosspoint/home_progress.json.
  void loadFromDisk();
  void saveToDisk() const;

 public:
  static constexpr int8_t Unknown = -1;

  HomeProgressCache(const HomeProgressCache&) = delete;
  HomeProgressCache& operator=(const HomeProgressCache&) = delete;

  static HomeProgressCache& getInstance() { return instance; }

  // Returns the cached progress for `path`, or Unknown if not yet loaded or unavailable.
  int8_t getProgress(const std::string& path) const;

  // Reads progress.bin for `path` and produces a percentage. The expensive
  // BookMetadataCache load is only performed when the on-disk progress.bin
  // points at a different spine index than the persisted cache -- otherwise
  // the cached percent is returned immediately. Cache file lives at
  // /.crosspoint/home_progress.json so cold boots paint the hero ring
  // instantly without parsing book.bin for every recent.
  // Safe to call repeatedly - subsequent calls for the same path are no-ops.
  bool loadProgressFor(const std::string& path);

  // Push an authoritative (spineIndex, percent) for `path`, persisting to
  // disk so the home ring is up-to-date next entry without re-parsing
  // book.bin. Intended for the reader to call after saveProgress(): the
  // reader already has the percent computed for its status bar, so home
  // can reuse it for free. No-op when the value matches the current cache.
  void recordProgress(const std::string& path, int spineIndex, int8_t percent);

  // Drops all in-memory cached entries. Call when leaving the home activity
  // to free memory. The on-disk JSON is preserved so the next home entry
  // still benefits from the cache.
  void clear();
};
