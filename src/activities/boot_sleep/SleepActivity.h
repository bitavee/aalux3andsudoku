#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderBlankSleepScreen() const;
  // Draws a white band along the bottom of the screen with the currently
  // opened book's title and read-percentage. Used by CUSTOM_INSIGHTS sleep
  // mode. No-op when no book is open.
  void drawBookInsightsOverlay() const;
};
