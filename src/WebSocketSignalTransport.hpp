#pragma once

#include "rtc/rtc.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace demo {

struct WebSocketServerOptions {
  uint16_t port = 8080;
  std::string bind_address = "0.0.0.0";
};

struct WebSocketClientOptions {
  std::string url;
  std::chrono::milliseconds reconnect_interval{1000};
};

using WebSocketSignalTransportConfig =
    std::variant<WebSocketServerOptions, WebSocketClientOptions>;

class WebSocketSignalTransport {
 public:
  explicit WebSocketSignalTransport(WebSocketSignalTransportConfig config);
  ~WebSocketSignalTransport();

  void start();
  void setOnConnected(std::function<void()> callback);
  void setOnClosed(std::function<void()> callback);
  void setOnError(std::function<void(const std::string&)> callback);
  void setOnMessage(std::function<void(const std::string&)> callback);

  void send(std::string payload);
  bool isConnected() const;
  bool isServer() const;
  uint16_t port() const;
  std::string endpointDescription() const;

 private:
  void runClientReconnectLoop();
  void setupClient(const WebSocketClientOptions& options);
  void setupServer(const WebSocketServerOptions& options);
  void attachPeer(const std::shared_ptr<rtc::WebSocket>& peer);
  void handleOpen();
  void handleClosed(const std::shared_ptr<rtc::WebSocket>& peer);
  void flushPendingMessages();
  void reportError(const std::string& error);

  WebSocketSignalTransportConfig config_;
  std::shared_ptr<rtc::WebSocket> socket_;
  std::unique_ptr<rtc::WebSocketServer> server_;

  mutable std::mutex mutex_;
  std::condition_variable reconnect_cv_;
  bool started_ = false;
  bool stop_requested_ = false;
  bool client_connecting_ = false;
  std::shared_ptr<rtc::WebSocket> peer_;
  std::vector<std::string> pending_messages_;
  std::function<void()> on_connected_;
  std::function<void()> on_closed_;
  std::function<void(const std::string&)> on_error_;
  std::function<void(const std::string&)> on_message_;
  std::thread reconnect_thread_;
};

}  // namespace demo
