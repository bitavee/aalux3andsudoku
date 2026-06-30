#!/usr/bin/env python3
"""Sim/dev helper (NOT used on device): writes sdcard/.crosspoint/stats.bin with
the reading pet placed at a target XP so the emulator's Pet tab renders that
evolution stage. The file is a valid 812-byte GlobalStats v8 with bookCount=0.

Usage:
  python3 scripts/sim_seed_pet.py <petXp>      # raw XP
  python3 scripts/sim_seed_pet.py stage <N>    # entry XP of stage N (0..10)
"""
import os
import struct
import sys

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "..", "sdcard", ".crosspoint", "stats.bin")

THRESHOLDS = [300, 700, 1300, 2200, 3500, 5500, 8500, 13000, 20000, 30000]
MAX_XP = THRESHOLDS[-1]
NAMES = [
    "Kitten", "Young Cat", "Cat", "Tiger Cub", "Young Tiger", "Tiger",
    "Dragon Egg", "Hatchling", "Young Dragon", "Dragon", "Elder Dragon",
]


def stage_for_xp(xp):
    s = 0
    for i, t in enumerate(THRESHOLDS):
        if xp >= t:
            s = i + 1
    return s


def level_for_xp(xp):
    return 100 if xp >= MAX_XP else 1 + (xp * 99) // MAX_XP


def entry_xp_for_stage(n):
    return 0 if n <= 0 else THRESHOLDS[min(n, len(THRESHOLDS)) - 1]


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "stage":
        xp = entry_xp_for_stage(int(args[1]))
    elif args:
        xp = int(args[0])
    else:
        xp = 0
    xp = max(0, min(xp, 65535))
    stage = stage_for_xp(xp)

    g = struct.pack(
        "<BBBBHBB"   # version, sessionRingHead, sessionRingCount, bookCount, totalBooksFinished, goalType, petStage
        "II"         # totalReadingMs, totalSessionCount
        "7I"         # sessionRing[7]
        "10H"        # dayRingStartDay..lifetimeActiveDays
        "II"         # achievementBits, totalPagesLifetime
        "H"          # petXp
        "BB"         # petHunger, petHappiness
        "365H"       # dayMinutes[365]
        "H"          # lastSyncedDay
        "I",         # petLastReadEpoch
        8, 0, 0, 0, 0, 0, stage,
        0, 0,
        0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0,
        xp,
        80, 80,
        *([0] * 365),
        0,
        0,
    )
    assert len(g) == 812, "GlobalStats packed to %d bytes, expected 812" % len(g)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(os.path.abspath(OUT), "wb") as f:
        f.write(g)
    print("wrote %s (%d bytes)" % (os.path.abspath(OUT), len(g)))
    print("  petXp=%d  stage=%d/%d  level=%d  name=%s" % (xp, stage, len(THRESHOLDS), level_for_xp(xp), NAMES[stage]))


main()
