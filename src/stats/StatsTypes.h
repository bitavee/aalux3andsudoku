#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

static constexpr uint8_t STATS_MAX_BOOK_ENTRIES = 9;
static constexpr uint8_t STATS_SESSION_RING_SIZE = 7;
static constexpr uint16_t STATS_DAY_RING_SIZE = 365;
static constexpr uint8_t STATS_FILE_VERSION = 7;
static constexpr uint16_t STATS_DEFAULT_GOAL_MINUTES = 30;

/**
 * Achievement flags packed into GlobalStats.achievementBits (one bit each).
 * Tiered families (streak/books/pages/hours) plus two time-of-day badges are
 * derived from the persisted aggregates by stats::evaluateAchievements().
 * 16 of 32 bits are used; the remainder are reserved for future badges.
 */
enum AchievementBit : uint32_t {
  ACH_STREAK_3 = 1u << 0,     ///< Current/longest reading streak reached 3 days.
  ACH_STREAK_7 = 1u << 1,     ///< Reading streak reached 7 days.
  ACH_STREAK_30 = 1u << 2,    ///< Reading streak reached 30 days.
  ACH_STREAK_100 = 1u << 3,   ///< Reading streak reached 100 days.
  ACH_BOOKS_1 = 1u << 4,      ///< Finished the first book.
  ACH_BOOKS_10 = 1u << 5,     ///< Finished 10 books.
  ACH_BOOKS_25 = 1u << 6,     ///< Finished 25 books.
  ACH_BOOKS_50 = 1u << 7,     ///< Finished 50 books.
  ACH_PAGES_1K = 1u << 8,     ///< Turned 1,000 pages (lifetime).
  ACH_PAGES_10K = 1u << 9,    ///< Turned 10,000 pages (lifetime).
  ACH_PAGES_100K = 1u << 10,  ///< Turned 100,000 pages (lifetime).
  ACH_HOURS_10 = 1u << 11,    ///< Read for 10 hours (lifetime).
  ACH_HOURS_50 = 1u << 12,    ///< Read for 50 hours (lifetime).
  ACH_HOURS_100 = 1u << 13,   ///< Read for 100 hours (lifetime).
  ACH_EARLY_BIRD = 1u << 14,  ///< Ended a reading session between 04:00 and 08:00.
  ACH_NIGHT_OWL = 1u << 15,   ///< Ended a reading session between 22:00 and 03:00.
};

struct SpeedSample {
  uint16_t pagesPerHour;
  uint16_t minutesInSession;
};
static_assert(sizeof(SpeedSample) == 4, "SpeedSample layout changed -- bump STATS_FILE_VERSION");

struct BookStatEntry {
  char cacheKey[64];
  char title[64];
  char author[64];
  char bookPath[128];
  char thumbBmpPath[128];
  uint32_t totalReadingMs;
  uint32_t sessionCount;
  uint32_t totalPagesRead;
  uint32_t lastSessionMs;
  uint8_t progressPercent;
  uint8_t speedHead;
  uint8_t speedCount;
  uint8_t _pad0;
  SpeedSample speedSamples[4];
  uint16_t lastReadDay;
  uint16_t _pad1;
};
static_assert(sizeof(BookStatEntry) == 488, "BookStatEntry layout changed -- bump STATS_FILE_VERSION");
static_assert(offsetof(BookStatEntry, progressPercent) == 464, "v6 byte-prefix broken -- breaks migration");

struct GlobalStats {
  uint8_t version;
  uint8_t sessionRingHead;
  uint8_t sessionRingCount;
  uint8_t bookCount;
  uint16_t totalBooksFinished;
  uint8_t goalType;
  uint8_t petStage;
  uint32_t totalReadingMs;
  uint32_t totalSessionCount;
  uint32_t sessionRing[STATS_SESSION_RING_SIZE];
  uint16_t dayRingStartDay;
  uint16_t dayRingHead;
  uint16_t currentStreakDays;
  uint16_t longestStreakDays;
  uint16_t lastReadDay;
  uint16_t firstEverDay;
  uint16_t goalTarget;
  uint16_t goalAchievedDays;
  uint16_t bestDayMinutes;
  uint16_t lifetimeActiveDays;
  uint32_t achievementBits;
  uint32_t totalPagesLifetime;
  uint16_t petXp;
  uint8_t petHunger;
  uint8_t petHappiness;
  uint16_t dayMinutes[STATS_DAY_RING_SIZE];
  uint16_t lastSyncedDay;
};
static_assert(sizeof(GlobalStats) == 808, "GlobalStats layout changed -- bump STATS_FILE_VERSION");
static_assert(offsetof(GlobalStats, totalReadingMs) == 8, "v6 byte-prefix broken -- breaks migration");
static_assert(offsetof(GlobalStats, sessionRing) == 16, "v6 byte-prefix broken -- breaks migration");
static_assert(offsetof(GlobalStats, dayMinutes) == 76, "day-ring offset moved -- update migration");

namespace stats {

inline uint16_t saturatingAddU16(uint16_t a, uint16_t b) {
  const uint32_t s = static_cast<uint32_t>(a) + b;
  return s > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu) : static_cast<uint16_t>(s);
}

inline bool epochValid(int64_t epochSeconds) { return epochSeconds >= 1700000000; }

inline int utcOffsetSeconds(uint8_t clockUtcOffsetQ) { return (static_cast<int>(clockUtcOffsetQ) - 48) * 900; }

inline uint16_t dayNumber(int64_t epochSeconds, int offsetSeconds) {
  const int64_t local = epochSeconds + offsetSeconds;
  if (local < 0) return 0;
  return static_cast<uint16_t>(local / 86400);
}

struct CivilDate {
  int year;
  int month;
  int day;
};

inline CivilDate civilFromDays(int z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{y + static_cast<int>(m <= 2), static_cast<int>(m), static_cast<int>(d)};
}

inline int daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

inline int weekdayMon(int z) { return (z % 7 + 3) % 7; }

inline int daysInMonth(int y, int m) {
  static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return leap ? 29 : 28;
  }
  return dim[(m - 1) % 12];
}

inline uint16_t minutesOnDay(const GlobalStats& g, uint16_t day) {
  if (g.lastReadDay == 0 || day > g.lastReadDay) return 0;
  const uint16_t back = static_cast<uint16_t>(g.lastReadDay - day);
  if (back >= STATS_DAY_RING_SIZE) return 0;
  int idx = static_cast<int>(g.dayRingHead) - static_cast<int>(back);
  idx %= STATS_DAY_RING_SIZE;
  if (idx < 0) idx += STATS_DAY_RING_SIZE;
  return g.dayMinutes[idx];
}

inline bool streakAlive(const GlobalStats& g, uint16_t today) {
  if (g.lastReadDay == 0) return false;
  if (today <= g.lastReadDay) return true;
  return static_cast<uint16_t>(today - g.lastReadDay) <= 1;
}

inline uint32_t sumLastNDays(const GlobalStats& g, uint16_t today, uint16_t n) {
  uint32_t total = 0;
  for (uint16_t i = 0; i < n && i < STATS_DAY_RING_SIZE; ++i) {
    if (today < i) break;
    total += minutesOnDay(g, static_cast<uint16_t>(today - i));
  }
  return total;
}

inline uint32_t evaluateAchievements(GlobalStats& g, int sessionEndHour) {
  uint32_t bits = 0;
  if (g.longestStreakDays >= 3) bits |= ACH_STREAK_3;
  if (g.longestStreakDays >= 7) bits |= ACH_STREAK_7;
  if (g.longestStreakDays >= 30) bits |= ACH_STREAK_30;
  if (g.longestStreakDays >= 100) bits |= ACH_STREAK_100;
  if (g.totalBooksFinished >= 1) bits |= ACH_BOOKS_1;
  if (g.totalBooksFinished >= 10) bits |= ACH_BOOKS_10;
  if (g.totalBooksFinished >= 25) bits |= ACH_BOOKS_25;
  if (g.totalBooksFinished >= 50) bits |= ACH_BOOKS_50;
  if (g.totalPagesLifetime >= 1000) bits |= ACH_PAGES_1K;
  if (g.totalPagesLifetime >= 10000) bits |= ACH_PAGES_10K;
  if (g.totalPagesLifetime >= 100000) bits |= ACH_PAGES_100K;
  if (g.totalReadingMs >= 10u * 3600000u) bits |= ACH_HOURS_10;
  if (g.totalReadingMs >= 50u * 3600000u) bits |= ACH_HOURS_50;
  if (g.totalReadingMs >= 100u * 3600000u) bits |= ACH_HOURS_100;
  if (sessionEndHour >= 4 && sessionEndHour < 8) bits |= ACH_EARLY_BIRD;
  if (sessionEndHour >= 22 || (sessionEndHour >= 0 && sessionEndHour < 3)) bits |= ACH_NIGHT_OWL;
  const uint32_t newly = bits & ~g.achievementBits;
  g.achievementBits |= bits;
  return newly;
}

inline uint8_t petClampUp(uint8_t value, int delta) {
  const int r = static_cast<int>(value) + delta;
  return r > 100 ? 100 : static_cast<uint8_t>(r);
}

inline uint8_t petClampDown(uint8_t value, int delta) {
  const int r = static_cast<int>(value) - delta;
  return r < 0 ? 0 : static_cast<uint8_t>(r);
}

inline uint8_t petStageForXp(uint16_t xp) {
  if (xp < 50) return 0;
  if (xp < 150) return 1;
  if (xp < 350) return 2;
  if (xp < 700) return 3;
  if (xp < 1200) return 4;
  return 5;
}

inline void updatePet(GlobalStats& g, uint16_t today) {
  if (g.lastReadDay != 0 && today > g.lastReadDay) {
    const int missed = static_cast<int>(today - g.lastReadDay) - 1;
    if (missed > 0) {
      g.petHunger = petClampDown(g.petHunger, 8 * missed);
      g.petHappiness = petClampDown(g.petHappiness, 6 * missed);
    }
  }
  g.petHunger = petClampUp(g.petHunger, 20);
  g.petHappiness = petClampUp(g.petHappiness, 12);
  g.petXp = saturatingAddU16(g.petXp, 10);
  g.petStage = petStageForXp(g.petXp);
}

inline void maybeCountGoalCrossing(GlobalStats& g, uint16_t prevMinutes, uint16_t newMinutes) {
  if (g.goalTarget == 0) return;
  if (newMinutes >= g.goalTarget && prevMinutes < g.goalTarget && g.goalAchievedDays < 0xFFFF) {
    g.goalAchievedDays++;
  }
}

inline void recordReadingDay(GlobalStats& g, uint16_t day, uint16_t minutes) {
  if (minutes == 0) return;

  if (g.lastReadDay == 0) {
    g.dayRingHead = 0;
    g.dayMinutes[0] = minutes;
    g.lastReadDay = day;
    g.firstEverDay = day;
    g.dayRingStartDay = day;
    g.currentStreakDays = 1;
    g.longestStreakDays = 1;
    g.lifetimeActiveDays = 1;
    g.bestDayMinutes = minutes;
    maybeCountGoalCrossing(g, 0, minutes);
    return;
  }

  if (day == g.lastReadDay) {
    const uint16_t prev = g.dayMinutes[g.dayRingHead];
    const uint16_t now = saturatingAddU16(prev, minutes);
    g.dayMinutes[g.dayRingHead] = now;
    if (now > g.bestDayMinutes) g.bestDayMinutes = now;
    maybeCountGoalCrossing(g, prev, now);
    return;
  }

  if (day < g.lastReadDay) {
    const uint16_t back = static_cast<uint16_t>(g.lastReadDay - day);
    if (back < STATS_DAY_RING_SIZE) {
      int idx = static_cast<int>(g.dayRingHead) - static_cast<int>(back);
      idx %= STATS_DAY_RING_SIZE;
      if (idx < 0) idx += STATS_DAY_RING_SIZE;
      g.dayMinutes[idx] = saturatingAddU16(g.dayMinutes[idx], minutes);
    }
    return;
  }

  const uint16_t delta = static_cast<uint16_t>(day - g.lastReadDay);
  if (delta >= STATS_DAY_RING_SIZE) {
    memset(g.dayMinutes, 0, sizeof(g.dayMinutes));
    g.dayRingHead = 0;
    g.dayMinutes[0] = minutes;
  } else {
    for (uint16_t k = 0; k < delta; ++k) {
      g.dayRingHead = static_cast<uint16_t>((g.dayRingHead + 1) % STATS_DAY_RING_SIZE);
      g.dayMinutes[g.dayRingHead] = 0;
    }
    g.dayMinutes[g.dayRingHead] = minutes;
  }

  if (delta == 1) {
    if (g.currentStreakDays < 0xFFFF) g.currentStreakDays++;
  } else {
    g.currentStreakDays = 1;
  }
  if (g.currentStreakDays > g.longestStreakDays) g.longestStreakDays = g.currentStreakDays;

  g.lastReadDay = day;
  if (g.lifetimeActiveDays < 0xFFFF) g.lifetimeActiveDays++;
  if (minutes > g.bestDayMinutes) g.bestDayMinutes = minutes;
  if (day > STATS_DAY_RING_SIZE - 1) {
    const uint16_t oldest = static_cast<uint16_t>(day - (STATS_DAY_RING_SIZE - 1));
    g.dayRingStartDay = (oldest < g.firstEverDay) ? g.firstEverDay : oldest;
  } else {
    g.dayRingStartDay = g.firstEverDay;
  }
  maybeCountGoalCrossing(g, 0, minutes);
}

}  // namespace stats
