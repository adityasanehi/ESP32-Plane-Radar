#pragma once

#include <cstddef>
#include <cstdint>

namespace data {

/** Logo for a frequent airline, built into flash: the same 34 px JPEG pair
 *  (on black / on white) the device would otherwise download. */
struct BundledLogo {
  const char* icao;
  const uint8_t* on_black;
  size_t black_len;
  const uint8_t* on_white;
  size_t white_len;
};

// Top airlines at DEL by scheduled flight numbers (vradarserver standing-data,
// Nov 2026), filtered to carriers AeroCorner lists as operating there.
// Regenerate: scripts/build_airline_logos.py
extern const BundledLogo kBundledLogos[];
extern const size_t kBundledLogoCount;

}  // namespace data
