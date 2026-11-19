#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <algorithm>
#include <cstdlib>

#include "config.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsCardSecsKey[] = "cardSecs";
constexpr char kPrefsIconSizeKey[] = "iconSize";
constexpr char kPrefsCustomKmKey[] = "customKm";
constexpr float kIconScales[] = {1.0f, 1.3f, 1.6f};  // sizes 1-3
constexpr uint8_t kDefaultIconSize = 2;
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
unsigned s_card_secs = config::kFlightCardDefaultSec;
uint8_t s_icon_size = kDefaultIconSize;
// Custom range from the portal; ring3_km == 0 means "use the preset".
RangePreset s_custom = {0.0f, 0.0f};

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_card_secs = std::min<unsigned>(
      s_prefs.getUChar(kPrefsCardSecsKey, config::kFlightCardDefaultSec),
      config::kFlightCardMaxSec);
  s_icon_size = std::max<uint8_t>(
      1, std::min<uint8_t>(3, s_prefs.getUChar(kPrefsIconSizeKey, kDefaultIconSize)));
  const float custom_km = s_prefs.getFloat(kPrefsCustomKmKey, 0.0f);
  if (custom_km >= kMinCustomRangeKm && custom_km <= kMaxCustomRangeKm) {
    s_custom = {custom_km, custom_km * kRing3ToOuterKm};
  }
  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
  if (s_custom.ring3_km > 0.0f) {  // BOOT tap returns to the presets
    s_custom = {0.0f, 0.0f};
    if (s_prefs.begin(kPrefsNamespace, false)) {
      s_prefs.putFloat(kPrefsCustomKmKey, 0.0f);
      s_prefs.end();
    }
  }
}

void saveRangeKmFromPortal(const char* value) {
  char* end = nullptr;
  const float km = strtof(value, &end);
  if (end == value) return;  // empty field: leave the range alone
  if (km < kMinCustomRangeKm || km > kMaxCustomRangeKm) {
    Serial.printf("Range '%s' km out of range, keeping current\n", value);
    return;
  }
  if (fabsf(km - rangeCurrent().ring3_km) < 0.05f) return;  // unchanged
  s_custom = {km, km * kRing3ToOuterKm};
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putFloat(kPrefsCustomKmKey, km);
    s_prefs.end();
  }
  Serial.printf("Range: %.1f km on ring 3\n", km);
}

const RangePreset& rangeCurrent() {
  return s_custom.ring3_km > 0.0f ? s_custom : kRangePresets[s_range_index];
}

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

uint8_t iconSize() { return s_icon_size; }

float iconScale() { return kIconScales[s_icon_size - 1]; }

void saveIconSizeFromPortal(const char* value) {
  char* end = nullptr;
  const long size = strtol(value, &end, 10);
  if (end == value || size < 1 || size > 3) {
    Serial.printf("Icon size '%s' invalid, keeping %u\n", value, s_icon_size);
    return;
  }
  s_icon_size = static_cast<uint8_t>(size);
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putUChar(kPrefsIconSizeKey, s_icon_size);
    s_prefs.end();
  }
}

unsigned long cardShowMs() { return s_card_secs * 1000UL; }

unsigned cardShowSeconds() { return s_card_secs; }

void saveCardSecondsFromPortal(const char* value) {
  char* end = nullptr;
  const long secs = strtol(value, &end, 10);
  if (end == value || secs < 0 || secs > static_cast<long>(config::kFlightCardMaxSec)) {
    Serial.printf("Card duration '%s' invalid, keeping %u s\n", value, s_card_secs);
    return;
  }
  s_card_secs = static_cast<unsigned>(secs);
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putUChar(kPrefsCardSecsKey, static_cast<uint8_t>(s_card_secs));
    s_prefs.end();
  }
  Serial.printf("Info card: %u s\n", s_card_secs);
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = false;
  s_show_runways = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
