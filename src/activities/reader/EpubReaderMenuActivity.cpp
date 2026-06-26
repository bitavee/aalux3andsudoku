#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const bool hasFootnotes,
                                               const bool hasDictionary, const bool hasLookupHistory)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasDictionary, hasLookupHistory)),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool hasDictionary,
                                                                                     bool hasLookupHistory) {
  std::vector<MenuItem> items;
  items.reserve(14);

  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER, "", true});
  items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS, "", true});
  items.push_back({MenuAction::ADD_BOOKMARK, StrId::STR_ADD_BOOKMARK, "", false});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES, "", true});
  }
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT, "", true});
  if (hasDictionary) {
    items.push_back({MenuAction::LOOKUP, (StrId)0, "Lookup", true});
  }
  if (hasLookupHistory) {
    items.push_back({MenuAction::LOOKED_UP_WORDS, (StrId)0, "Lookup History", true});
  }
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS, "", false});
  items.push_back({MenuAction::SYNC_CLOCK, StrId::STR_CLOCK_SYNC, "", false});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON, "", false});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR, "", false});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE, "", false});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON, "", false});
  items.push_back({MenuAction::READING_SETTINGS, StrId::STR_READING_SETTINGS, "", true});

  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(MenuResult{static_cast<int>(menuItems[selectedIndex].action)});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  char subtitle[24];
  if (totalPages > 0) {
    snprintf(subtitle, sizeof(subtitle), "%d/%d \xC2\xB7 %d%%", currentPage, totalPages, bookProgressPercent);
  } else {
    snprintf(subtitle, sizeof(subtitle), "%d%%", bookProgressPercent);
  }

  const ReaderUtils::BandGutter gutter = ReaderUtils::bandGutterForBottomHints(renderer, metrics.buttonHintsHeight);
  const int contentX = gutter.contentX();
  const int contentWidth = gutter.contentWidth(pageWidth);
  const int headerTop = gutter.contentY() + metrics.topPadding;

  GUI.drawHeader(renderer, Rect{contentX, headerTop, contentWidth, metrics.headerHeight}, title.c_str(), subtitle);

  const int listTop = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - gutter.bottom - listTop - metrics.verticalSpacing;

  const auto& items = menuItems;
  GUI.drawList(
      renderer, Rect{contentX, listTop, contentWidth, listHeight}, static_cast<int>(items.size()), selectedIndex,
      [&items](int index) {
        return items[index].labelId != (StrId)0 ? std::string(I18N.get(items[index].labelId))
                                                : items[index].customLabel;
      },
      nullptr, nullptr, nullptr, false, nullptr, nullptr, [&items](int index) { return items[index].opensScreen; });

  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation);

  renderer.displayBuffer();
}
