#include "Consumer.hpp"

#include "SignalingProtocol.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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
  setupUdpProbe();
  setupPeerConnection();
  setupWebSocket();
}

Consumer::~Consumer() {
  if (udp_probe_fd_ >= 0) {
    close(udp_probe_fd_);
  }
}

void Consumer::wait() const {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void Consumer::setupUdpProbe() {
  udp_probe_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_probe_fd_ < 0) {
    throw std::runtime_error("consumer failed to create UDP debug probe socket");
  }

  udp_probe_dst_.sin_family = AF_INET;
  udp_probe_dst_.sin_port = htons(udp_probe_port_);
  if (inet_pton(AF_INET, "127.0.0.1", &udp_probe_dst_.sin_addr) != 1) {
    throw std::runtime_error("consumer failed to configure UDP debug probe destination");
  }

  std::cout << "consumer UDP debug probe forwarding RTP to 127.0.0.1:"
            << udp_probe_port_ << '\n';
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

  peer_connection_.onTrack([this](const std::shared_ptr<rtc::Track>& track) {
    tracks_.push_back(track);
    std::cout << "consumer accepted track: mid=" << track->mid()
              << " direction=" << track->direction() << '\n';

    track->onOpen([track]() {
      std::cout << "consumer track open: " << track->mid() << '\n';
    });

    track->onClosed([track]() {
      std::cout << "consumer track closed: " << track->mid() << '\n';
    });

    track->onError([track](const std::string& error) {
      std::cerr << "consumer track error (" << track->mid() << "): " << error << '\n';
    });

    track->onMessage([this, track](rtc::binary message) {
      static std::uint64_t packet_count = 0;
      ++packet_count;

      mirrorRtpToUdpProbe(message);

      if (message.size() >= sizeof(rtc::RtpHeader)) {
        const auto* header = reinterpret_cast<const rtc::RtpHeader*>(message.data());
        if (packet_count == 1 || packet_count % 30 == 0) {
          std::cout << "consumer received RTP packet #" << packet_count
                    << " on track " << track->mid()
                    << " bytes=" << message.size()
                    << " payloadType=" << static_cast<int>(header->payloadType())
                    << " seq=" << header->seqNumber()
                    << " timestamp=" << header->timestamp()
                    << " ssrc=" << header->ssrc() << '\n';
        }
      } else {
        std::cout << "consumer received short media packet on track "
                  << track->mid() << " bytes=" << message.size() << '\n';
      }
    }, nullptr);
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

void Consumer::mirrorRtpToUdpProbe(const rtc::binary& packet) const {
  if (udp_probe_fd_ < 0) {
    return;
  }

  const ssize_t sent = sendto(
      udp_probe_fd_,
      reinterpret_cast<const char*>(packet.data()),
      packet.size(),
      0,
      reinterpret_cast<const sockaddr*>(&udp_probe_dst_),
      sizeof(udp_probe_dst_));

  if (sent < 0) {
    std::cerr << "consumer failed to mirror RTP packet to UDP probe\n";
  }
}

}  // namespace demo
