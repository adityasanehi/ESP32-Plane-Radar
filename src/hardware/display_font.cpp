#include "hardware/display_font.h"

#include "hardware/display.h"

extern "C" {
extern const uint8_t _binary_data_ui_font_vlw_start[] asm(
    "_binary_data_ui_font_vlw_start");
extern const uint8_t _binary_data_ui_font_vlw_end[] asm("_binary_data_ui_font_vlw_end");
}

const lgfx::IFont* EmbeddedVlw::get() {
  if (!tried_) {
    tried_ = true;
    data_.set(start_, static_cast<uint32_t>(end_ - start_));
    ok_ = end_ > start_ && font_.loadFont(&data_);
  }
  return ok_ ? &font_ : nullptr;
}

namespace {

EmbeddedVlw s_ui_font(_binary_data_ui_font_vlw_start, _binary_data_ui_font_vlw_end);

}  // namespace

bool displayFontInit() {
  if (s_ui_font.get() == nullptr) {
    Serial.println("Smooth font load failed — using bitmap fallback");
    return false;
  }
  tft.setFont(s_ui_font.get());
  return true;
}

bool displayFontIsSmooth() { return s_ui_font.get() != nullptr; }

bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (s_ui_font.get() == nullptr) {
    return false;
  }
  gfx.setFont(s_ui_font.get());
  return true;
}

void displayFontSetSmoothSize(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
}

void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font) {
  gfx.setFont(font);
  gfx.setTextSize(1);
}
