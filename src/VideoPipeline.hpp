#pragma once

#include "rtc/rtc.hpp"

#include <gst/app/gstappsink.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace demo {

class VideoPipeline {
 public:
  VideoPipeline();
  ~VideoPipeline();

 VideoPipeline(const VideoPipeline&) = delete;
  VideoPipeline& operator=(const VideoPipeline&) = delete;

  void setTrack(std::shared_ptr<rtc::Track> track);
  void start();
  void stop();
  bool isRunning() const;

 private:
  static void ensureGStreamerInitialized();
  static GstFlowReturn onNewRtpSample(GstAppSink* sink, gpointer user_data);

  GstFlowReturn handleNewRtpSample(GstAppSink* sink);
  void busLoop();
  void logBusMessages();

  GstElement* pipeline_ = nullptr;
  GstAppSink* appsink_ = nullptr;
  std::thread bus_thread_;
  std::atomic<bool> running_{false};
  std::shared_ptr<rtc::Track> track_;
  std::mutex mutex_;
  std::uint64_t rtp_packet_count_ = 0;
};

}  // namespace demo
