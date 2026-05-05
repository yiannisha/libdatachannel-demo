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

std::string
formatMessage(const std::variant<rtc::binary, rtc::string> &message) {
  if (std::holds_alternative<rtc::string>(message)) {
    return std::get<rtc::string>(message);
  }

  return "<binary payload: " +
         std::to_string(std::get<rtc::binary>(message).size()) + " bytes>";
}

} // namespace

Consumer::Consumer(WebSocketSignalTransportConfig signaling_config,
                   RtpPacketCallback on_rtp_packet, bool enable_udp_probe)
    : peer_connection_(makePeerConfiguration()),
      signaling_transport_(std::move(signaling_config)),
      on_rtp_packet_(std::move(on_rtp_packet)),
      enable_udp_probe_(enable_udp_probe) {
  setupUdpProbe();
  setupPeerConnection();
  setupSignalingTransport();
  signaling_transport_.start();
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

uint16_t Consumer::port() const { return signaling_transport_.port(); }

bool Consumer::isSignalingServer() const {
  return signaling_transport_.isServer();
}

std::string Consumer::signalingEndpoint() const {
  return signaling_transport_.endpointDescription();
}

void Consumer::setupUdpProbe() {
  if (!enable_udp_probe_) {
    return;
  }

  udp_probe_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_probe_fd_ < 0) {
    throw std::runtime_error(
        "consumer failed to create UDP debug probe socket");
  }

  udp_probe_dst_.sin_family = AF_INET;
  udp_probe_dst_.sin_port = htons(udp_probe_port_);
  if (inet_pton(AF_INET, "127.0.0.1", &udp_probe_dst_.sin_addr) != 1) {
    throw std::runtime_error(
        "consumer failed to configure UDP debug probe destination");
  }

  std::cout << "consumer UDP debug probe forwarding RTP to 127.0.0.1:"
            << udp_probe_port_ << '\n';
}

void Consumer::setupPeerConnection() {
  peer_connection_.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "consumer peer state: " << state << '\n';
  });

  peer_connection_.onGatheringStateChange(
      [](rtc::PeerConnection::GatheringState state) {
        std::cout << "consumer gathering state: " << state << '\n';
      });

  peer_connection_.onLocalDescription(
      [this](const rtc::Description &description) {
        std::cout << "consumer generated local description\n";
        signaling_transport_.send(serializeSignalingMessage(
            makeLocalDescriptionMessage(description)));
      });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate &candidate) {
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });

  peer_connection_.onDataChannel(
      [this](const std::shared_ptr<rtc::DataChannel> &channel) {
        data_channel_ = channel;
        std::cout << "consumer accepted data channel: " << channel->label()
                  << '\n';

        data_channel_->onOpen([channel]() {
          std::cout << "consumer data channel open (" << channel->label()
                    << ")\n";
        });

        data_channel_->onClosed(
            []() { std::cout << "consumer data channel closed\n"; });

        data_channel_->onMessage([](const auto &message) {
          std::cout << "consumer received on data channel: "
                    << formatMessage(message) << '\n';
        });
      });

  peer_connection_.onTrack([this](const std::shared_ptr<rtc::Track> &track) {
    tracks_.push_back(track);
    std::cout << "consumer accepted track: mid=" << track->mid()
              << " direction=" << track->direction() << '\n';

    track->onOpen([track]() {
      std::cout << "consumer track open: " << track->mid() << '\n';
    });

    track->onClosed([track]() {
      std::cout << "consumer track closed: " << track->mid() << '\n';
    });

    track->onError([track](const std::string &error) {
      std::cerr << "consumer track error (" << track->mid() << "): " << error
                << '\n';
    });

    track->onMessage(
        [this, track, track_mid = track->mid()](rtc::binary message) {
          static std::uint64_t packet_count = 0;
          ++packet_count;

          if (on_rtp_packet_) {
            on_rtp_packet_(track_mid, message);
          }

          mirrorRtpToUdpProbe(message);

          if (message.size() >= sizeof(rtc::RtpHeader)) {
            const auto *header =
                reinterpret_cast<const rtc::RtpHeader *>(message.data());
            if (packet_count == 1 || packet_count % 30 == 0) {
              std::cout << "consumer received RTP packet #" << packet_count
                        << " on track " << track->mid()
                        << " bytes=" << message.size() << " payloadType="
                        << static_cast<int>(header->payloadType())
                        << " seq=" << header->seqNumber()
                        << " timestamp=" << header->timestamp()
                        << " ssrc=" << header->ssrc() << '\n';
            }
          } else {
            std::cout << "consumer received short media packet on track "
                      << track->mid() << " bytes=" << message.size() << '\n';
          }
        },
        nullptr);
  });
}

void Consumer::setupSignalingTransport() {
  signaling_transport_.setOnConnected([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "consumer signaling websocket client connected\n";
    } else {
      std::cout << "consumer websocket connected to "
                << signaling_transport_.endpointDescription() << '\n';
    }
  });

  signaling_transport_.setOnClosed([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "consumer signaling websocket client disconnected\n";
    } else {
      std::cout << "consumer websocket closed\n";
    }
  });

  signaling_transport_.setOnError([](const std::string &error) {
    std::cerr << "consumer websocket error: " << error << '\n';
  });

  signaling_transport_.setOnMessage(
      [this](const std::string &payload) { handleSignalingMessage(payload); });
}

void Consumer::handleSignalingMessage(const std::string &payload) {
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
  } catch (const std::exception &error) {
    std::cerr << "consumer failed to handle websocket message: " << error.what()
              << '\n';
  }
}

void Consumer::handleRemoteDescription(const rtc::Description &description) {
  peer_connection_.setRemoteDescription(description);

  std::vector<rtc::Candidate> pending_candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending_candidates.swap(pending_candidates_);
  }

  for (auto &candidate : pending_candidates) {
    peer_connection_.addRemoteCandidate(std::move(candidate));
  }
}

void Consumer::handleRemoteCandidate(const rtc::Candidate &candidate) {
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

void Consumer::mirrorRtpToUdpProbe(const rtc::binary &packet) const {
  if (udp_probe_fd_ < 0) {
    return;
  }

  const ssize_t sent = sendto(
      udp_probe_fd_, reinterpret_cast<const char *>(packet.data()),
      packet.size(), 0, reinterpret_cast<const sockaddr *>(&udp_probe_dst_),
      sizeof(udp_probe_dst_));

  if (sent < 0) {
    std::cerr << "consumer failed to mirror RTP packet to UDP probe\n";
  }
}

} // namespace demo
