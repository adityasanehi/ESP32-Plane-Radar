#include "services/log_buffer.h"

#include <WiFi.h>

#include <cstdarg>

namespace services::log {

namespace {

constexpr size_t kLines = 50;
constexpr size_t kLineLen = 100;
char s_lines[kLines][kLineLen];
size_t s_next = 0;   // slot for the next line
size_t s_count = 0;  // lines stored (<= kLines)
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void logf(const char* fmt, ...) {
  char msg[kLineLen - 12];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.println(msg);

  char line[kLineLen];
  snprintf(line, sizeof(line), "%7lus  %s", millis() / 1000, msg);
  portENTER_CRITICAL(&s_mux);
  memcpy(s_lines[s_next], line, kLineLen);
  s_next = (s_next + 1) % kLines;
  if (s_count < kLines) ++s_count;
  portEXIT_CRITICAL(&s_mux);
}

String dump() {
  portENTER_CRITICAL(&s_mux);
  const size_t count = s_count;
  const size_t first = (s_next + kLines - count) % kLines;
  portEXIT_CRITICAL(&s_mux);

  String out;
  out.reserve(count * 64 + 128);
  out += "Plane Radar log  (uptime ";
  out += millis() / 1000;
  out += " s, free heap ";
  out += ESP.getFreeHeap();
  out += ", largest block ";
  out += ESP.getMaxAllocHeap();
  out += ", Wi-Fi ";
  out += WiFi.SSID();
  out += " RSSI ";
  out += WiFi.RSSI();
  out += " dBm)\n\n";
  // One line at a time under the lock, appended outside it (String
  // allocates). A line logged mid-dump may shift the view; harmless.
  char line[kLineLen];
  for (size_t i = 0; i < count; ++i) {
    portENTER_CRITICAL(&s_mux);
    memcpy(line, s_lines[(first + i) % kLines], kLineLen);
    portEXIT_CRITICAL(&s_mux);
    out += line;
    out += '\n';
  }
  return out;
}

}  // namespace services::log
