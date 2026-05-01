#include "Producer.hpp"

#include "SignalingProtocol.hpp"

#include <chrono>
#include <iostream>
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

Producer::Producer(uint16_t websocket_port,
                   std::string bind_address,
                   VideoPipeline::Profile video_pipeline_profile)
    : peer_connection_(makePeerConfiguration()),
      server_([&]() {
        rtc::WebSocketServer::Configuration config;
        config.port = websocket_port;
        config.bindAddress = std::move(bind_address);
        return config;
      }()),
      video_pipeline_(video_pipeline_profile) {
  setupPeerConnection();
  setupVideoTrack();
  video_pipeline_.setTrack(video_track_);
  video_pipeline_.start();
  setupWebSocketServer();
}

void Producer::wait() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

uint16_t Producer::port() const {
  return server_.port();
}

void Producer::setupPeerConnection() {
  peer_connection_.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "producer peer state: " << state << '\n';
  });

  peer_connection_.onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "producer gathering state: " << state << '\n';
  });

  peer_connection_.onLocalDescription([this](const rtc::Description& description) {
    std::cout << "producer generated local description\n";
    queueOrSendSignalingMessage(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate& candidate) {
    queueOrSendSignalingMessage(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });
}

void Producer::setupVideoTrack() {
  constexpr rtc::SSRC kVideoSsrc = 42;

  rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
  video.addH264Codec(96);
  video.addSSRC(kVideoSsrc, "video", "stream1", "track1");

  video_track_ = peer_connection_.addTrack(video);
  video_track_->onOpen([this]() {
    std::cout << "producer video track open (" << video_track_->mid() << ")\n";
  });
  video_track_->onClosed([]() {
    std::cout << "producer video track closed\n";
  });
  video_track_->onError([](const std::string& error) {
    std::cerr << "producer video track error: " << error << '\n';
  });
}

void Producer::setupWebSocketServer() {
  server_.onClient([this](std::shared_ptr<rtc::WebSocket> client) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ && !client_->isClosed()) {
      std::cerr << "producer rejected an extra websocket client\n";
      client->close();
      return;
    }

    client_ = std::move(client);
    attachClient(client_);
  });
}

void Producer::attachClient(const std::shared_ptr<rtc::WebSocket>& client) {
  client->onOpen([this]() {
    std::cout << "producer websocket client connected\n";
    flushPendingSignalingMessages();
    startDataChannel();
  });

  client->onClosed([this]() {
    std::cout << "producer websocket client disconnected\n";
  });

  client->onError([](const std::string& error) {
    std::cerr << "producer websocket error: " << error << '\n';
  });

  client->onMessage([this](const auto& message) {
    if (!std::holds_alternative<rtc::string>(message)) {
      std::cerr << "producer received an unexpected binary websocket message\n";
      return;
    }

    handleWebSocketMessage(std::get<rtc::string>(message));
  });
}

void Producer::handleWebSocketMessage(const std::string& payload) {
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
    std::cerr << "producer failed to handle websocket message: " << error.what() << '\n';
  }
}

void Producer::handleRemoteDescription(const rtc::Description& description) {
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

void Producer::handleRemoteCandidate(const rtc::Candidate& candidate) {
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

void Producer::startDataChannel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (data_channel_started_) {
    return;
  }

  data_channel_started_ = true;
  data_channel_ = peer_connection_.createDataChannel("producer-demo");
  data_channel_->onOpen([channel = data_channel_]() {
    std::cout << "producer data channel open (" << channel->label() << ")\n";
    channel->send("hello from producer");
    channel->send("second message from producer");
    channel->send("third message from producer");
  });

  data_channel_->onClosed([]() {
    std::cout << "producer data channel closed\n";
  });

  data_channel_->onMessage([](const auto& message) {
    std::cout << "producer received on data channel: " << formatMessage(message) << '\n';
  });
}

void Producer::queueOrSendSignalingMessage(std::string payload) {
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
    std::cerr << "producer failed to send signaling message over websocket\n";
  }
}

void Producer::flushPendingSignalingMessages() {
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
      std::cerr << "producer failed to flush signaling message over websocket\n";
    }
  }
}

}  // namespace demo
