#include "WebSocketSignalTransport.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace demo {

WebSocketSignalTransport::WebSocketSignalTransport(
    WebSocketSignalTransportConfig config)
    : config_(std::move(config)) {}

WebSocketSignalTransport::~WebSocketSignalTransport() {
  std::shared_ptr<rtc::WebSocket> peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
    peer = peer_;
  }

  reconnect_cv_.notify_all();

  if (peer && !peer->isClosed()) {
    peer->close();
  }

  if (reconnect_thread_.joinable()) {
    reconnect_thread_.join();
  }
}

void WebSocketSignalTransport::start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return;
    }

    started_ = true;
  }

  if (std::holds_alternative<WebSocketServerOptions>(config_)) {
    setupServer(std::get<WebSocketServerOptions>(config_));
    return;
  }

  reconnect_thread_ = std::thread([this]() {
    runClientReconnectLoop();
  });
}

void WebSocketSignalTransport::setOnConnected(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_connected_ = std::move(callback);
}

void WebSocketSignalTransport::setOnClosed(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_closed_ = std::move(callback);
}

void WebSocketSignalTransport::setOnError(
    std::function<void(const std::string&)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_error_ = std::move(callback);
}

void WebSocketSignalTransport::setOnMessage(
    std::function<void(const std::string&)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_message_ = std::move(callback);
}

void WebSocketSignalTransport::send(std::string payload) {
  std::shared_ptr<rtc::WebSocket> peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!peer_ || !peer_->isOpen()) {
      pending_messages_.push_back(std::move(payload));
      return;
    }

    peer = peer_;
  }

  if (!peer->send(payload)) {
    reportError("failed to send signaling message over websocket");
  }
}

bool WebSocketSignalTransport::isConnected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_ && peer_->isOpen();
}

bool WebSocketSignalTransport::isServer() const {
  return std::holds_alternative<WebSocketServerOptions>(config_);
}

uint16_t WebSocketSignalTransport::port() const {
  if (!server_) {
    return 0;
  }

  return server_->port();
}

std::string WebSocketSignalTransport::endpointDescription() const {
  if (std::holds_alternative<WebSocketServerOptions>(config_)) {
    const WebSocketServerOptions& options = std::get<WebSocketServerOptions>(config_);
    return "ws://" + options.bind_address + ':' + std::to_string(port()) + '/';
  }

  return std::get<WebSocketClientOptions>(config_).url;
}

void WebSocketSignalTransport::runClientReconnectLoop() {
  const WebSocketClientOptions options = std::get<WebSocketClientOptions>(config_);

  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      reconnect_cv_.wait(lock, [this]() {
        return stop_requested_ || (!peer_ && !client_connecting_);
      });

      if (stop_requested_) {
        return;
      }

      client_connecting_ = true;
    }

    setupClient(options);

    std::unique_lock<std::mutex> lock(mutex_);
    reconnect_cv_.wait(lock, [this]() {
      return stop_requested_ || (!peer_ && !client_connecting_);
    });

    if (stop_requested_) {
      return;
    }

    lock.unlock();
    reportError("retrying websocket connection to " + options.url + " in " +
                std::to_string(options.reconnect_interval.count()) + " ms");
    std::this_thread::sleep_for(options.reconnect_interval);
  }
}

void WebSocketSignalTransport::setupClient(const WebSocketClientOptions& options) {
  if (options.url.empty()) {
    throw std::invalid_argument("websocket client url must not be empty");
  }

  std::shared_ptr<rtc::WebSocket> socket = std::make_shared<rtc::WebSocket>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      client_connecting_ = false;
      return;
    }

    socket_ = socket;
    peer_ = socket;
  }

  attachPeer(socket);

  try {
    socket->open(options.url);
  } catch (const std::exception& error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (peer_ == socket) {
        peer_.reset();
        socket_.reset();
      }
      client_connecting_ = false;
    }
    reconnect_cv_.notify_all();
    reportError(error.what());
  }
}

void WebSocketSignalTransport::setupServer(const WebSocketServerOptions& options) {
  rtc::WebSocketServer::Configuration configuration;
  configuration.port = options.port;
  configuration.bindAddress = options.bind_address;

  server_ = std::make_unique<rtc::WebSocketServer>(configuration);
  server_->onClient([this](std::shared_ptr<rtc::WebSocket> peer) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (peer_ && !peer_->isClosed()) {
        peer->close();
        return;
      }

      peer_ = peer;
    }

    attachPeer(peer);
  });
}

void WebSocketSignalTransport::attachPeer(const std::shared_ptr<rtc::WebSocket>& peer) {
  peer->onOpen([this]() {
    handleOpen();
  });

  peer->onClosed([this, peer]() {
    handleClosed(peer);
  });

  peer->onError([this](const std::string& error) {
    reportError(error);
  });

  peer->onMessage([this](const auto& message) {
    if (!std::holds_alternative<rtc::string>(message)) {
      reportError("received an unexpected binary websocket message");
      return;
    }

    std::function<void(const std::string&)> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = on_message_;
    }

    if (callback) {
      callback(std::get<rtc::string>(message));
    }
  });
}

void WebSocketSignalTransport::handleOpen() {
  flushPendingMessages();

  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client_connecting_ = false;
    callback = on_connected_;
  }

  reconnect_cv_.notify_all();

  if (callback) {
    callback();
  }
}

void WebSocketSignalTransport::handleClosed(const std::shared_ptr<rtc::WebSocket>& peer) {
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer_ == peer) {
      if (server_) {
        peer_.reset();
      } else {
        peer_.reset();
        socket_.reset();
        client_connecting_ = false;
      }
    }
    callback = on_closed_;
  }

  reconnect_cv_.notify_all();

  if (callback) {
    callback();
  }
}

void WebSocketSignalTransport::flushPendingMessages() {
  std::shared_ptr<rtc::WebSocket> peer;
  std::vector<std::string> pending_messages;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!peer_ || !peer_->isOpen()) {
      return;
    }

    peer = peer_;
    pending_messages.swap(pending_messages_);
  }

  for (const std::string& payload : pending_messages) {
    if (!peer->send(payload)) {
      reportError("failed to flush signaling message over websocket");
      return;
    }
  }
}

void WebSocketSignalTransport::reportError(const std::string& error) {
  std::function<void(const std::string&)> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callback = on_error_;
  }

  if (callback) {
    callback(error);
  }
}

}  // namespace demo
