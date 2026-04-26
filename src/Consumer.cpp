#include "Consumer.hpp"

#include "SignalingProtocol.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
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

}  // namespace

Consumer::Consumer(std::string websocket_url)
    : websocket_url_(std::move(websocket_url)),
      peer_connection_(makePeerConfiguration()) {
  setupPeerConnection();
  setupWebSocket();
}

void Consumer::wait() const {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void Consumer::setupPeerConnection() {
  peer_connection_.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "consumer peer state: " << state << '\n';
  });

  peer_connection_.onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "consumer gathering state: " << state << '\n';
  });

  peer_connection_.onLocalDescription([this](const rtc::Description& description) {
    std::cout << "consumer generated local description\n";
    queueOrSendSignalingMessage(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate& candidate) {
    queueOrSendSignalingMessage(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });

  peer_connection_.onDataChannel([this](const std::shared_ptr<rtc::DataChannel>& channel) {
    data_channel_ = channel;
    std::cout << "consumer accepted data channel: " << channel->label() << '\n';

    data_channel_->onOpen([channel]() {
      std::cout << "consumer data channel open (" << channel->label() << ")\n";
    });

    data_channel_->onClosed([]() {
      std::cout << "consumer data channel closed\n";
    });

    data_channel_->onMessage([](const auto& message) {
      std::cout << "consumer received on data channel: " << formatMessage(message) << '\n';
    });
  });
}

void Consumer::setupWebSocket() {
  websocket_.onOpen([this]() {
    std::cout << "consumer websocket connected to " << websocket_url_ << '\n';
    flushPendingSignalingMessages();
  });

  websocket_.onClosed([]() {
    std::cout << "consumer websocket closed\n";
  });

  websocket_.onError([](const std::string& error) {
    std::cerr << "consumer websocket error: " << error << '\n';
  });

  websocket_.onMessage([this](const auto& message) {
    if (!std::holds_alternative<rtc::string>(message)) {
      std::cerr << "consumer received an unexpected binary websocket message\n";
      return;
    }

    handleWebSocketMessage(std::get<rtc::string>(message));
  });

  websocket_.open(websocket_url_);
}

void Consumer::handleWebSocketMessage(const std::string& payload) {
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
    std::cerr << "consumer failed to handle websocket message: " << error.what() << '\n';
  }
}

void Consumer::handleRemoteDescription(const rtc::Description& description) {
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

void Consumer::handleRemoteCandidate(const rtc::Candidate& candidate) {
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

void Consumer::queueOrSendSignalingMessage(std::string payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!websocket_.isOpen()) {
    pending_signaling_messages_.push_back(std::move(payload));
    return;
  }

  if (!websocket_.send(payload)) {
    std::cerr << "consumer failed to send signaling message over websocket\n";
  }
}

void Consumer::flushPendingSignalingMessages() {
  std::vector<std::string> pending_messages;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!websocket_.isOpen()) {
      return;
    }

    pending_messages.swap(pending_signaling_messages_);
  }

  for (auto& payload : pending_messages) {
    if (!websocket_.send(payload)) {
      std::cerr << "consumer failed to flush signaling message over websocket\n";
    }
  }
}

}  // namespace demo
