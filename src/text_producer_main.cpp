#include "TextProducer.hpp"
#include "SignalingCli.hpp"

#include "rtc/rtc.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TextProducerOptions {
  demo::WebSocketSignalTransportConfig signaling =
      demo::WebSocketServerOptions{};
  bool interactive = false;
  std::vector<std::string> messages;
};

TextProducerOptions parseOptions(int argc, char** argv) {
  TextProducerOptions options;
  int argument_index = 1;

  if (argument_index < argc) {
    const std::string mode = argv[argument_index];
    if (mode == "--connect") {
      if (argc - argument_index < 3) {
        throw std::invalid_argument(
            "usage: text_producer --connect <host> <port> [--retry-ms <ms>] [--interactive] [messages...]");
      }

      demo::WebSocketClientOptions client_options{
          demo::makeWebSocketUrl(
              argv[argument_index + 1],
              demo::parsePortArgument(argv[argument_index + 2]))};
      argument_index += 3;

      if (argument_index < argc && std::string(argv[argument_index]) == "--retry-ms") {
        if (argument_index + 1 >= argc) {
          throw std::invalid_argument("--retry-ms requires a value");
        }
        client_options.reconnect_interval =
            demo::parseRetryIntervalArgument(argv[argument_index + 1]);
        argument_index += 2;
      }

      options.signaling = client_options;
    } else if (mode == "--listen") {
      if (argc - argument_index < 2) {
        throw std::invalid_argument(
            "usage: text_producer --listen <port> [--bind <bind_address>] [--interactive] [messages...]");
      }

      demo::WebSocketServerOptions server_options;
      server_options.port = demo::parsePortArgument(argv[argument_index + 1]);
      argument_index += 2;

      if (argument_index < argc && std::string(argv[argument_index]) == "--bind") {
        if (argument_index + 1 >= argc) {
          throw std::invalid_argument("--bind requires a bind address");
        }
        server_options.bind_address = argv[argument_index + 1];
        argument_index += 2;
      }

      options.signaling = server_options;
    } else if (argc >= 2 && mode.rfind("--", 0) != 0) {
      demo::WebSocketServerOptions server_options;
      server_options.port = demo::parsePortArgument(argv[argument_index]);
      argument_index += 1;

      if (argument_index < argc && std::string(argv[argument_index]).rfind("--", 0) != 0) {
        server_options.bind_address = argv[argument_index];
        argument_index += 1;
      }

      options.signaling = server_options;
    }
  }

  for (int index = argument_index; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--interactive" || argument == "--stdin") {
      options.interactive = true;
      continue;
    }

    options.messages.push_back(argument);
  }

  return options;
}

void runInteractiveInputLoop(demo::TextProducer& producer) {
  std::string line;
  while (std::getline(std::cin, line)) {
    producer.enqueueMessage(line);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    rtc::InitLogger(rtc::LogLevel::Info);

    const TextProducerOptions options = parseOptions(argc, argv);

    demo::TextProducer producer(options.signaling, options.messages);
    if (producer.isSignalingServer()) {
      std::cout << "text producer websocket server listening on "
                << producer.signalingEndpoint() << '\n';
    } else {
      std::cout << "text producer websocket client targeting "
                << producer.signalingEndpoint() << '\n';
    }

    std::thread input_thread;
    if (options.interactive) {
      std::cout << "text producer interactive input enabled; type a line and press enter to send\n";
      input_thread = std::thread(runInteractiveInputLoop, std::ref(producer));
      input_thread.detach();
    }

    producer.wait();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "text producer failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
