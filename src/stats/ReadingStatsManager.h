#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstring>

#include "Logging.h"
#include "stats/StatsTypes.h"

static constexpr uint32_t STATS_MIN_SESSION_MS = 3UL * 60UL * 1000UL;
static constexpr const char* STATS_FILE_PATH = "/.crosspoint/stats.bin";

// -----------------------------------------------------------------------
// ReadingStatsManager singleton
// -----------------------------------------------------------------------
class ReadingStatsManager {
 public:
  static ReadingStatsManager& getInstance() {
    static ReadingStatsManager instance;
    return instance;
  }

  bool load();

  void beginSession(const char* cacheKey, const char* title, const char* author, const char* bookPath,
                    const char* thumbBmpPath, uint8_t progressPercent);

  void endSession(uint8_t progressPercent, uint32_t sessionPagesTurned);

  const GlobalStats& getGlobal() const { return global; }
  uint8_t getBookCount() const { return global.bookCount; }
  const BookStatEntry& getBook(uint8_t index) const { return books[index]; }
  // Removes the entry at `index` from the in-memory list, shifts trailing
  // entries down, and persists the new state. Returns false if the index is
  // out of range. Lifetime counters (totalReadingMs, totalSessionCount,
  // totalBooksFinished) are intentionally left untouched -- removing a stats
  // row hides the book from the list, it does not retroactively erase the
  // time the user spent reading it.
  bool removeBook(uint8_t index);
  uint32_t getLast7SessionsMs() const;
  uint8_t getLast7SessionCount() const { return global.sessionRingCount; }

  void reset();
  void setDailyGoalMinutes(uint16_t minutes);

 private:
  ReadingStatsManager() = default;

  bool save();
  int findBook(const char* cacheKey) const;
  void bringBookToFront(uint8_t index);
  void sortByProgress();

  GlobalStats global{};
  BookStatEntry books[STATS_MAX_BOOK_ENTRIES]{};

  TickType_t sessionStartTick = 0;
  bool sessionActive = false;
  uint8_t sessionBookIndex = 0xFF;
};

#define StatsManager ReadingStatsManager::getInstance()
