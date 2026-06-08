#include "Consumer.hpp"

#include "DemoLogging.hpp"
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

rtc::Configuration makePeerConfiguration(const std::string &bind_address) {
  rtc::Configuration config;
  config.iceServers.emplace_back("stun:stun.l.google.com:19302");
  if (!bind_address.empty()) {
    // Pin ICE to a single local interface (e.g. the direct Ethernet link)
    // instead of gathering candidates on every interface.
    config.bindAddress = bind_address;
  }
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
                   RtpPacketCallback on_rtp_packet, bool enable_udp_probe,
                   std::string bind_address)
    : peer_config_(makePeerConfiguration(bind_address)),
      signaling_transport_(std::move(signaling_config)),
      on_rtp_packet_(std::move(on_rtp_packet)),
      enable_udp_probe_(enable_udp_probe) {
  setupUdpProbe();
  createPeerConnection();
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

void Consumer::createPeerConnection() {
  auto new_pc = std::make_shared<rtc::PeerConnection>(peer_config_);
  wirePeerCallbacks(new_pc);

  std::shared_ptr<rtc::PeerConnection> old_pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old_pc = std::move(peer_connection_);
    peer_connection_ = new_pc;
    remote_description_set_ = false;
    pending_candidates_.clear();
    data_channel_.reset();
    tracks_.clear();
  }
  // old_pc (if any) is destroyed here, outside the lock, so its in-flight
  // callbacks can drain without deadlocking against mutex_.
}

void Consumer::wirePeerCallbacks(
    const std::shared_ptr<rtc::PeerConnection> &pc) {
  pc->onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "consumer peer state: " << state << '\n';
  });

  pc->onGatheringStateChange(
      [](rtc::PeerConnection::GatheringState state) {
        std::cout << "consumer gathering state: " << state << '\n';
      });

  pc->onLocalDescription(
      [this](const rtc::Description &description) {
        std::cout << "consumer generated local description\n";
        signaling_transport_.send(serializeSignalingMessage(
            makeLocalDescriptionMessage(description)));
      });

  pc->onLocalCandidate([this](const rtc::Candidate &candidate) {
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });

  pc->onDataChannel(
      [this](const std::shared_ptr<rtc::DataChannel> &channel) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          data_channel_ = channel;
        }
        std::cout << "consumer accepted data channel: " << channel->label()
                  << '\n';

        channel->onOpen([channel]() {
          std::cout << "consumer data channel open (" << channel->label()
                    << ")\n";
        });

        channel->onClosed(
            []() { std::cout << "consumer data channel closed\n"; });

        channel->onMessage([](const auto &message) {
          std::cout << "consumer received on data channel: "
                    << formatMessage(message) << '\n';
        });
      });

  pc->onTrack([this](const std::shared_ptr<rtc::Track> &track) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tracks_.push_back(track);
    }
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

          if (!verboseLoggingEnabled()) {
            return;
          }

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

    onSignalingConnected();
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

void Consumer::onSignalingConnected() {
  bool reconnect;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect = signaling_connected_before_;
    signaling_connected_before_ = true;
  }

  if (reconnect) {
    std::cout << "consumer signaling reconnected; rebuilding peer connection\n";
    createPeerConnection();
  }
}

void Consumer::handleSignalingMessage(const std::string &payload) {
  std::shared_ptr<rtc::PeerConnection> pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pc = peer_connection_;
  }
  if (!pc) {
    return;
  }

  try {
    const SignalingMessage message = parseSignalingMessage(payload);
    switch (message.command) {
    case SignalingCommand::LocalDescription:
      handleRemoteDescription(
          pc, parseRemoteDescription(message, pc->signalingState()));
      break;
    case SignalingCommand::LocalCandidate:
      handleRemoteCandidate(pc, parseRemoteCandidate(message));
      break;
    }
  } catch (const std::exception &error) {
    std::cerr << "consumer failed to handle websocket message: " << error.what()
              << '\n';
  }
}

void Consumer::handleRemoteDescription(
    const std::shared_ptr<rtc::PeerConnection> &pc,
    const rtc::Description &description) {
  pc->setRemoteDescription(description);

  std::vector<rtc::Candidate> pending_candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending_candidates.swap(pending_candidates_);
  }

  for (auto &candidate : pending_candidates) {
    pc->addRemoteCandidate(std::move(candidate));
  }
}

void Consumer::handleRemoteCandidate(
    const std::shared_ptr<rtc::PeerConnection> &pc,
    const rtc::Candidate &candidate) {
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
    pc->addRemoteCandidate(candidate);
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
