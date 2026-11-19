#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "services/http_body_framing.h"
#include "services/log_buffer.h"
#include "data/airline_logos.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectTimeoutMs = 5000;  // TLS handshake needs room
constexpr unsigned long kRequestTimeoutMs = 6000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;
SemaphoreHandle_t s_mutex = nullptr;

FlightCard s_card = {};  // guarded by s_mutex
// Logo JPEGs for s_card ([0] on black, [1] on white). ~1-1.7 KB each at 34 px.
// The device can't spare the ~43 KB contiguous a PNG decode needs, so the
// wsrv.nl image proxy resizes FlightAware's PNG and re-encodes it as JPEG.
constexpr size_t kLogoMaxBytes = 2560;

/** Where the active card's logo bytes live: flash (bundled) or a cache slot.
 *  Pointers, not a copy: the UI decodes them within one frame of publishing,
 *  and a slot is only reused for a later card, after this one has ended. */
struct LogoRef {
  const uint8_t* jpg[2];
  size_t len[2];
};
LogoRef s_card_logo = {};  // guarded by s_mutex

// Frequent DEL airlines are bundled in flash (data/airline_logos.h). Others go
// through this cache of recently seen airlines, so repeats skip the two TLS
// downloads -- each needs ~35 KB contiguous and occasionally can't get it.
// Only the fetch task touches it.
struct LogoCacheEntry {
  char icao[4];  // "" = empty slot
  bool missing;  // proxy said 404: airline has no logo
  uint16_t len[2];
  unsigned long used_ms;
  uint8_t jpg[2][kLogoMaxBytes];
};
LogoCacheEntry s_logo_cache[5] = {};

struct SeenEntry {
  uint32_t hex;
  unsigned long last_ms;
};
// ponytail: linear scan, 128 x 64 compares per fetch; fine at 5 s cadence.
SeenEntry s_seen[128] = {};
bool s_seen_seeded = false;
// Newcomer picked by fetchUpdate(), looked up later by processNewAircraft()
// once the fetch's JSON doc and TLS client are freed (TLS needs ~40 KB).
uint32_t s_pending_hex = 0;
char s_pending_callsign[9] = "";

/** Record hex as seen now; true if it wasn't seen within kFlightCardForgetMs.
 *  With insert=false, unknown planes aren't recorded, so they still count as
 *  new on a later fetch (used while a card is showing). */
bool markSeen(uint32_t hex, unsigned long now, bool insert = true) {
  size_t oldest = 0;
  for (size_t i = 0; i < sizeof(s_seen) / sizeof(s_seen[0]); ++i) {
    if (s_seen[i].hex == hex) {
      const bool is_new = now - s_seen[i].last_ms > config::kFlightCardForgetMs;
      s_seen[i].last_ms = now;
      return is_new;
    }
    if (s_seen[i].last_ms < s_seen[oldest].last_ms) {
      oldest = i;
    }
  }
  if (!insert) return false;
  s_seen[oldest] = {hex, now};
  return true;
}

void lock() {
  if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
}
void unlock() {
  if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
}

/** Publish parsed aircraft to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  for (size_t i = 0; i < count; ++i) {
    s_aircraft[i] = src[i];
  }
  s_aircraft_count = count;
  s_last_update_ms = millis();  // base time for dead-reckoning
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectTimeoutMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

/**
 * Pumps the socket one byte at a time, in blocks, without buffering the whole
 * response.
 *
 * Each refill runs the network poll callback, which is why HTTPClient's own
 * body readers (getString/writeToStream) can't be used here -- they block
 * without giving the fetch task a chance to poll. That also means they can't
 * de-chunk for us, so the wire image comes out raw and BodyFramer unwraps it.
 *
 * Where the body ends is BodyFramer's business, not this class's: it stops
 * pulling at the right byte. Reading a little past that point into buffer_ is
 * harmless while the connection is torn down after every fetch.
 */
class PollingSocketSource {
 public:
  PollingSocketSource(HTTPClient& http, WiFiClient& stream,
                      unsigned long deadline)
      : http_(&http), stream_(&stream), deadline_(deadline) {}

  /** Next raw byte, or -1 once the socket closes or the deadline passes. */
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    while (millis() < deadline_) {
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        const int to_read = available > static_cast<int>(sizeof(buffer_))
                                ? static_cast<int>(sizeof(buffer_))
                                : available;
        const int read_bytes = stream_->readBytes(buffer_, to_read);
        if (read_bytes > 0) {
          len_ = static_cast<size_t>(read_bytes);
          return true;
        }
      }
      if (!http_->connected() && stream_->available() <= 0) {
        break;  // server closed and the socket is drained
      }
      delay(1);
    }
    return false;
  }

  HTTPClient* http_;
  WiFiClient* stream_;
  unsigned long deadline_;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
};

using BodyReader = services::http::BodyFramer<PollingSocketSource>;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  // Non-ICAO targets come as "~abc123"; strtoul stops at '~' and yields 0.
  ac->hex = plane["hex"].is<const char*>()
                ? static_cast<uint32_t>(
                      strtoul(plane["hex"].as<const char*>(), nullptr, 16))
                : 0;
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

void copyStr(char* out, size_t out_len, JsonVariantConst v) {
  out[0] = '\0';
  if (v.is<const char*>()) {
    strncpy(out, v.as<const char*>(), out_len - 1);
    out[out_len - 1] = '\0';
  }
}

int getAdsbdbJsonOnce(const char* url, JsonDocument& doc);

/** GET a small adsbdb JSON body into doc, retrying once on connection failure.
 *  Returns the HTTP status (negative = connection error). */
int getAdsbdbJson(const char* url, JsonDocument& doc) {
  int code = getAdsbdbJsonOnce(url, doc);
  if (code < 0) {
    delay(300);
    doc.clear();
    code = getAdsbdbJsonOnce(url, doc);
  }
  return code;
}

int getAdsbdbJsonOnce(const char* url, JsonDocument& doc) {
  JsonDocument filter;
  filter["response"]["aircraft"]["type"] = true;
  filter["response"]["aircraft"]["manufacturer"] = true;
  filter["response"]["aircraft"]["registration"] = true;
  filter["response"]["aircraft"]["registered_owner"] = true;
  filter["response"]["aircraft"]["registered_owner_operator_flag_code"] = true;
  JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
  fr["callsign_iata"] = true;
  fr["airline"]["name"] = true;
  fr["airline"]["icao"] = true;
  fr["airline"]["iata"] = true;
  for (const char* end : {"origin", "destination"}) {
    fr[end]["iata_code"] = true;
    fr[end]["icao_code"] = true;
    fr[end]["municipality"] = true;
    fr[end]["latitude"] = true;
    fr[end]["longitude"] = true;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    // Body is ~1-2 KB; getString() handles chunked encoding for us.
    if (deserializeJson(doc, http.getString(),
                        DeserializationOption::Filter(filter))) {
      http.end();
      return -2;
    }
  }
  http.end();
  return code;
}

void copyAirport(JsonObjectConst ap, char* code, size_t code_len, char* city,
                 size_t city_len) {
  copyStr(code, code_len, ap["iata_code"]);
  if (code[0] == '\0') copyStr(code, code_len, ap["icao_code"]);
  copyStr(city, city_len, ap["municipality"]);
}

/** Download a small file into buf. Returns its length, 0 on any failure. */
size_t httpGetSmallOnce(const char* url, uint8_t* buf, size_t max_len,
                        int* out_code) {
  *out_code = HTTPC_ERROR_CONNECTION_REFUSED;  // if begin() fails
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return 0;
  }
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  size_t got = 0;
  const int code = http.GET();
  *out_code = code;
  const int len = http.getSize();
  if (code == HTTP_CODE_OK && (len <= 0 || static_cast<size_t>(len) > max_len)) {
    services::log::logf("http: body size %d unusable (max %u)", len,
                        static_cast<unsigned>(max_len));
  }
  if (code == HTTP_CODE_OK && len > 0 && static_cast<size_t>(len) <= max_len) {
    WiFiClient* stream = http.getStreamPtr();
    stream->setTimeout(kRequestTimeoutMs / 1000);
    got = stream->readBytes(buf, len);
    if (got != static_cast<size_t>(len)) got = 0;
  }
  http.end();
  return got;
}

/** As above, retrying once when the connection itself failed (code < 0): with
 *  no PSRAM, a TLS handshake occasionally can't get its ~35 KB buffers. */
size_t httpGetSmall(const char* url, uint8_t* buf, size_t max_len,
                    int* out_code) {
  int code = 0;
  size_t got = httpGetSmallOnce(url, buf, max_len, &code);
  if (got == 0 && code < 0) {
    services::log::logf("http: %s (largest block %u), retrying",
                        HTTPClient::errorToString(code).c_str(),
                        static_cast<unsigned>(ESP.getMaxAllocHeap()));
    delay(300);
    got = httpGetSmallOnce(url, buf, max_len, &code);
  }
  *out_code = code;
  return got;
}

/** Logo JPEG pair for an airline: bundled, cached, or downloaded into the
 *  cache. False when there's none (404) or on a transient failure (not cached,
 *  so the next plane retries). */
bool logoFor(const char* icao, LogoRef* out) {
  for (size_t i = 0; i < data::kBundledLogoCount; ++i) {
    const data::BundledLogo& b = data::kBundledLogos[i];
    if (strcmp(b.icao, icao) == 0) {
      *out = {{b.on_black, b.on_white}, {b.black_len, b.white_len}};
      services::log::logf("logo: %s bundled", icao);
      return true;
    }
  }

  LogoCacheEntry* victim = &s_logo_cache[0];
  for (LogoCacheEntry& e : s_logo_cache) {
    if (strcmp(e.icao, icao) == 0) {
      e.used_ms = millis();
      services::log::logf("logo: %s from cache%s", icao,
                          e.missing ? " (airline has none)" : "");
      *out = {{e.jpg[0], e.jpg[1]}, {e.len[0], e.len[1]}};
      return !e.missing;
    }
    if (e.used_ms < victim->used_ms) victim = &e;
  }

  LogoCacheEntry& e = *victim;
  e.icao[0] = '\0';  // slot invalid until the downloads succeed
  e.missing = false;
  const char* bgs[] = {"black", "white"};
  for (int i = 0; i < 2; ++i) {
    char url[200];
    snprintf(url, sizeof(url),
             "https://wsrv.nl/?url=cdn.jsdelivr.net/gh/Jxck-S/"
             "airline-logos@main/flightaware_logos/%s.png"
             "&w=34&h=34&fit=contain&bg=%s&output=jpg&q=92",
             icao, bgs[i]);
    int code = 0;
    e.len[i] = httpGetSmall(url, e.jpg[i], kLogoMaxBytes, &code);
    if (e.len[i] == 0) {
      if (code == HTTP_CODE_NOT_FOUND) {
        e.missing = true;  // cache the miss too: no point asking again
        break;
      }
      services::log::logf("logo: %s download failed: %d %s", icao, code,
                          code < 0 ? HTTPClient::errorToString(code).c_str()
                                   : "");
      return false;
    }
  }
  memcpy(e.icao, icao, sizeof(e.icao));
  e.used_ms = millis();
  services::log::logf("logo: %s %s", icao,
                      e.missing ? "not available (404)" : "downloaded");
  *out = {{e.jpg[0], e.jpg[1]}, {e.len[0], e.len[1]}};
  return !e.missing;
}

/** "Boeing" + "777 223ER" -> "Boeing 777-223ER" (dash only before a digit,
 *  so names like "Citation Excel" keep their space). */
void formatModel(const char* maker, const char* type, char* out, size_t len) {
  snprintf(out, len, "%s%s%s", maker, maker[0] ? " " : "", type);
  // maker is network data and may be longer than out: clamp the search start
  // to what snprintf actually wrote.
  const size_t written = strlen(out);
  const size_t start =
      std::min(written, strlen(maker) + (maker[0] ? 1 : 0));
  char* sp = strchr(out + start, ' ');
  if (sp != nullptr && sp[1] >= '0' && sp[1] <= '9') *sp = '-';
}

/** Airline ICAO from a callsign like "AAL173" (three letters, then a digit). */
void icaoFromCallsign(const char* cs, char* out) {
  out[0] = '\0';
  for (int i = 0; i < 3; ++i) {
    if (cs[i] < 'A' || cs[i] > 'Z') return;
  }
  if (cs[3] < '0' || cs[3] > '9') return;
  memcpy(out, cs, 3);
  out[3] = '\0';
}

/**
 * Build a card for hex. adsbdb's /aircraft/{hex}?callsign= returns airframe and
 * route together, but 404s when the airframe is unknown even if the route is
 * known -- so fall back to /callsign/{cs} for the route alone.
 */
void lookupCard(uint32_t hex, const char* callsign, FlightCard* card) {
  *card = {};
  card->hex = hex;
  strncpy(card->callsign, callsign, sizeof(card->callsign) - 1);

  char url[96];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%06X%s%s",
           static_cast<unsigned>(hex), callsign[0] ? "?callsign=" : "",
           callsign);
  JsonDocument doc;
  int code = getAdsbdbJson(url, doc);
  if (code == 404 && callsign[0] != '\0') {
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s",
             callsign);
    doc.clear();
    code = getAdsbdbJson(url, doc);
  }
  services::log::logf("adsbdb: %06X %s -> %d%s%s", static_cast<unsigned>(hex),
                      callsign, code, code < 0 ? " " : "",
                      code < 0 ? HTTPClient::errorToString(code).c_str() : "");
  if (code != HTTP_CODE_OK) {
    icaoFromCallsign(callsign, card->airline_icao);
    return;  // card still shows the live ADS-B fields
  }

  JsonObjectConst ac = doc["response"]["aircraft"];
  if (!ac.isNull()) {
    formatModel(ac["manufacturer"] | "", ac["type"] | "", card->model,
                sizeof(card->model));
    copyStr(card->reg, sizeof(card->reg), ac["registration"]);
    copyStr(card->airline, sizeof(card->airline), ac["registered_owner"]);
    copyStr(card->airline_icao, sizeof(card->airline_icao),
            ac["registered_owner_operator_flag_code"]);
  }
  JsonObjectConst fr = doc["response"]["flightroute"];
  if (!fr.isNull()) {
    copyStr(card->flight_iata, sizeof(card->flight_iata), fr["callsign_iata"]);
    if (fr["airline"]["name"].is<const char*>()) {
      copyStr(card->airline, sizeof(card->airline), fr["airline"]["name"]);
      copyStr(card->airline_icao, sizeof(card->airline_icao),
              fr["airline"]["icao"]);
      copyStr(card->airline_iata, sizeof(card->airline_iata),
              fr["airline"]["iata"]);
    }
    card->has_coords = fr["origin"]["latitude"].is<float>() &&
                       fr["destination"]["latitude"].is<float>();
    card->orig_lat = fr["origin"]["latitude"] | 0.0f;
    card->orig_lon = fr["origin"]["longitude"] | 0.0f;
    card->dest_lat = fr["destination"]["latitude"] | 0.0f;
    card->dest_lon = fr["destination"]["longitude"] | 0.0f;
    copyAirport(fr["origin"], card->orig, sizeof(card->orig), card->orig_city,
                sizeof(card->orig_city));
    copyAirport(fr["destination"], card->dest, sizeof(card->dest),
                card->dest_city, sizeof(card->dest_city));
  }
}

}  // namespace

bool currentCard(FlightCard* out) {
  lock();
  *out = s_card;
  unlock();
  return out->hex != 0 &&
         millis() - out->shown_ms < ui::radar::cardShowMs();
}

void withCardLogo(LogoFn fn) {
  lock();
  if (s_card.logo_black_len > 0 && s_card.logo_white_len > 0) {
    fn(s_card_logo.jpg[0], s_card_logo.len[0], s_card_logo.jpg[1],
       s_card_logo.len[1]);
  }
  unlock();
}

void dismissCard() {
  lock();
  s_card.hex = 0;
  unlock();
}

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  // Keep only the fields we render; the rest never reaches RAM.
  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "dir", "gs",
        "tas", "ias", "alt_baro", "alt_geom", "seen_pos", "flight", "hex", "t",
        "category"}) {
    f[key] = true;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  // HTTPClient only records Transfer-Encoding in the collected headers when
  // it is asked for up front; _transferEncoding itself is private.
  static const char* kWantedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kWantedHeaders, 1);

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    services::log::logf("adsb: HTTP %d %s", code,
                        code < 0 ? HTTPClient::errorToString(code).c_str() : "");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no response stream");
    http.end();
    return false;
  }

  // On HTTP/1.1 the CDN answers with Transfer-Encoding: chunked, and
  // getStreamPtr() hands back the raw socket -- chunk sizes and all. BodyFramer
  // strips that framing back off.
  const services::http::BodyFraming framing =
      http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
          ? services::http::BodyFraming::kChunked
          : services::http::BodyFraming::kIdentity;

  PollingSocketSource source(http, *stream, millis() + kRequestTimeoutMs);
  BodyReader body(source, framing, http.getSize());
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  // Read off the terminating chunk the parser stopped short of, so the socket
  // sits at the end of the message. Not needed while every fetch builds its own
  // connection, but a prerequisite for ever reusing one.
  body.drain();
  http.end();
  if (err) {
    if (body.framingError()) {
      Serial.println("adsb: malformed chunked body");
    } else if (body.bytesRead() == 0) {
      Serial.println("adsb: empty response");
    } else {
      services::log::logf("adsb: JSON parse error: %s", err.c_str());
    }
    return false;
  }

  // Parse into a local buffer, then publish atomically so a reader on another
  // thread never sees a half-updated list.
  Aircraft parsed[kMaxAircraft];
  size_t n = 0;
  // Closest aircraft not seen recently gets the info card.
  const unsigned long now = millis();
  const float cos_lat = cosf(static_cast<float>(center_lat) * 0.01745329f);
  float best_d2 = 1e9f;
  uint32_t best_hex = 0;
  char best_callsign[9] = "";
  // While a card is up, newcomers wait (unrecorded) so they get their own
  // card afterwards instead of cutting this one short.
  FlightCard showing;
  const bool card_busy = currentCard(&showing);
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (!ac.isNull()) {
    for (JsonObject plane : ac) {
      if (n >= kMaxAircraft) {
        break;
      }
      if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
        continue;
      }
      if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
        continue;
      }

      parsed[n].lat = plane["lat"].as<float>();
      parsed[n].lon = plane["lon"].as<float>();
      parsed[n].nose_deg = pickNoseHeading(plane);
      parsed[n].track_deg = pickTrackHeading(plane);
      parsed[n].gs_knots = pickGroundSpeed(plane);

      // seen_pos: seconds since this position was measured. Use it as the
      // dead-reckoning age offset, capped so a very stale fix isn't flung far.
      float seen_pos = 0.0f;
      readJsonFloat(plane, "seen_pos", &seen_pos);
      if (seen_pos < 0.0f) seen_pos = 0.0f;
      if (seen_pos > 30.0f) seen_pos = 30.0f;
      parsed[n].pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);

      fillTagFields(&parsed[n], plane);
      if (parsed[n].hex != 0 &&
          markSeen(parsed[n].hex, now, !card_busy || !s_seen_seeded) &&
          s_seen_seeded && !card_busy) {
        const float dy = parsed[n].lat - static_cast<float>(center_lat);
        const float dx =
            (parsed[n].lon - static_cast<float>(center_lon)) * cos_lat;
        if (dx * dx + dy * dy < best_d2) {
          best_d2 = dx * dx + dy * dy;
          best_hex = parsed[n].hex;
          // Empty when the feed has no callsign (tag shows hex instead).
          copyJsonStringTrimmed(plane, "flight", best_callsign,
                                sizeof(best_callsign));
        }
      }
      ++n;
    }
  }

  publish(parsed, n);
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));

  // First fetch after boot only seeds the seen list: no card for planes that
  // were already overhead.
  s_seen_seeded = true;
  s_pending_hex = ui::radar::cardShowMs() > 0 ? best_hex : 0;
  memcpy(s_pending_callsign, best_callsign, sizeof(s_pending_callsign));
  return true;
}

void processNewAircraft() {
  const uint32_t best_hex = s_pending_hex;
  const char* best_callsign = s_pending_callsign;
  s_pending_hex = 0;
  if (best_hex != 0) {
    services::log::logf("new: %06X %s  (heap %u, largest %u)",
                        static_cast<unsigned>(best_hex), best_callsign,
                        static_cast<unsigned>(ESP.getFreeHeap()),
                        static_cast<unsigned>(ESP.getMaxAllocHeap()));
    FlightCard card;
    lookupCard(best_hex, best_callsign, &card);
    if (card.airline_icao[0] == '\0') {
      icaoFromCallsign(best_callsign, card.airline_icao);
    }
    LogoRef logo = {};
    if (card.airline_icao[0] != '\0' && logoFor(card.airline_icao, &logo)) {
      card.logo_black_len = logo.len[0];
      card.logo_white_len = logo.len[1];
    }
    card.shown_ms = millis();
    lock();
    s_card = card;
    s_card_logo = logo;
    unlock();
  }
}

}  // namespace services::adsb
