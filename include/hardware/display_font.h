#pragma once

#include <LovyanGFX.hpp>

/**
 * A VLW font embedded in flash (board_build.embed_files), loaded on first use.
 * Owning the VLWfont (rather than gfx.loadFont) lets several fonts coexist and
 * be switched per draw call with setFont().
 */
class EmbeddedVlw {
 public:
  EmbeddedVlw(const uint8_t* start, const uint8_t* end)
      : start_(start), end_(end) {}
  /** The loaded font, or nullptr if it failed to load. */
  const lgfx::IFont* get();

 private:
  const uint8_t* start_;
  const uint8_t* end_;
  lgfx::PointerWrapper data_;
  lgfx::VLWfont font_;
  bool tried_ = false;
  bool ok_ = false;
};

bool displayFontInit();
bool displayFontIsSmooth();

/** Select the embedded UI font on gfx if smooth fonts are enabled. */
bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx);

/** VLW: setTextSize scale (1.0 = font point size). Bitmap: no-op — use displayFontSetBitmap. */
void displayFontSetSmoothSize(lgfx::LGFXBase& gfx, float size);

/** Bitmap GFXfont fallback; clears any runtime VLW font on this instance. */
void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font);
