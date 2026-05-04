#include "Consumer.hpp"
#include "SignalingCli.hpp"

#include "rtc/rtc.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

demo::WebSocketSignalTransportConfig parseOptions(int argc, char** argv) {
  if (argc < 2) {
    throw std::invalid_argument(
        "usage: consumer (--listen <port> [bind_address] | --connect <host> <port> | <ws-url> | <host> <port>)");
  }

  const std::string mode = argv[1];
  if (mode == "--listen") {
    if (argc < 3 || argc > 4) {
      throw std::invalid_argument("usage: consumer --listen <port> [bind_address]");
    }

    return demo::WebSocketServerOptions{
        demo::parsePortArgument(argv[2]),
        argc >= 4 ? argv[3] : "0.0.0.0",
    };
  }

  if (mode == "--connect") {
    std::chrono::milliseconds retry_interval(1000);
    if (argc == 5 && std::string(argv[3]) == "--retry-ms") {
      retry_interval = demo::parseRetryIntervalArgument(argv[4]);
      return demo::WebSocketClientOptions{argv[2], retry_interval};
    }

    if (argc == 3 && demo::looksLikeWebSocketUrl(argv[2])) {
      return demo::WebSocketClientOptions{argv[2], retry_interval};
    }

    if (argc == 4) {
      return demo::WebSocketClientOptions{
          demo::makeWebSocketUrl(argv[2], demo::parsePortArgument(argv[3])),
          retry_interval};
    }

    if (argc == 6 && std::string(argv[4]) == "--retry-ms") {
      retry_interval = demo::parseRetryIntervalArgument(argv[5]);
      return demo::WebSocketClientOptions{
          demo::makeWebSocketUrl(argv[2], demo::parsePortArgument(argv[3])),
          retry_interval};
    }

    throw std::invalid_argument(
        "usage: consumer --connect <host> <port> [--retry-ms <ms>] or consumer --connect <ws-url> [--retry-ms <ms>]");
  }

  if (argc == 2 && demo::looksLikeWebSocketUrl(argv[1])) {
    return demo::WebSocketClientOptions{argv[1]};
  }

  if (argc == 3) {
    return demo::WebSocketClientOptions{
        demo::makeWebSocketUrl(argv[1], demo::parsePortArgument(argv[2]))};
  }

  throw std::invalid_argument(
      "usage: consumer (--listen <port> [bind_address] | --connect <host> <port> [--retry-ms <ms>] | --connect <ws-url> [--retry-ms <ms>] | <ws-url> | <host> <port>)");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    rtc::InitLogger(rtc::LogLevel::Info);

    demo::Consumer consumer(parseOptions(argc, argv));
    if (consumer.isSignalingServer()) {
      std::cout << "consumer websocket server listening on "
                << consumer.signalingEndpoint() << '\n';
    } else {
      std::cout << "consumer websocket client targeting "
                << consumer.signalingEndpoint() << '\n';
    }
    consumer.wait();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "consumer failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
