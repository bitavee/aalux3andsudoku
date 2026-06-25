#pragma once
#include "../Activity.h"

class GfxRenderer;
struct RecentBook;

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Boot", renderer, mappedInput) {}
  void onEnter() override;

  // Draws the "Resuming…" card over the boot screen: cover (left), title /
  // author / series / progress bar / time-left (right), "Resuming…" centered
  // below. Caller is responsible for invoking this only when a resume is
  // actually about to happen (i.e. before goToReader). One full E-ink refresh.
  // POLISH-BOOT (docs/UX_REDESIGN.md §2.1.1).
  static void renderResumingCard(GfxRenderer& renderer, const RecentBook& book, int8_t progressPercent);
};
