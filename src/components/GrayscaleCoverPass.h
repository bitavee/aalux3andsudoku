#pragma once

#include <GfxRenderer.h>

// Composite the cover bitmaps as 4-level grayscale over an already-rendered BW
// base frame. The caller first paints the whole screen in BW (UI, chrome, and
// the covers as their darkest silhouette); this then re-runs coverFn twice to
// fill the LSB and MSB planes and pushes the composite via displayGrayBuffer.
// coverFn MUST draw only the cover bitmaps (drawBitmap / drawPerspectiveBitmap)
// -- chrome and text are BW-only and already live in the base frame. Falls back
// to a plain BW display when the second buffer cannot be allocated.
template <typename CoverFn>
inline void renderCoversGrayscale(GfxRenderer& renderer, CoverFn&& coverFn) {
  if (!renderer.storeBwBuffer()) {
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  coverFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  coverFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}
