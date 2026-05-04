#include "TextConsumer.hpp"

#include "SignalingProtocol.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

namespace demo {

namespace {

rtc::Configuration makePeerConfiguration() {
  rtc::Configuration config;
  config.iceServers.emplace_back("stun:stun.l.google.com:19302");
  return config;
}

std::string formatMessage(const std::variant<rtc::binary, rtc::string>& message) {
  if (std::holds_alternative<rtc::string>(message)) {
    return std::get<rtc::string>(message);
  }

  return "<binary payload: " +
         std::to_string(std::get<rtc::binary>(message).size()) + " bytes>";
}

using Clock = std::chrono::system_clock;

std::string formatTimestamp(Clock::time_point time_point) {
  const std::time_t time_value = Clock::to_time_t(time_point);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          time_point.time_since_epoch()) %
      1000;

  std::tm local_time = *std::localtime(&time_value);
  std::ostringstream stream;
  stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << milliseconds.count();
  return stream.str();
}

std::string formatReceivedTextMessage(const std::string& payload) {
  constexpr std::string_view prefix = "__text_demo_sent_at_ms=";
  const std::size_t separator = payload.find(';');
  if (payload.compare(0, prefix.size(), prefix.data()) != 0 ||
      separator == std::string::npos) {
    return payload;
  }

  try {
    const auto sent_at_ms = std::stoll(
        payload.substr(prefix.size(), separator - prefix.size()));
    const auto sent_at = Clock::time_point(std::chrono::milliseconds(sent_at_ms));
    const Clock::time_point received_at = Clock::now();
    const std::string message = payload.substr(separator + 1);

    std::ostringstream stream;
    stream << "text consumer received at " << formatTimestamp(received_at)
           << " (producer sent at " << formatTimestamp(sent_at) << "): "
           << message;
    return stream.str();
  } catch (const std::exception&) {
    return payload;
  }
}

}  // namespace

TextConsumer::TextConsumer(WebSocketSignalTransportConfig signaling_config)
    : peer_connection_(makePeerConfiguration()),
      signaling_transport_(std::move(signaling_config)) {
  setupPeerConnection();
  setupSignalingTransport();
  signaling_transport_.start();
}

void TextConsumer::wait() const {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

uint16_t TextConsumer::port() const {
  return signaling_transport_.port();
}

bool TextConsumer::isSignalingServer() const {
  return signaling_transport_.isServer();
}

std::string TextConsumer::signalingEndpoint() const {
  return signaling_transport_.endpointDescription();
}

void TextConsumer::setupPeerConnection() {
  peer_connection_.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "text consumer peer state: " << state << '\n';
  });

  peer_connection_.onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "text consumer gathering state: " << state << '\n';
  });

  peer_connection_.onLocalDescription([this](const rtc::Description& description) {
    std::cout << "text consumer generated local description\n";
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate& candidate) {
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });

  peer_connection_.onDataChannel([this](const std::shared_ptr<rtc::DataChannel>& channel) {
    data_channel_ = channel;
    std::cout << "text consumer accepted data channel: " << channel->label() << '\n';

    data_channel_->onOpen([channel]() {
      std::cout << "text consumer data channel open (" << channel->label() << ")\n";
      channel->send("ack from text consumer");
    });

    data_channel_->onClosed([]() {
      std::cout << "text consumer data channel closed\n";
    });

    data_channel_->onMessage([](const auto& message) {
      if (std::holds_alternative<rtc::string>(message)) {
        std::cout << formatReceivedTextMessage(std::get<rtc::string>(message)) << '\n';
        return;
      }

      std::cout << "text consumer received on data channel: "
                << formatMessage(message) << '\n';
    });
  });
}

void TextConsumer::setupSignalingTransport() {
  signaling_transport_.setOnConnected([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "text consumer signaling websocket client connected\n";
    } else {
      std::cout << "text consumer websocket connected to "
                << signaling_transport_.endpointDescription() << '\n';
    }
  });

  signaling_transport_.setOnClosed([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "text consumer signaling websocket client disconnected\n";
    } else {
      std::cout << "text consumer websocket closed\n";
    }
  });

  signaling_transport_.setOnError([](const std::string& error) {
    std::cerr << "text consumer websocket error: " << error << '\n';
  });

  signaling_transport_.setOnMessage([this](const std::string& payload) {
    handleSignalingMessage(payload);
  });
}

void TextConsumer::handleSignalingMessage(const std::string& payload) {
  try {
    const SignalingMessage message = parseSignalingMessage(payload);
    switch (message.command) {
      case SignalingCommand::LocalDescription:
        handleRemoteDescription(
            parseRemoteDescription(message, peer_connection_.signalingState()));
        break;
      case SignalingCommand::LocalCandidate:
        handleRemoteCandidate(parseRemoteCandidate(message));
        break;
    }
  } catch (const std::exception& error) {
    std::cerr << "text consumer failed to handle websocket message: "
              << error.what() << '\n';
  }
}

void TextConsumer::handleRemoteDescription(const rtc::Description& description) {
  peer_connection_.setRemoteDescription(description);

  std::vector<rtc::Candidate> pending_candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending_candidates.swap(pending_candidates_);
  }

  for (auto& candidate : pending_candidates) {
    peer_connection_.addRemoteCandidate(std::move(candidate));
  }
}

void TextConsumer::handleRemoteCandidate(const rtc::Candidate& candidate) {
  if (candidate.candidate().empty()) {
    return;
  }

  bool should_queue = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!remote_description_set_) {
      pending_candidates_.push_back(candidate);
      should_queue = true;
    }
  }

  if (!should_queue) {
    peer_connection_.addRemoteCandidate(candidate);
  }
}

}  // namespace demo
