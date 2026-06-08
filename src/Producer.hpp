#pragma once

#include "VideoPipeline.hpp"
#include "WebSocketSignalTransport.hpp"
#include "rtc/rtc.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class Producer {
 public:
  Producer(WebSocketSignalTransportConfig signaling_config,
           VideoPipeline::Profile video_pipeline_profile =
               VideoPipeline::Profile::Default,
           VideoPipeline::Config video_pipeline_config =
               VideoPipeline::Config{},
           std::string bind_address = {});

  void wait();
  uint16_t port() const;
  bool isSignalingServer() const;
  std::string signalingEndpoint() const;

  // Per-output (RTP packets / encoded frames) counters for the stats table.
  std::vector<VideoPipeline::OutputStat> videoOutputStats() const;

 private:
  void createPeerConnection();
  void wirePeerCallbacks(const std::shared_ptr<rtc::PeerConnection>& pc);
  void setupSignalingTransport();
  void onSignalingConnected();
  void handleSignalingMessage(const std::string& payload);
  void handleRemoteDescription(const std::shared_ptr<rtc::PeerConnection>& pc,
                               const rtc::Description& description);
  void handleRemoteCandidate(const std::shared_ptr<rtc::PeerConnection>& pc,
                             const rtc::Candidate& candidate);
  void startDataChannel();

  rtc::Configuration peer_config_;
  VideoPipeline::Profile video_pipeline_profile_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  WebSocketSignalTransport signaling_transport_;
  VideoPipeline video_pipeline_;

  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::vector<std::shared_ptr<rtc::Track>> video_tracks_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  bool data_channel_started_ = false;
  bool signaling_connected_before_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
};

}  // namespace demo
