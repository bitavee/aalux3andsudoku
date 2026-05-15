#pragma once

#include <cstddef>
#include <cstdint>

// Bionic Reading: bolds the first ~half of each word to create fixation points.
//
// Implementation notes:
// - `enabled` is a global flag set by EpubReaderActivity before page->render. Threading the flag
//   through Page/PageElement/PageLine/TextBlock::render added too much code and pushed the
//   firmware past the OTA partition limit (see CLAUDE.md "Flash budget").
// - Uses byte-position approximation, then advances past UTF-8 continuation bytes so the split
//   never falls inside a codepoint. For ASCII this is exact; for multibyte UTF-8 it can be off
//   by one codepoint, which is visually fine and saves ~100+ bytes of flash vs. a true
//   codepoint counter.
namespace BionicReading {

inline bool enabled = false;

// Returns byte offset where the BOLD prefix ends (0 means skip the split).
inline size_t prefixByteLength(const char* word, const size_t len) {
  if (len < 2) return 0;
  size_t prefix = (len + 1) / 2;
  if (prefix > 4) prefix = 4;
  if (prefix >= len) prefix = len - 1;
  // Advance past any UTF-8 continuation bytes so we never split a codepoint.
  while (prefix < len && (static_cast<uint8_t>(word[prefix]) & 0xC0) == 0x80) {
    ++prefix;
  }
  if (prefix >= len) return 0;
  return prefix;
}

}  // namespace BionicReading
