#include "Producer.hpp"

#include "rtc/rtc.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

uint16_t parsePort(const char* value) {
  const unsigned long parsed = std::stoul(value);
  if (parsed > 65535) {
    throw std::invalid_argument("port must be <= 65535");
  }

  return static_cast<uint16_t>(parsed);
}

demo::VideoPipeline::Profile parseVideoPipelineProfile(const char* value) {
  const std::string profile = value;
  if (profile == "default") {
    return demo::VideoPipeline::Profile::Default;
  }

  if (profile == "zed-appsink") {
    return demo::VideoPipeline::Profile::ZedAppsink;
  }

  if (profile == "zed-two-stream-appsink") {
    return demo::VideoPipeline::Profile::ZedTwoStreamAppsink;
  }

  throw std::invalid_argument(
      "unknown video pipeline profile '" + profile +
      "' (expected 'default', 'zed-appsink', or 'zed-two-stream-appsink')");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    rtc::InitLogger(rtc::LogLevel::Info);

    const uint16_t port = argc >= 2 ? parsePort(argv[1]) : 8080;
    const std::string bind_address = argc >= 3 ? argv[2] : "0.0.0.0";
    const demo::VideoPipeline::Profile video_pipeline_profile =
        argc >= 4 ? parseVideoPipelineProfile(argv[3])
                  : demo::VideoPipeline::Profile::Default;

    demo::Producer producer(port, bind_address, video_pipeline_profile);
    std::cout << "producer websocket server listening on ws://" << bind_address << ':'
              << producer.port() << "/\n";
    producer.wait();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "producer failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
