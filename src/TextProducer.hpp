#pragma once

#include "rtc/rtc.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class TextProducer {
 public:
  TextProducer(uint16_t websocket_port,
               std::string bind_address = "0.0.0.0",
               std::vector<std::string> messages = {});

  void enqueueMessage(std::string message);
  void wait() const;
  uint16_t port() const;

 private:
  void setupPeerConnection();
  void setupWebSocketServer();
  void attachClient(const std::shared_ptr<rtc::WebSocket>& client);
  void handleWebSocketMessage(const std::string& payload);
  void handleRemoteDescription(const rtc::Description& description);
  void handleRemoteCandidate(const rtc::Candidate& candidate);
  void startDataChannel();
  void flushPendingDataChannelMessages();
  void queueOrSendSignalingMessage(std::string payload);
  void flushPendingSignalingMessages();

  rtc::PeerConnection peer_connection_;
  rtc::WebSocketServer server_;

  std::shared_ptr<rtc::WebSocket> client_;
  std::shared_ptr<rtc::DataChannel> data_channel_;

  mutable std::mutex mutex_;
  bool remote_description_set_ = false;
  bool data_channel_started_ = false;
  bool data_channel_open_ = false;
  std::vector<rtc::Candidate> pending_candidates_;
  std::vector<std::string> pending_signaling_messages_;
  std::vector<std::string> pending_data_channel_messages_;
};

}  // namespace demo
