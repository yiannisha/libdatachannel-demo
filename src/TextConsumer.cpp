#include "TextConsumer.hpp"

#include "DemoLogging.hpp"
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

std::string formatReceivedTextMessage(const std::string &payload) {
  constexpr std::string_view prefix = "__text_demo_sent_at_ms=";
  const std::size_t separator = payload.find(';');
  if (payload.compare(0, prefix.size(), prefix.data()) != 0 ||
      separator == std::string::npos) {
    return payload;
  }

  try {
    const auto sent_at_ms =
        std::stoll(payload.substr(prefix.size(), separator - prefix.size()));
    const auto sent_at =
        Clock::time_point(std::chrono::milliseconds(sent_at_ms));
    const Clock::time_point received_at = Clock::now();
    const std::string message = payload.substr(separator + 1);

    std::ostringstream stream;
    stream << "text consumer received at " << formatTimestamp(received_at)
           << " (producer sent at " << formatTimestamp(sent_at)
           << "): " << message;
    return stream.str();
  } catch (const std::exception &) {
    return payload;
  }
}

std::string decodeTextMessagePayload(const std::string &payload) {
  constexpr std::string_view prefix = "__text_demo_sent_at_ms=";
  const std::size_t separator = payload.find(';');
  if (payload.compare(0, prefix.size(), prefix.data()) != 0 ||
      separator == std::string::npos) {
    return payload;
  }

  return payload.substr(separator + 1);
}

} // namespace

TextConsumer::TextConsumer(
    WebSocketSignalTransportConfig signaling_config,
    std::function<void(const std::string &)> on_text_message,
    std::string bind_address)
    : peer_config_(makePeerConfiguration(bind_address)),
      signaling_transport_(std::move(signaling_config)),
      on_text_message_(std::move(on_text_message)) {
  createPeerConnection();
  setupSignalingTransport();
  signaling_transport_.start();
}

void TextConsumer::wait() const {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

uint16_t TextConsumer::port() const { return signaling_transport_.port(); }

bool TextConsumer::isSignalingServer() const {
  return signaling_transport_.isServer();
}

std::string TextConsumer::signalingEndpoint() const {
  return signaling_transport_.endpointDescription();
}

void TextConsumer::createPeerConnection() {
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
  }
  // old_pc (if any) is destroyed here, outside the lock, so its in-flight
  // callbacks can drain without deadlocking against mutex_.
}

void TextConsumer::wirePeerCallbacks(
    const std::shared_ptr<rtc::PeerConnection> &pc) {
  pc->onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "text consumer peer state: " << state << '\n';
  });

  pc->onGatheringStateChange(
      [](rtc::PeerConnection::GatheringState state) {
        std::cout << "text consumer gathering state: " << state << '\n';
      });

  pc->onLocalDescription(
      [this](const rtc::Description &description) {
        std::cout << "text consumer generated local description\n";
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
        std::cout << "text consumer accepted data channel: " << channel->label()
                  << '\n';

        channel->onOpen([channel]() {
          std::cout << "text consumer data channel open (" << channel->label()
                    << ")\n";
          channel->send("ack from text consumer");
        });

        channel->onClosed(
            []() { std::cout << "text consumer data channel closed\n"; });

        channel->onMessage([this](const auto &message) {
          if (std::holds_alternative<rtc::string>(message)) {
            const std::string &payload = std::get<rtc::string>(message);
            if (verboseLoggingEnabled()) {
              std::cout << formatReceivedTextMessage(payload) << '\n';
            }
            if (on_text_message_) {
              on_text_message_(decodeTextMessagePayload(payload));
            }
            return;
          }

          if (verboseLoggingEnabled()) {
            std::cout << "text consumer received on data channel: "
                      << formatMessage(message) << '\n';
          }
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

    onSignalingConnected();
  });

  signaling_transport_.setOnClosed([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "text consumer signaling websocket client disconnected\n";
    } else {
      std::cout << "text consumer websocket closed\n";
    }
  });

  signaling_transport_.setOnError([](const std::string &error) {
    std::cerr << "text consumer websocket error: " << error << '\n';
  });

  signaling_transport_.setOnMessage(
      [this](const std::string &payload) { handleSignalingMessage(payload); });
}

void TextConsumer::onSignalingConnected() {
  bool reconnect;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect = signaling_connected_before_;
    signaling_connected_before_ = true;
  }

  if (reconnect) {
    std::cout << "text consumer signaling reconnected; rebuilding peer "
                 "connection\n";
    createPeerConnection();
  }
}

void TextConsumer::handleSignalingMessage(const std::string &payload) {
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
    std::cerr << "text consumer failed to handle websocket message: "
              << error.what() << '\n';
  }
}

void TextConsumer::handleRemoteDescription(
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

void TextConsumer::handleRemoteCandidate(
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

} // namespace demo
