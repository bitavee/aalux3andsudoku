#include "stats/ReadingStatsManager.h"

#include <HalStorage.h>

#include <cstring>

bool ReadingStatsManager::load() {
  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) {
    LOG_DBG("STATS", "No stats file found, starting fresh");
    global = GlobalStats{};
    global.version = STATS_FILE_VERSION;
    memset(books, 0, sizeof(books));
    return true;
  }

  uint8_t fileVersion;
  f.read(&fileVersion, 1);
  f.seek(0);

  if (fileVersion < STATS_FILE_VERSION) {
    LOG_INF("STATS", "Migrating stats %d -> %d", fileVersion, STATS_FILE_VERSION);

    // GlobalStats (v4=40, v5=44, v6=44). v5 and v6 match in size.
    size_t globalSize = (fileVersion == 4) ? 40 : 44;
    f.read(&global, globalSize);
    global.version = STATS_FILE_VERSION;

    for (uint8_t i = 0; i < global.bookCount; ++i) {
      memset(&books[i], 0, sizeof(BookStatEntry));
      if (fileVersion == 5) {
        // v5 entry was 464 bytes. lastSessionMs (v6) is a new 4-byte field.
        // Read everything up to totalPagesRead (460 bytes)
        f.read(&books[i], 460);
        // lastSessionMs is new in v6 at this offset, so we skip reading it from v5 file
        // and read the remaining v5 data (progressPercent + pads) into the new offset
        f.read(&books[i].progressPercent, 4);
      } else {
        // v4 migration (396 bytes)
        f.read(&books[i], 396);
      }
    }
    f.close();
    save();  // Force clean save in V6 format
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
  global.sessionRing[global.sessionRingHead] = elapsedMs;
  global.sessionRingHead = (global.sessionRingHead + 1) % STATS_SESSION_RING_SIZE;
  if (global.sessionRingCount < STATS_SESSION_RING_SIZE) {
    global.sessionRingCount++;
  }

  sortByProgress();
  save();
}

uint32_t ReadingStatsManager::getLast7SessionsMs() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < global.sessionRingCount; ++i) {
    total += global.sessionRing[i];
  }
  return total;
}