#include "InMemorySignaler.hpp"
#include "Signaler.hpp"
#include "rtc/rtc.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <variant>

namespace {

struct DemoState {
  std::mutex mutex;
  std::condition_variable cv;
  int messages_received = 0;
};

std::string formatMessage(const std::variant<rtc::binary, rtc::string>& message) {
  if (std::holds_alternative<rtc::string>(message)) {
    return std::get<rtc::string>(message);
  }

  return "<binary payload: " +
         std::to_string(std::get<rtc::binary>(message).size()) + " bytes>";
}

void attachChannelHandlers(const std::string& peer_name,
                           const std::shared_ptr<rtc::DataChannel>& channel,
                           const std::string& greeting,
                           DemoState& state) {
  channel->onOpen([peer_name, greeting, channel]() {
    std::cout << peer_name << " channel open (" << channel->label() << ")\n";
    channel->send(greeting);
  });

  channel->onClosed([peer_name]() {
    std::cout << peer_name << " channel closed\n";
  });

  channel->onMessage([peer_name, &state](const auto& message) {
    std::cout << peer_name << " received: " << formatMessage(message) << '\n';

    {
      std::lock_guard<std::mutex> lock(state.mutex);
      ++state.messages_received;
    }

    state.cv.notify_all();
  });
}

}  // namespace

int main() {
  using namespace std::chrono_literals;
  using demo::InMemorySignaler;
  using demo::Signaler;

  // initialize libdatachannel logging
  rtc::InitLogger(rtc::LogLevel::Warning);

  rtc::Configuration config;

  // create two peer connections and an in-memory signaler to exchange messages between them
  auto offerer = std::make_shared<rtc::PeerConnection>(config);
  auto answerer = std::make_shared<rtc::PeerConnection>(config);

  InMemorySignaler to_offerer(offerer);
  InMemorySignaler to_answerer(answerer);
  DemoState state;

  // ----------

  // ---------- REGISTER CALLBACKS ----------

  offerer->onStateChange([](rtc::PeerConnection::State current) {
    std::cout << "offerer state: " << current << '\n';
  });
  answerer->onStateChange([](rtc::PeerConnection::State current) {
    std::cout << "answerer state: " << current << '\n';
  });

  offerer->onGatheringStateChange([](rtc::PeerConnection::GatheringState current) {
    std::cout << "offerer gathering: " << current << '\n';
  });
  answerer->onGatheringStateChange([](rtc::PeerConnection::GatheringState current) {
    std::cout << "answerer gathering: " << current << '\n';
  });

  offerer->onLocalDescription([&to_answerer](const rtc::Description& description) {
    std::cout << "offerer produced local description\n";
    to_answerer.deliverDescription(description);
  });
  answerer->onLocalDescription([&to_offerer](const rtc::Description& description) {
    std::cout << "answerer produced local description\n";
    to_offerer.deliverDescription(description);
  });

  offerer->onLocalCandidate([&to_answerer](const rtc::Candidate& candidate) {
    to_answerer.deliverCandidate(candidate);
  });
  answerer->onLocalCandidate([&to_offerer](const rtc::Candidate& candidate) {
    to_offerer.deliverCandidate(candidate);
  });

  // ----------

  // create a DataChannel on the answerer and attach channel handlers to it
  std::shared_ptr<rtc::DataChannel> answerer_channel;
  answerer->onDataChannel([&](const std::shared_ptr<rtc::DataChannel>& channel) {
    answerer_channel = channel;
    std::cout << "answerer accepted data channel: " << channel->label() << '\n';
    attachChannelHandlers("answerer", answerer_channel, "hello from answerer", state);
  });

  // create a DataChannel on the offerer and attach channel handlers to it
  auto offerer_channel = offerer->createDataChannel("demo");
  attachChannelHandlers("offerer", offerer_channel, "hello from offerer", state);

  // wait for both channels to receive their greeting message, or time out after 15 seconds
  std::unique_lock<std::mutex> lock(state.mutex);
  const bool completed = state.cv.wait_for(lock, 15s, [&state]() {
    return state.messages_received >= 2;
  });
  lock.unlock();

  if (!completed) {
    std::cerr << "Timed out waiting for both demo messages to arrive\n";
    if (offerer_channel) {
      offerer_channel->close();
    }
    if (answerer_channel) {
      answerer_channel->close();
    }
    offerer->close();
    answerer->close();
    return EXIT_FAILURE;
  }

  std::cout << "Demo completed successfully\n";

  if (offerer_channel) {
    offerer_channel->close();
  }
  if (answerer_channel) {
    answerer_channel->close();
  }
  offerer->close();
  answerer->close();
  return EXIT_SUCCESS;
}
