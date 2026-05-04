#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace demo {

inline uint16_t parsePortArgument(const char* value) {
  const unsigned long parsed = std::stoul(value);
  if (parsed > 65535) {
    throw std::invalid_argument("port must be <= 65535");
  }

  return static_cast<uint16_t>(parsed);
}

inline std::string makeWebSocketUrl(const std::string& host, uint16_t port) {
  return "ws://" + host + ':' + std::to_string(port) + '/';
}

inline bool looksLikeWebSocketUrl(const std::string& value) {
  return value.rfind("ws://", 0) == 0 || value.rfind("wss://", 0) == 0;
}

inline std::chrono::milliseconds parseRetryIntervalArgument(const char* value) {
  const unsigned long parsed = std::stoul(value);
  if (parsed == 0) {
    throw std::invalid_argument("retry interval must be > 0 ms");
  }

  return std::chrono::milliseconds(parsed);
}

}  // namespace demo
