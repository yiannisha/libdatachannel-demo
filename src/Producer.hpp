#pragma once

#include "VideoPipeline.hpp"
#include "WebSocketSignalTransport.hpp"
#include "rtc/rtc.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace demo {

class Producer {
 public:
  Producer(WebSocketSignalTransportConfig signaling_config,
           VideoPipeline::Profile video_pipeline_profile =
               VideoPipeline::Profile::Default,
           VideoPipeline::Config video_pipeline_config =
               VideoPipeline::Config{});

  void wait();
  uint16_t port() const;
  bool isSignalingServer() const;
  std::string signalingEndpoint() const;

 private:
  void setupPeerConnection();
  void setupVideoTracks(VideoPipeline::Profile video_pipeline_profile);
  void setupSignalingTransport();
  void handleSignalingMessage(const std::string& payload);
  void handleRemoteDescription(const rtc::Description& description);
  void handleRemoteCandidate(const rtc::Candidate& candidate);
  void startDataChannel();

  rtc::PeerConnection peer_connection_;
  WebSocketSignalTransport signaling_transport_;
  VideoPipeline video_pipeline_;

  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::vector<std::shared_ptr<rtc::Track>> video_tracks_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  bool data_channel_started_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
};

}  // namespace demo
