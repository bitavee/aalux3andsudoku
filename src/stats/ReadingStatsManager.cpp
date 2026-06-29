#include "stats/ReadingStatsManager.h"

#include <HalStorage.h>

#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"

bool ReadingStatsManager::load() {
  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) {
    LOG_DBG("STATS", "No stats file found, starting fresh");
    global = GlobalStats{};
    global.version = STATS_FILE_VERSION;
    global.goalTarget = STATS_DEFAULT_GOAL_MINUTES;
    memset(books, 0, sizeof(books));
    return true;
  }

  uint8_t fileVersion;
  f.read(&fileVersion, 1);
  f.seek(0);

  if (fileVersion < STATS_FILE_VERSION) {
    LOG_INF("STATS", "Migrating stats %d -> %d", fileVersion, STATS_FILE_VERSION);

    global = GlobalStats{};
    // Each older layout is a strict prefix of the current struct, so reading the
    // old byte count into a zero-initialised struct leaves the new tail at 0.
    // GlobalStats sizes: v4=40, v5/v6=44, v7=808 (v8 appends petLastReadEpoch).
    size_t globalSize;
    if (fileVersion == 4) {
      globalSize = 40;
    } else if (fileVersion == 7) {
      globalSize = 808;
    } else {
      globalSize = 44;  // v5, v6
    }
    f.read(&global, globalSize);
    global.version = STATS_FILE_VERSION;

    for (uint8_t i = 0; i < global.bookCount; ++i) {
      memset(&books[i], 0, sizeof(BookStatEntry));
      if (fileVersion == 7) {
        // v7 entry is already the current 488-byte layout; copy it whole.
        f.read(&books[i], 488);
      } else if (fileVersion == 5) {
        // v5 entry was 464 bytes. lastSessionMs (v6) is a new 4-byte field.
        // Read everything up to totalPagesRead (460 bytes)
        f.read(&books[i], 460);
        // lastSessionMs is new in v6 at this offset, so we skip reading it from v5 file
        // and read the remaining v5 data (progressPercent + pads) into the new offset
        f.read(&books[i].progressPercent, 4);
      } else if (fileVersion == 6) {
        // v6 entry was 468 bytes; v7 keeps that exact prefix and appends a zeroed tail.
        f.read(&books[i], 468);
      } else {
        // v4 migration (396 bytes)
        f.read(&books[i], 396);
      }
    }
    if (global.goalTarget == 0) global.goalTarget = STATS_DEFAULT_GOAL_MINUTES;
    stats::evaluateAchievements(global, -1);
    f.close();
    save();  // Force clean save in V7 format
    return true;
  }

  // Standard V6 load logic continues...
  f.read(&global, sizeof(GlobalStats));
  for (uint8_t i = 0; i < global.bookCount; ++i) {
    f.read(&books[i], sizeof(BookStatEntry));
  }
  f.close();
  return true;
}

bool ReadingStatsManager::save() {
  FsFile f;
  if (!Storage.openFileForWrite("STATS", STATS_FILE_PATH, f)) {
    LOG_ERR("STATS", "Could not open stats file for write");
    return false;
  }

  uint8_t rawGlobal[sizeof(GlobalStats)];
  memcpy(rawGlobal, &global, sizeof(GlobalStats));
  f.write(rawGlobal, sizeof(GlobalStats));

  for (uint8_t i = 0; i < global.bookCount; ++i) {
    uint8_t rawBook[sizeof(BookStatEntry)];
    memcpy(rawBook, &books[i], sizeof(BookStatEntry));
    f.write(rawBook, sizeof(BookStatEntry));
  }

  f.close();
  LOG_DBG("STATS", "Stats saved");
  return true;
}

int ReadingStatsManager::findBook(const char* cacheKey) const {
  for (uint8_t i = 0; i < global.bookCount; ++i) {
    if (strncmp(books[i].cacheKey, cacheKey, sizeof(books[i].cacheKey)) == 0) {
      return i;
    }
  }
  return -1;
}

void ReadingStatsManager::bringBookToFront(uint8_t index) {
  if (index == 0) return;
  BookStatEntry tmp;
  memcpy(&tmp, &books[index], sizeof(BookStatEntry));
  for (uint8_t i = index; i > 0; --i) {
    memcpy(&books[i], &books[i - 1], sizeof(BookStatEntry));
  }
  memcpy(&books[0], &tmp, sizeof(BookStatEntry));
}

void ReadingStatsManager::sortByProgress() {
  // Insertion sort descending by progressPercent — max 9 elements
  for (uint8_t i = 1; i < global.bookCount; ++i) {
    BookStatEntry tmp;
    memcpy(&tmp, &books[i], sizeof(BookStatEntry));
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && books[j].progressPercent < tmp.progressPercent) {
      memcpy(&books[j + 1], &books[j], sizeof(BookStatEntry));
      j--;
    }
    memcpy(&books[j + 1], &tmp, sizeof(BookStatEntry));
  }
}

void ReadingStatsManager::beginSession(const char* cacheKey, const char* title, const char* author,
                                       const char* bookPath, const char* thumbBmpPath, uint8_t progressPercent) {
  sessionStartTick = xTaskGetTickCount();
  sessionActive = true;

  int idx = findBook(cacheKey);
  if (idx == -1) {
    // New book: Add it and increment count, but don't force it to front yet
    // Sorting will happen during the first save in endSession()
    if (global.bookCount < STATS_MAX_BOOK_ENTRIES) {
      idx = global.bookCount;
      global.bookCount++;
    } else {
      // If full, reuse the last (least progress) slot
      idx = STATS_MAX_BOOK_ENTRIES - 1;
    }

    memset(&books[idx], 0, sizeof(BookStatEntry));
    strncpy(books[idx].cacheKey, cacheKey, sizeof(books[idx].cacheKey) - 1);
    strncpy(books[idx].title, title, sizeof(books[idx].title) - 1);
    strncpy(books[idx].author, author, sizeof(books[idx].author) - 1);
    // ... rest of the strncpy calls for books[idx] ...
    sessionBookIndex = idx;
  } else {
    // Existing book: Just update the index and metadata, NO bringBookToFront()
    sessionBookIndex = static_cast<uint8_t>(idx);
    strncpy(books[idx].bookPath, bookPath, sizeof(books[idx].bookPath) - 1);
    strncpy(books[idx].thumbBmpPath, thumbBmpPath, sizeof(books[idx].thumbBmpPath) - 1);
    // NOTE: progressPercent is intentionally NOT overwritten here. The value
    // passed in is byte-weighted at chapter-start (read from progress.bin),
    // which is less precise than the page-precise value endSession() saves on
    // exit. Overwriting on every entry would cause the Stats screen to flicker
    // back to a coarser percentage every time the user opens the book.
  }
}

void ReadingStatsManager::endSession(uint8_t progressPercent, uint32_t sessionPagesTurned) {
  if (!sessionActive) return;
  sessionActive = false;

  const uint32_t elapsedMs = (xTaskGetTickCount() - sessionStartTick) * portTICK_PERIOD_MS;
  // The "long enough" gate exists only to keep a 5-second "peek" from
  // overwriting the user-visible Last Session row. Global totals and per-book
  // running totals always count - otherwise All Time and Finished Books stay
  // at zero for users whose typical sessions are under three minutes.
  const bool longEnoughForLastSession = (elapsedMs >= STATS_MIN_SESSION_MS);

  if (sessionBookIndex < global.bookCount) {
    // Always reflect the user's true current position. The reader is the only
    // caller now (the deep-sleep safety-net endSession(0, 0) was removed in
    // main.cpp:enterDeepSleep) and it always passes a precise, page-accurate
    // value, so a smaller percentage means the user really did navigate
    // backward. The "Finished Books" lifetime counter only ever increments,
    // so regressing from 100% does not decrement it.
    if (progressPercent == 100 && books[sessionBookIndex].progressPercent < 100) {
      global.totalBooksFinished++;
    }
    books[sessionBookIndex].progressPercent = progressPercent;

    if (longEnoughForLastSession) {
      books[sessionBookIndex].lastSessionMs = elapsedMs;
    }

    books[sessionBookIndex].totalReadingMs += elapsedMs;
    books[sessionBookIndex].sessionCount++;
    books[sessionBookIndex].totalPagesRead += sessionPagesTurned;
  }

  global.totalReadingMs += elapsedMs;
  global.totalSessionCount++;
  global.totalPagesLifetime += sessionPagesTurned;
  global.sessionRing[global.sessionRingHead] = elapsedMs;
  global.sessionRingHead = (global.sessionRingHead + 1) % STATS_SESSION_RING_SIZE;
  if (global.sessionRingCount < STATS_SESSION_RING_SIZE) {
    global.sessionRingCount++;
  }

  // Per-day history, streak and goal only accrue when a real wall-clock day is
  // known. The X4 has no RTC, so time() is only valid after one NTP sync;
  // epochValid() gates on that. Reuse the long-session gate so a brief peek
  // does not register a reading day.
  const int64_t nowEpoch = static_cast<int64_t>(time(nullptr));
  if (longEnoughForLastSession && stats::epochValid(nowEpoch)) {
    const uint16_t today = stats::dayNumber(nowEpoch, stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ));
    const uint16_t minutes = static_cast<uint16_t>(elapsedMs / 60000UL);
    stats::updatePet(global, nowEpoch);
    stats::recordReadingDay(global, today, minutes);
    if (sessionBookIndex < global.bookCount) {
      BookStatEntry& b = books[sessionBookIndex];
      b.lastReadDay = today;
      if (sessionPagesTurned > 0 && elapsedMs > 0) {
        const uint32_t pph = static_cast<uint32_t>(sessionPagesTurned) * 3600000UL / elapsedMs;
        b.speedSamples[b.speedHead % 4].pagesPerHour = pph > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(pph);
        b.speedSamples[b.speedHead % 4].minutesInSession = minutes;
        b.speedHead = static_cast<uint8_t>((b.speedHead + 1) % 4);
        if (b.speedCount < 4) b.speedCount++;
      }
    }
  }

  int sessionEndHour = -1;
  if (stats::epochValid(nowEpoch)) {
    const int offset = stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ);
    const int64_t localEpoch = nowEpoch + offset;
    sessionEndHour = static_cast<int>((localEpoch % 86400) / 3600);
    global.lastSyncedDay = stats::dayNumber(nowEpoch, offset);
  }
  stats::evaluateAchievements(global, sessionEndHour);

  sortByProgress();
  save();
}

bool ReadingStatsManager::removeBook(uint8_t index) {
  if (index >= global.bookCount) return false;

  // If the active session is for this book, drop the binding so endSession
  // doesn't write back to a stale slot after the shift.
  if (sessionActive && sessionBookIndex == index) {
    sessionActive = false;
    sessionBookIndex = 0xFF;
  } else if (sessionActive && sessionBookIndex > index && sessionBookIndex < global.bookCount) {
    sessionBookIndex--;
  }

  for (uint8_t i = index; i + 1 < global.bookCount; ++i) {
    memcpy(&books[i], &books[i + 1], sizeof(BookStatEntry));
  }
  global.bookCount--;
  memset(&books[global.bookCount], 0, sizeof(BookStatEntry));

  return save();
}

uint32_t ReadingStatsManager::getLast7SessionsMs() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < global.sessionRingCount; ++i) {
    total += global.sessionRing[i];
  }
  return total;
}

void ReadingStatsManager::reset() {
  global = GlobalStats{};
  global.version = STATS_FILE_VERSION;
  global.goalTarget = STATS_DEFAULT_GOAL_MINUTES;
  memset(books, 0, sizeof(books));
  sessionActive = false;
  sessionBookIndex = 0xFF;
  save();
}

void ReadingStatsManager::setDailyGoalMinutes(uint16_t minutes) {
  global.goalTarget = minutes;
  save();
}