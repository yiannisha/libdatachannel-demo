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

TextProducer::TextProducer(uint16_t websocket_port,
                           std::string bind_address,
                           std::vector<std::string> messages)
    : peer_connection_(makePeerConfiguration()),
      server_([&]() {
        rtc::WebSocketServer::Configuration config;
        config.port = websocket_port;
        config.bindAddress = std::move(bind_address);
        return config;
      }()) {
  pending_data_channel_messages_ =
      messages.empty() ? makeDefaultMessages() : std::move(messages);
  setupPeerConnection();
  setupWebSocketServer();
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
  return server_.port();
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
    queueOrSendSignalingMessage(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate& candidate) {
    queueOrSendSignalingMessage(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });
}

void TextProducer::setupWebSocketServer() {
  server_.onClient([this](std::shared_ptr<rtc::WebSocket> client) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ && !client_->isClosed()) {
      std::cerr << "text producer rejected an extra websocket client\n";
      client->close();
      return;
    }

    client_ = std::move(client);
    attachClient(client_);
  });
}

void TextProducer::attachClient(const std::shared_ptr<rtc::WebSocket>& client) {
  client->onOpen([this]() {
    std::cout << "text producer websocket client connected\n";
    flushPendingSignalingMessages();
    startDataChannel();
  });

  client->onClosed([this]() {
    std::cout << "text producer websocket client disconnected\n";
  });

  client->onError([](const std::string& error) {
    std::cerr << "text producer websocket error: " << error << '\n';
  });

  client->onMessage([this](const auto& message) {
    if (!std::holds_alternative<rtc::string>(message)) {
      std::cerr << "text producer received an unexpected binary websocket message\n";
      return;
    }

    handleWebSocketMessage(std::get<rtc::string>(message));
  });
}

void TextProducer::handleWebSocketMessage(const std::string& payload) {
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

void TextProducer::queueOrSendSignalingMessage(std::string payload) {
  std::shared_ptr<rtc::WebSocket> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_ || !client_->isOpen()) {
      pending_signaling_messages_.push_back(std::move(payload));
      return;
    }

    client = client_;
  }

  if (!client->send(payload)) {
    std::cerr << "text producer failed to send signaling message over websocket\n";
  }
}

void TextProducer::flushPendingSignalingMessages() {
  std::shared_ptr<rtc::WebSocket> client;
  std::vector<std::string> pending_messages;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_ || !client_->isOpen()) {
      return;
    }

    client = client_;
    pending_messages.swap(pending_signaling_messages_);
  }

  for (auto& payload : pending_messages) {
    if (!client->send(payload)) {
      std::cerr << "text producer failed to flush signaling message over websocket\n";
    }
  }
}

}  // namespace demo
