#include "WebSocketSignaler.hpp"

#include <iostream>
#include <utility>
#include <variant>

namespace demo {

namespace {

std::string formatWebSocketMessage(const std::variant<rtc::binary, rtc::string>& message) {
  if (std::holds_alternative<rtc::string>(message)) {
    return std::get<rtc::string>(message);
  }

  return "<binary payload: " +
         std::to_string(std::get<rtc::binary>(message).size()) + " bytes>";
}

}  // namespace

WebSocketSignaler::WebSocketSignaler(
    std::shared_ptr<rtc::PeerConnection> target,
    std::string url,
    rtc::WebSocket::Configuration wsConfig,
    std::unique_ptr<rtc::WebSocketServer::Configuration> serverConfig)
    : target_(std::move(target)),
      url_(std::move(url)),
      socket_(std::move(wsConfig)) {
  setupWebSocket();
  if (serverConfig) {
    setupWebSocketServer(std::move(*serverConfig));
  }
}

void WebSocketSignaler::setupWebSocket() {
  socket_.onOpen([]() {
    std::cout << "WebSocket connection established\n";
  });

  socket_.onClosed([]() {
    std::cout << "WebSocket connection closed\n";
  });

  socket_.onError([](const std::string& error) {
    std::cerr << "WebSocket error: " << error << '\n';
  });

  socket_.onMessage([this](const auto& message) {
    std::cout << "WebSocket received message: "
              << formatWebSocketMessage(message) << '\n';
    // Handle incoming messages as needed, e.g., parse them as JSON and call
    // deliverDescription or deliverCandidate accordingly.
  });

  if (!url_.empty()) {
    socket_.open(url_);
  }
}

void WebSocketSignaler::setupWebSocketServer(
    rtc::WebSocketServer::Configuration serverConfig) {
  server_ = std::make_unique<rtc::WebSocketServer>(std::move(serverConfig));
  server_->onClient([](const std::shared_ptr<rtc::WebSocket>& client) {
    std::cout << "WebSocket server accepted client\n";

    client->onOpen([]() {
      std::cout << "Accepted WebSocket client open\n";
    });

    client->onClosed([]() {
      std::cout << "Accepted WebSocket client closed\n";
    });

    client->onError([](const std::string& error) {
      std::cerr << "Accepted WebSocket client error: " << error << '\n';
    });

    client->onMessage([](const auto& message) {
      std::cout << "Accepted WebSocket client message: "
                << formatWebSocketMessage(message) << '\n';
    });
  });
}

void WebSocketSignaler::deliverDescription(const rtc::Description& description) {
  target_->setRemoteDescription(description);

  std::vector<PendingCandidate> pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending.swap(pending_candidates_);
  }

  // Deliver any candidates that were queued while waiting for the remote
  // description to be set.
  for (const auto& candidate : pending) {
    target_->addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
  }
}

void WebSocketSignaler::deliverCandidate(const rtc::Candidate& candidate) {
  if (candidate.candidate().empty()) {
    return;
  }

  bool queue_candidate = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // If the remote description has not been set yet, queue candidates until it
    // is available so the peer connection does not reject them.
    if (!remote_description_set_) {
      pending_candidates_.push_back(
          PendingCandidate{candidate.candidate(), candidate.mid()});
      queue_candidate = true;
    }
  }

  if (!queue_candidate) {
    target_->addRemoteCandidate(rtc::Candidate(candidate.candidate(), candidate.mid()));
  }
}

}  // namespace demo
