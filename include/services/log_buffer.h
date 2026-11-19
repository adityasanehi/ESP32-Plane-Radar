#pragma once

#include <Arduino.h>

namespace services::log {

/** printf to Serial and to an in-RAM ring of recent lines (served at /log). */
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Recent lines, oldest first, with uptime stamps, as plain text. */
String dump();

}  // namespace services::log
