#pragma once

#include "WebSocketSignalTransport.hpp"
#include "rtc/rtc.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class TextConsumer {
public:
  explicit TextConsumer(
      WebSocketSignalTransportConfig signaling_config,
      std::function<void(const std::string &)> on_text_message = {},
      std::string bind_address = {});

  void wait() const;
  uint16_t port() const;
  bool isSignalingServer() const;
  std::string signalingEndpoint() const;

private:
  void setupPeerConnection();
  void setupSignalingTransport();
  void handleSignalingMessage(const std::string &payload);
  void handleRemoteDescription(const rtc::Description &description);
  void handleRemoteCandidate(const rtc::Candidate &candidate);

  rtc::PeerConnection peer_connection_;
  WebSocketSignalTransport signaling_transport_;
  std::function<void(const std::string &)> on_text_message_;

  std::shared_ptr<rtc::DataChannel> data_channel_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
};

} // namespace demo
