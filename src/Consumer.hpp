#pragma once

#include "rtc/rtc.hpp"

#include <netinet/in.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class Consumer {
 public:
  explicit Consumer(std::string websocket_url);
  ~Consumer();

  void wait() const;

 private:
  void setupUdpProbe();
  void setupPeerConnection();
  void setupWebSocket();
  void handleWebSocketMessage(const std::string& payload);
  void handleRemoteDescription(const rtc::Description& description);
  void handleRemoteCandidate(const rtc::Candidate& candidate);
  void queueOrSendSignalingMessage(std::string payload);
  void flushPendingSignalingMessages();
  void mirrorRtpToUdpProbe(const rtc::binary& packet) const;

  std::string websocket_url_;
  rtc::PeerConnection peer_connection_;
  rtc::WebSocket websocket_;

  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::vector<std::shared_ptr<rtc::Track>> tracks_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
  std::vector<std::string> pending_signaling_messages_;

  int udp_probe_fd_ = -1;
  sockaddr_in udp_probe_dst_ = {};
  std::uint16_t udp_probe_port_ = 5004;
};

}  // namespace demo
