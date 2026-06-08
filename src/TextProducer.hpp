#pragma once

#include "WebSocketSignalTransport.hpp"
#include "rtc/rtc.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class TextProducer {
public:
  TextProducer(WebSocketSignalTransportConfig signaling_config,
               std::vector<std::string> messages = {},
               bool use_default_messages = true,
               std::string data_channel_label = "text-demo",
               std::string bind_address = {});

  void enqueueMessage(std::string message);
  bool sendMessageIfOpen(const std::string &message);
  void wait() const;
  uint16_t port() const;
  bool isSignalingServer() const;
  std::string signalingEndpoint() const;

private:
  void createPeerConnection();
  void wirePeerCallbacks(const std::shared_ptr<rtc::PeerConnection> &pc);
  void setupSignalingTransport();
  void onSignalingConnected();
  void handleSignalingMessage(const std::string &payload);
  void handleRemoteDescription(const std::shared_ptr<rtc::PeerConnection> &pc,
                               const rtc::Description &description);
  void handleRemoteCandidate(const std::shared_ptr<rtc::PeerConnection> &pc,
                             const rtc::Candidate &candidate);
  void startDataChannel();
  void flushPendingDataChannelMessages();

  rtc::Configuration peer_config_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  WebSocketSignalTransport signaling_transport_;

  std::shared_ptr<rtc::DataChannel> data_channel_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  bool data_channel_started_ = false;
  bool data_channel_open_ = false;
  bool signaling_connected_before_ = false;
  std::string data_channel_label_;
  std::vector<rtc::Candidate> pending_candidates_;
  std::vector<std::string> pending_data_channel_messages_;
};

} // namespace demo
