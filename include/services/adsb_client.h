#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  /** Age of the position fix at fetch time (ms), from the feed's seen_pos.
   *  Added to the elapsed time when dead-reckoning so the drawn position
   *  reflects the estimated current location, not the already-stale fix. */
  uint32_t pos_age_ms;
  char callsign[9];
  char type[5];
  char alt[12];
  /** 24-bit ICAO address; 0 for non-ICAO (TIS-B "~" hex) targets. */
  uint32_t hex;
};

/** Info card for a newly seen aircraft, enriched from adsbdb.com. Empty
 *  strings mean the lookup had no data for that field. */
struct FlightCard {
  uint32_t hex;  // 0 = no card
  unsigned long shown_ms;
  char callsign[9];     // ADS-B callsign (ICAO, e.g. AAL173)
  char flight_iata[9];  // e.g. AA173
  char airline[32];
  char airline_icao[4];  // e.g. AAL (logo lookup)
  char airline_iata[3];  // e.g. AA (badge text when there's no logo)
  char orig[5];
  char dest[5];
  char orig_city[24];
  char dest_city[24];
  char model[40];  // e.g. Boeing 777-223ER
  char reg[12];
  bool has_coords;  // origin/destination lat/lon valid (route progress)
  float orig_lat, orig_lon, dest_lat, dest_lon;
  // Airline logo as two 34 px JPEGs, composited on black and on white (their
  // difference gives alpha). 0 = no logo. See withCardLogo().
  size_t logo_black_len;
  size_t logo_white_len;
};

constexpr size_t kMaxAircraft = 64;

/** Create the internal lock. Call once before any fetch/snapshot. */
void init();

/**
 * Copy the current aircraft into `out` (up to `max_out`) and report the fetch
 * timestamp, all under a lock — safe to call from a different thread than the
 * one running fetchUpdate(). Returns the number copied.
 */
size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms);

size_t aircraftCount();
const Aircraft* aircraftList();

/**
 * millis() timestamp of the last successful fetch (0 before the first). The
 * stored lat/lon are the values as of this time; callers can dead-reckon
 * newer positions from track_deg/gs_knots and the elapsed time.
 */
unsigned long lastUpdateMs();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Copy the active card into `out`; false when none is showing. */
bool currentCard(FlightCard* out);
/** Hide the active card early (BOOT tap). */
void dismissCard();
/** Run fn on the active card's logo JPEGs under the data lock, so the fetch
 *  task can't overwrite them mid-decode. Not called when there's no logo. */
using LogoFn = void (*)(const uint8_t* on_black, size_t black_len,
                        const uint8_t* on_white, size_t white_len);
void withCardLogo(LogoFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi.
 *  The first aircraft not seen recently also gets an adsbdb lookup and card. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/** Look up the newcomer fetchUpdate() picked (adsbdb + logo) and show its card.
 *  Separate call so the fetch's buffers are freed first. No-op if none. */
void processNewAircraft();

}  // namespace services::adsb
