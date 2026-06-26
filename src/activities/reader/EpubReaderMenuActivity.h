#pragma once

#include <string>
#include <vector>

#include "I18n.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Navigation and action items offered by the in-reader menu. Live formatting
  // (orientation, auto page turn, fonts, spacing) is owned by Quick Settings and
  // is intentionally absent here; READING_SETTINGS opens that overlay.
  enum class MenuAction {
    // Open the chapter selection screen.
    SELECT_CHAPTER,
    // Toggle a bookmark at the current position.
    ADD_BOOKMARK,
    // Open the bookmarks list.
    BOOKMARKS,
    // Open the footnotes list for the current page.
    FOOTNOTES,
    // Open the go-to-percentage screen.
    GO_TO_PERCENT,
    // Capture a screenshot of the current page.
    SCREENSHOT,
    // Display the current page text as a QR code.
    DISPLAY_QR,
    // Return to the home screen.
    GO_HOME,
    // Sync reading progress with KOReader.
    SYNC,
    // Sync the device clock from NTP.
    SYNC_CLOCK,
    // Delete this book's render cache.
    DELETE_CACHE,
    // Look up a word in the offline dictionary.
    LOOKUP,
    // Open the dictionary lookup history.
    LOOKED_UP_WORDS,
    // Open the Quick Settings overlay (live formatting).
    READING_SETTINGS,
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
    std::string customLabel = "";  // Fallback for labels without StrId (like "Lookup")
    bool opensScreen = false;      // Drill-in rows render a leading tile + trailing chevron.
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const bool hasFootnotes, const bool hasDictionary, const bool hasLookupHistory);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<MenuItem> menuItems;
  std::string title;
  int currentPage;
  int totalPages;
  int bookProgressPercent;
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;

  std::vector<MenuItem> buildMenuItems(bool hasFootnotes, bool hasDictionary, bool hasLookupHistory);
};
