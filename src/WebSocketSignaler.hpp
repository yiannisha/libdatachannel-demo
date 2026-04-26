#pragma once

#include "Signaler.hpp"
#include "rtc/rtc.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class WebSocketSignaler : public Signaler {
 public:
  WebSocketSignaler(std::shared_ptr<rtc::PeerConnection> target,
                    std::string url,
                    rtc::WebSocket::Configuration wsConfig = {},
                    std::unique_ptr<rtc::WebSocketServer::Configuration> serverConfig = nullptr);

  void deliverDescription(const rtc::Description& description) override;
  void deliverCandidate(const rtc::Candidate& candidate) override;

 private:
  struct PendingCandidate {
    std::string candidate;
    std::string mid;
  };

  std::shared_ptr<rtc::PeerConnection> target_;
  std::mutex mutex_;
  bool remote_description_set_ = false;
  std::vector<PendingCandidate> pending_candidates_;

  std::string url_;
  rtc::WebSocket socket_;
  std::unique_ptr<rtc::WebSocketServer> server_;

  void setupWebSocket();
  void setupWebSocketServer(rtc::WebSocketServer::Configuration serverConfig);
};

}  // namespace demo
