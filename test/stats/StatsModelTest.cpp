#include <gtest/gtest.h>

#include "stats/StatsTypes.h"

// stats.bin v7 binary layout is load-bearing: any change must bump the file
// version + migration. These guard the exact on-disk sizes.
TEST(StatsTypes, V7StructSizes) {
  EXPECT_EQ(sizeof(SpeedSample), 4u);
  EXPECT_EQ(sizeof(BookStatEntry), 488u);
  EXPECT_EQ(sizeof(GlobalStats), 808u);
}

TEST(StatsTypes, V7PrefixMatchesV6) {
  // The first 44 bytes of GlobalStats and 468 of BookStatEntry must stay
  // byte-identical to v6 so migration is a pure tail-append.
  EXPECT_EQ(offsetof(GlobalStats, totalReadingMs), 8u);
  EXPECT_EQ(offsetof(GlobalStats, sessionRing), 16u);
  EXPECT_EQ(offsetof(GlobalStats, dayMinutes), 76u);
  EXPECT_EQ(offsetof(BookStatEntry, progressPercent), 464u);
}

TEST(StatsStreak, FirstReadingStartsStreakAtOne) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 30);
  EXPECT_EQ(g.currentStreakDays, 1);
  EXPECT_EQ(g.longestStreakDays, 1);
  EXPECT_EQ(g.lastReadDay, 20000);
  EXPECT_EQ(g.firstEverDay, 20000);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 30);
}

TEST(StatsStreak, ConsecutiveDaysExtendStreak) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20001, 20);
  EXPECT_EQ(g.currentStreakDays, 2);
  EXPECT_EQ(g.longestStreakDays, 2);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 10);
  EXPECT_EQ(stats::minutesOnDay(g, 20001), 20);
}

TEST(StatsStreak, SameDayAccumulatesMinutesNoStreakChange) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20000, 25);
  EXPECT_EQ(g.currentStreakDays, 1);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 35);
}

TEST(StatsStreak, GapResetsCurrentButKeepsLongest) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20001, 10);
  stats::recordReadingDay(g, 20002, 10);  // streak now 3
  stats::recordReadingDay(g, 20005, 10);  // 3-day gap breaks it
  EXPECT_EQ(g.currentStreakDays, 1);
  EXPECT_EQ(g.longestStreakDays, 3);
  EXPECT_EQ(stats::minutesOnDay(g, 20003), 0);  // skipped days read as blank
  EXPECT_EQ(stats::minutesOnDay(g, 20004), 0);
  EXPECT_EQ(stats::minutesOnDay(g, 20005), 10);
}

TEST(StatsStreak, AliveOnlyWithinOneDay) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  EXPECT_TRUE(stats::streakAlive(g, 20000));
  EXPECT_TRUE(stats::streakAlive(g, 20001));
  EXPECT_FALSE(stats::streakAlive(g, 20002));
}

TEST(StatsDayRing, SaturatesMinutesAtU16Max) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 65000);
  stats::recordReadingDay(g, 20000, 2000);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 65535);
}

TEST(StatsDayRing, JumpBeyondWindowZeroesHistory) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 50);
  stats::recordReadingDay(g, 20400, 10);  // > 365-day jump
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 0);
  EXPECT_EQ(stats::minutesOnDay(g, 20400), 10);
  EXPECT_EQ(g.currentStreakDays, 1);
}

TEST(StatsDayRing, BackwardClockDoesNotZeroValidHistory) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20001, 40);
  stats::recordReadingDay(g, 20000, 5);  // clock corrected backward
  EXPECT_EQ(stats::minutesOnDay(g, 20001), 40);
  EXPECT_EQ(g.lastReadDay, 20001);  // anchor never moves backward
}

TEST(StatsAggregates, TracksBestDayAndActiveDays) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 30);
  stats::recordReadingDay(g, 20001, 90);
  stats::recordReadingDay(g, 20002, 15);
  EXPECT_EQ(g.bestDayMinutes, 90);
  EXPECT_EQ(g.lifetimeActiveDays, 3);
}

TEST(StatsGoal, CountsDistinctGoalMetDays) {
  GlobalStats g{};
  g.goalTarget = 30;
  stats::recordReadingDay(g, 20000, 30);  // meets exactly
  stats::recordReadingDay(g, 20001, 10);  // below
  stats::recordReadingDay(g, 20002, 45);  // meets
  EXPECT_EQ(g.goalAchievedDays, 2);
}

TEST(StatsGoal, SameDayCrossingCountsOnce) {
  GlobalStats g{};
  g.goalTarget = 30;
  stats::recordReadingDay(g, 20000, 20);  // below
  stats::recordReadingDay(g, 20000, 20);  // now 40, crosses the goal
  stats::recordReadingDay(g, 20000, 10);  // still above, must not double-count
  EXPECT_EQ(g.goalAchievedDays, 1);
}

TEST(StatsClock, RejectsUnsyncedEpoch) {
  EXPECT_FALSE(stats::epochValid(0));
  EXPECT_FALSE(stats::epochValid(1699999999));
  EXPECT_TRUE(stats::epochValid(1700000000));
}

TEST(StatsClock, OffsetSecondsFromQuarterHourSetting) {
  EXPECT_EQ(stats::utcOffsetSeconds(48), 0);      // UTC+0
  EXPECT_EQ(stats::utcOffsetSeconds(52), 3600);   // +4 quarter-hours = +1h
  EXPECT_EQ(stats::utcOffsetSeconds(44), -3600);  // -1h
}

TEST(StatsClock, DerivesLocalDayNumberFromEpoch) {
  // 2024-01-02 00:00:00 UTC = epoch 1704153600 = day 19724.
  EXPECT_EQ(stats::dayNumber(1704153600, 0), 19724);
  // 23:00 UTC on day 19723; a +2h offset rolls it into local day 19724.
  EXPECT_EQ(stats::dayNumber(1704153600 - 3600, 0), 19723);
  EXPECT_EQ(stats::dayNumber(1704153600 - 3600, 7200), 19724);
}

TEST(StatsPeriods, SumsLastNDaysInclusiveOfToday) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20001, 20);
  stats::recordReadingDay(g, 20002, 30);
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 1), 30u);
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 2), 50u);
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 7), 60u);
}

TEST(StatsPeriods, CountsUnreadDaysAsZeroWithinWindow) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);             // last read two days before "today"
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 1), 0u);   // nothing today
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 7), 10u);  // the read 2 days ago still counts
}

TEST(StatsAchievements, UnlocksFromLifetimeAggregates) {
  GlobalStats g{};
  g.totalReadingMs = 10u * 3600000u;  // 10 hours
  g.totalBooksFinished = 1;
  g.totalPagesLifetime = 1000;
  g.longestStreakDays = 7;
  const uint32_t newly = stats::evaluateAchievements(g, -1);
  EXPECT_TRUE(newly & ACH_HOURS_10);
  EXPECT_TRUE(newly & ACH_BOOKS_1);
  EXPECT_TRUE(newly & ACH_PAGES_1K);
  EXPECT_TRUE(newly & ACH_STREAK_7);
  EXPECT_FALSE(newly & ACH_HOURS_50);
  EXPECT_TRUE(g.achievementBits & ACH_HOURS_10);
}

TEST(StatsAchievements, IsIdempotent) {
  GlobalStats g{};
  g.longestStreakDays = 30;
  stats::evaluateAchievements(g, -1);
  EXPECT_EQ(stats::evaluateAchievements(g, -1), 0u);  // nothing new the second pass
  EXPECT_TRUE(g.achievementBits & ACH_STREAK_30);
  EXPECT_TRUE(g.achievementBits & ACH_STREAK_7);  // lower tiers also set
}

TEST(StatsAchievements, TimeOfDayBadgesNeedTheRightHour) {
  GlobalStats g{};
  EXPECT_FALSE(stats::evaluateAchievements(g, 12) & ACH_EARLY_BIRD);
  EXPECT_TRUE(stats::evaluateAchievements(g, 5) & ACH_EARLY_BIRD);
  EXPECT_TRUE(stats::evaluateAchievements(g, 23) & ACH_NIGHT_OWL);
  EXPECT_FALSE(stats::evaluateAchievements(g, -1) & ACH_NIGHT_OWL);  // unknown hour
}

TEST(StatsPet, ReadingFillsHungerAndHappiness) {
  GlobalStats g{};
  stats::updatePet(g, 20000);  // first feed
  EXPECT_EQ(g.petHunger, 20);
  EXPECT_EQ(g.petHappiness, 12);
}

TEST(StatsPet, ConsecutiveReadingKeepsFilling) {
  GlobalStats g{};
  g.lastReadDay = 20000;
  g.petHunger = 20;
  g.petHappiness = 12;
  stats::updatePet(g, 20001);  // next day, no gap
  EXPECT_EQ(g.petHunger, 40);
  EXPECT_EQ(g.petHappiness, 24);
}

TEST(StatsPet, MissedDaysDecayBeforeFeeding) {
  GlobalStats g{};
  g.lastReadDay = 20000;
  g.petHunger = 20;
  g.petHappiness = 12;
  stats::updatePet(g, 20003);  // 2 missed days: -16/-12 -> 4/0, then feed +20/+12 -> 24/12
  EXPECT_EQ(g.petHunger, 24);
  EXPECT_EQ(g.petHappiness, 12);
}

TEST(StatsPet, FeedingCapsAtHundred) {
  GlobalStats g{};
  g.lastReadDay = 20000;
  g.petHunger = 95;
  g.petHappiness = 95;
  stats::updatePet(g, 20000);  // same day, no decay, +20/+12 capped
  EXPECT_EQ(g.petHunger, 100);
  EXPECT_EQ(g.petHappiness, 100);
}

TEST(StatsPet, StageGrowsWithXp) {
  EXPECT_EQ(stats::petStageForXp(0), 0);  // kitten
  EXPECT_EQ(stats::petStageForXp(49), 0);
  EXPECT_EQ(stats::petStageForXp(50), 1);
  EXPECT_EQ(stats::petStageForXp(450), 3);  // adult cat
  EXPECT_EQ(stats::petStageForXp(1200), 5);
}

TEST(StatsPet, FeedingGrantsXpAndAdvancesStage) {
  GlobalStats g{};
  for (int i = 0; i < 5; ++i) stats::updatePet(g, 20000);  // 5 feeds * 10 XP
  EXPECT_EQ(g.petXp, 50);
  EXPECT_EQ(g.petStage, 1);
}

TEST(StatsCalendar, CivilFromDaysMatchesKnownDates) {
  const auto a = stats::civilFromDays(19724);  // 2024-01-02
  EXPECT_EQ(a.year, 2024);
  EXPECT_EQ(a.month, 1);
  EXPECT_EQ(a.day, 2);
  const auto b = stats::civilFromDays(0);  // 1970-01-01
  EXPECT_EQ(b.year, 1970);
  EXPECT_EQ(b.month, 1);
  EXPECT_EQ(b.day, 1);
}

TEST(StatsCalendar, DaysFromCivilRoundTrips) {
  EXPECT_EQ(stats::daysFromCivil(2024, 1, 2), 19724);
  EXPECT_EQ(stats::daysFromCivil(1970, 1, 1), 0);
}

TEST(StatsCalendar, WeekdayMonIsZeroBasedMonday) {
  EXPECT_EQ(stats::weekdayMon(19724), 1);  // 2024-01-02 was a Tuesday
}

TEST(StatsCalendar, DaysInMonthHandlesLeapYears) {
  EXPECT_EQ(stats::daysInMonth(2024, 2), 29);
  EXPECT_EQ(stats::daysInMonth(2023, 2), 28);
  EXPECT_EQ(stats::daysInMonth(2024, 4), 30);
  EXPECT_EQ(stats::daysInMonth(2024, 1), 31);
}
