#include "TextProducer.hpp"

#include "rtc/rtc.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TextProducerOptions {
  uint16_t port = 8080;
  std::string bind_address = "0.0.0.0";
  bool interactive = false;
  std::vector<std::string> messages;
};

uint16_t parsePort(const char* value) {
  const unsigned long parsed = std::stoul(value);
  if (parsed > 65535) {
    throw std::invalid_argument("port must be <= 65535");
  }

  return static_cast<uint16_t>(parsed);
}

TextProducerOptions parseOptions(int argc, char** argv) {
  TextProducerOptions options;
  bool port_set = false;
  bool bind_address_set = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--interactive" || argument == "--stdin") {
      options.interactive = true;
      continue;
    }

    if (!port_set) {
      options.port = parsePort(argv[index]);
      port_set = true;
      continue;
    }

    if (!bind_address_set) {
      options.bind_address = argument;
      bind_address_set = true;
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

    demo::TextProducer producer(options.port, options.bind_address, options.messages);
    std::cout << "text producer websocket server listening on ws://"
              << options.bind_address << ':' << producer.port() << "/\n";

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
