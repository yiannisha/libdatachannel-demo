#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

typedef struct _GstElement GstElement;
typedef struct _GstAppSink GstAppSink;

namespace demo {

class VideoPipeline {
 public:
  VideoPipeline();
  ~VideoPipeline();

  VideoPipeline(const VideoPipeline&) = delete;
  VideoPipeline& operator=(const VideoPipeline&) = delete;

  void start();
  void stop();
  bool isRunning() const;

 private:
  static void ensureGStreamerInitialized();

  void sampleLoop();
  void logBusMessages();

  GstElement* pipeline_ = nullptr;
  GstAppSink* appsink_ = nullptr;
  std::thread sample_thread_;
  std::atomic<bool> running_{false};
  std::uint64_t sample_count_ = 0;
};

}  // namespace demo
