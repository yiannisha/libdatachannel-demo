#include "TextConsumer.hpp"

#include "rtc/rtc.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
  try {
    rtc::InitLogger(rtc::LogLevel::Info);

    if (argc < 2) {
      std::cerr << "usage: text_consumer <ws://producer-host:port/>\n";
      return EXIT_FAILURE;
    }

    demo::TextConsumer consumer(argv[1]);
    consumer.wait();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "text consumer failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
