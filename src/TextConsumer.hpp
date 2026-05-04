#pragma once

#include "rtc/rtc.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class TextConsumer {
 public:
  explicit TextConsumer(std::string websocket_url);

  void wait() const;

 private:
  void setupPeerConnection();
  void setupWebSocket();
  void handleWebSocketMessage(const std::string& payload);
  void handleRemoteDescription(const rtc::Description& description);
  void handleRemoteCandidate(const rtc::Candidate& candidate);
  void queueOrSendSignalingMessage(std::string payload);
  void flushPendingSignalingMessages();

  std::string websocket_url_;
  rtc::PeerConnection peer_connection_;
  rtc::WebSocket websocket_;

  std::shared_ptr<rtc::DataChannel> data_channel_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
  std::vector<std::string> pending_signaling_messages_;
};

}  // namespace demo
