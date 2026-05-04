#pragma once

#include "WebSocketSignalTransport.hpp"
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
  explicit Consumer(WebSocketSignalTransportConfig signaling_config);
  ~Consumer();

  void wait() const;
  uint16_t port() const;
  bool isSignalingServer() const;
  std::string signalingEndpoint() const;

 private:
  void setupUdpProbe();
  void setupPeerConnection();
  void setupSignalingTransport();
  void handleSignalingMessage(const std::string& payload);
  void handleRemoteDescription(const rtc::Description& description);
  void handleRemoteCandidate(const rtc::Candidate& candidate);
  void mirrorRtpToUdpProbe(const rtc::binary& packet) const;

  rtc::PeerConnection peer_connection_;
  WebSocketSignalTransport signaling_transport_;

  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::vector<std::shared_ptr<rtc::Track>> tracks_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  std::vector<rtc::Candidate> pending_candidates_;

  int udp_probe_fd_ = -1;
  sockaddr_in udp_probe_dst_ = {};
  std::uint16_t udp_probe_port_ = 5004;
};

}  // namespace demo
