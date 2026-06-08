#pragma once

#include <cstdlib>

namespace demo {

// High-rate per-packet / per-message console logging in the transport classes
// is off by default so the bridges can print a clean rate table instead. Set
// INTERLINK_COMMS_VERBOSE=1 to restore the original chatty output (and raise the
// libdatachannel log level). Evaluated once and cached.
inline bool verboseLoggingEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("INTERLINK_COMMS_VERBOSE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

} // namespace demo
