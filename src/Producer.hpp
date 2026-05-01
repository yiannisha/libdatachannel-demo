#pragma once

#include "VideoPipeline.hpp"
#include "rtc/rtc.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class Producer {
 public:
  Producer(
      uint16_t websocket_port,
      std::string bind_address = "0.0.0.0",
      VideoPipeline::Profile video_pipeline_profile = VideoPipeline::Profile::Default);

  void wait();
  uint16_t port() const;

 private:
  void setupPeerConnection();
  void setupWebSocketServer();
  void setupVideoTrack();
  void attachClient(const std::shared_ptr<rtc::WebSocket>& client);
  void handleWebSocketMessage(const std::string& payload);
  void handleRemoteDescription(const rtc::Description& description);
  void handleRemoteCandidate(const rtc::Candidate& candidate);
  void startDataChannel();
  void queueOrSendSignalingMessage(std::string payload);
  void flushPendingSignalingMessages();

  rtc::PeerConnection peer_connection_;
  rtc::WebSocketServer server_;
  VideoPipeline video_pipeline_;

  std::shared_ptr<rtc::WebSocket> client_;
  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::shared_ptr<rtc::Track> video_track_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  bool data_channel_started_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
  std::vector<std::string> pending_signaling_messages_;
};

}  // namespace demo
