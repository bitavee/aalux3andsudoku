#include "HomeProgressCache.h"

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

// Mirrors the cache path scheme used by Epub::Epub() so that the home screen
// can locate progress.bin without instantiating an Epub object.
std::string cachePathFor(const std::string& bookPath) {
  return std::string(kCacheRoot) + "/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
}

// Reads the spine index from progress.bin. Format (matches EpubReaderActivity::saveProgress):
//   bytes 0..1: spineIndex   (LE uint16)
//   bytes 2..3: chapterPage  (LE uint16)
//   bytes 4..5: chapterTotal (LE uint16, optional)
// Returns -1 on read failure.
int readSpineIndex(const std::string& cachePath) {
  FsFile f;
  if (!Storage.openFileForRead("HPC", cachePath + "/progress.bin", f)) {
    return -1;
  }
  uint8_t data[6];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize < 4) {
    return -1;
  }
  return static_cast<int>(data[0]) | (static_cast<int>(data[1]) << 8);
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

bool HomeProgressCache::loadProgressFor(const std::string& path) {
  for (const auto& entry : entries) {
    if (entry.path == path && entry.tried) {
      return entry.progressPercent != Unknown;
    }
  }

  Entry result{path, Unknown, true};

  // Only EPUBs are supported for now - TXT/XTC have a different progress format
  // and would need their own loaders.
  if (!FsHelpers::hasEpubExtension(path)) {
    entries.push_back(std::move(result));
    return false;
  }

  const std::string cachePath = cachePathFor(path);
  const int spineIndex = readSpineIndex(cachePath);
  if (spineIndex < 0) {
    entries.push_back(std::move(result));
    return false;
  }

  BookMetadataCache cache(cachePath);
  if (!cache.load()) {
    LOG_DBG("HPC", "BookMetadataCache load failed for %s", path.c_str());
    entries.push_back(std::move(result));
    return false;
  }

  const int spineCount = cache.getSpineCount();
  if (spineCount <= 0) {
    entries.push_back(std::move(result));
    return false;
  }

  // Match Epub::calculateProgress(spineIndex, 0.0f): byte-weighted progress
  // computed from cumulative spine sizes. Plain (spineIndex / spineCount) is
  // wrong because chapters vary widely in length, and the reader's status bar
  // uses the byte-weighted form. Without this, the home and the reader show
  // different numbers for the same book.
  const size_t bookSize = cache.getSpineEntry(spineCount - 1).cumulativeSize;
  if (bookSize == 0) {
    entries.push_back(std::move(result));
    return false;
  }
  const size_t prevChapterSize = (spineIndex >= 1) ? cache.getSpineEntry(spineIndex - 1).cumulativeSize : 0;

  int percent = static_cast<int>((prevChapterSize * 100) / bookSize);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  result.progressPercent = static_cast<int8_t>(percent);
  entries.push_back(std::move(result));
  return true;
}

void HomeProgressCache::clear() { entries.clear(); }
