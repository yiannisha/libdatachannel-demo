#include "TextProducer.hpp"

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

std::string encodeMessagePayload(const std::string& body, Clock::time_point sent_at) {
  const auto sent_at_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          sent_at.time_since_epoch())
          .count();
  return "__text_demo_sent_at_ms=" + std::to_string(sent_at_ms) + ';' + body;
}

bool sendTextMessage(const std::shared_ptr<rtc::DataChannel>& channel,
                     const std::string& body) {
  const Clock::time_point sent_at = Clock::now();
  if (!channel->send(encodeMessagePayload(body, sent_at))) {
    return false;
  }

  std::cout << "text producer sent at " << formatTimestamp(sent_at)
            << ": " << body << '\n';
  return true;
}

std::vector<std::string> makeDefaultMessages() {
  return {
      "hello from text producer",
      "second text message",
      "third text message",
  };
}

}  // namespace

TextProducer::TextProducer(WebSocketSignalTransportConfig signaling_config,
                           std::vector<std::string> messages)
    : peer_connection_(makePeerConfiguration()),
      signaling_transport_(std::move(signaling_config)) {
  pending_data_channel_messages_ =
      messages.empty() ? makeDefaultMessages() : std::move(messages);
  setupPeerConnection();
  setupSignalingTransport();
  signaling_transport_.start();
}

void TextProducer::enqueueMessage(std::string message) {
  std::shared_ptr<rtc::DataChannel> channel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data_channel_ || !data_channel_open_) {
      pending_data_channel_messages_.push_back(std::move(message));
      return;
    }

    channel = data_channel_;
  }

  if (!sendTextMessage(channel, message)) {
    std::cerr << "text producer failed to send queued message over data channel\n";
    return;
  }
}

void TextProducer::wait() const {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

uint16_t TextProducer::port() const {
  return signaling_transport_.port();
}

bool TextProducer::isSignalingServer() const {
  return signaling_transport_.isServer();
}

std::string TextProducer::signalingEndpoint() const {
  return signaling_transport_.endpointDescription();
}

void TextProducer::setupPeerConnection() {
  peer_connection_.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "text producer peer state: " << state << '\n';
  });

  peer_connection_.onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "text producer gathering state: " << state << '\n';
  });

  peer_connection_.onLocalDescription([this](const rtc::Description& description) {
    std::cout << "text producer generated local description\n";
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate& candidate) {
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });
}

void TextProducer::setupSignalingTransport() {
  signaling_transport_.setOnConnected([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "text producer signaling websocket client connected\n";
    } else {
      std::cout << "text producer signaling websocket connected to "
                << signaling_transport_.endpointDescription() << '\n';
    }

    startDataChannel();
  });

  signaling_transport_.setOnClosed([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "text producer signaling websocket client disconnected\n";
    } else {
      std::cout << "text producer websocket closed\n";
    }
  });

  signaling_transport_.setOnError([](const std::string& error) {
    std::cerr << "text producer websocket error: " << error << '\n';
  });

  signaling_transport_.setOnMessage([this](const std::string& payload) {
    handleSignalingMessage(payload);
  });
}

void TextProducer::handleSignalingMessage(const std::string& payload) {
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
    std::cerr << "text producer failed to handle websocket message: "
              << error.what() << '\n';
  }
}

void TextProducer::handleRemoteDescription(const rtc::Description& description) {
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

void TextProducer::handleRemoteCandidate(const rtc::Candidate& candidate) {
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

void TextProducer::startDataChannel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (data_channel_started_) {
    return;
  }

  data_channel_started_ = true;
  data_channel_ = peer_connection_.createDataChannel("text-demo");
  data_channel_->onOpen([this, channel = data_channel_]() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      data_channel_open_ = true;
    }

    std::cout << "text producer data channel open (" << channel->label() << ")\n";
    flushPendingDataChannelMessages();
  });

  data_channel_->onClosed([this]() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_channel_open_ = false;
    std::cout << "text producer data channel closed\n";
  });

  data_channel_->onMessage([](const auto& message) {
    std::cout << "text producer received on data channel: "
              << formatMessage(message) << '\n';
  });
}

void TextProducer::flushPendingDataChannelMessages() {
  std::shared_ptr<rtc::DataChannel> channel;
  std::vector<std::string> pending_messages;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data_channel_ || !data_channel_open_) {
      return;
    }

    channel = data_channel_;
    pending_messages.swap(pending_data_channel_messages_);
  }

  for (const std::string& message : pending_messages) {
    if (!sendTextMessage(channel, message)) {
      std::cerr << "text producer failed to flush queued data channel message\n";
      return;
    }
  }
}

}  // namespace demo
