#include "Producer.hpp"
#include "SignalingCli.hpp"

#include "rtc/rtc.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

struct ProducerOptions {
  demo::WebSocketSignalTransportConfig signaling =
      demo::WebSocketServerOptions{};
  demo::VideoPipeline::Profile video_pipeline_profile =
      demo::VideoPipeline::Profile::Default;
};

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

  if (profile == "zed-x-one-mono-appsink") {
    return demo::VideoPipeline::Profile::ZedXOneMonoAppsink;
  }

  throw std::invalid_argument(
      "unknown video pipeline profile '" + profile +
      "' (expected 'default', 'zed-appsink', 'zed-two-stream-appsink', or "
      "'zed-x-one-mono-appsink')");
}

ProducerOptions parseOptions(int argc, char** argv) {
  ProducerOptions options;

  if (argc <= 1) {
    return options;
  }

  const std::string mode = argv[1];
  if (mode == "--listen") {
    if (argc < 3 || argc > 5) {
      throw std::invalid_argument(
          "usage: producer --listen <port> [bind_address] [pipeline_profile]");
    }

    const uint16_t port = demo::parsePortArgument(argv[2]);
    const std::string bind_address = argc >= 4 ? argv[3] : "0.0.0.0";
    options.signaling = demo::WebSocketServerOptions{port, bind_address};
    if (argc >= 5) {
      options.video_pipeline_profile = parseVideoPipelineProfile(argv[4]);
    }
    return options;
  }

  if (mode == "--connect") {
    if (argc < 4 || argc > 7) {
      throw std::invalid_argument(
          "usage: producer --connect <host> <port> [--retry-ms <ms>] [pipeline_profile]");
    }

    demo::WebSocketClientOptions client_options{
        demo::makeWebSocketUrl(argv[2], demo::parsePortArgument(argv[3]))};
    for (int index = 4; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--retry-ms") {
        if (index + 1 >= argc) {
          throw std::invalid_argument("--retry-ms requires a value");
        }
        client_options.reconnect_interval =
            demo::parseRetryIntervalArgument(argv[index + 1]);
        ++index;
        continue;
      }

      options.video_pipeline_profile = parseVideoPipelineProfile(argv[index]);
    }

    options.signaling = client_options;
    return options;
  }

  if (argc > 4) {
    throw std::invalid_argument(
        "usage: producer [port] [bind_address] [pipeline_profile]");
  }

  const uint16_t port = demo::parsePortArgument(argv[1]);
  const std::string bind_address = argc >= 3 ? argv[2] : "0.0.0.0";
  options.signaling = demo::WebSocketServerOptions{port, bind_address};
  if (argc >= 4) {
    options.video_pipeline_profile = parseVideoPipelineProfile(argv[3]);
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    rtc::InitLogger(rtc::LogLevel::Info);

    const ProducerOptions options = parseOptions(argc, argv);
    demo::Producer producer(options.signaling, options.video_pipeline_profile);
    if (producer.isSignalingServer()) {
      std::cout << "producer websocket server listening on "
                << producer.signalingEndpoint() << '\n';
    } else {
      std::cout << "producer websocket client targeting "
                << producer.signalingEndpoint() << '\n';
    }
    producer.wait();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "producer failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
