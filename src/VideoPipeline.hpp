#pragma once

#include "rtc/rtc.hpp"

#include <gst/app/gstappsink.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace demo {

class VideoPipeline {
 public:
  struct TrackBinding {
    std::string sink_name;
    std::shared_ptr<rtc::Track> track;
  };

  enum class Profile {
    Default,
    ZedAppsink,
    ZedTwoStreamAppsink,
  };

  explicit VideoPipeline(Profile profile = Profile::Default);
  VideoPipeline();
  ~VideoPipeline();

  VideoPipeline(const VideoPipeline&) = delete;
  VideoPipeline& operator=(const VideoPipeline&) = delete;

  void setTrackBindings(std::vector<TrackBinding> bindings);
  void start();
  void stop();
  bool isRunning() const;

 private:
  static void ensureGStreamerInitialized();
  static GstFlowReturn onNewRtpSample(GstAppSink* sink, gpointer user_data);

  GstFlowReturn handleNewRtpSample(GstAppSink* sink);
  void busLoop();
  void logBusMessages();

  struct ActiveOutput {
    std::string sink_name;
    GstAppSink* appsink = nullptr;
    std::shared_ptr<rtc::Track> track;
    std::uint64_t rtp_packet_count = 0;
  };

  GstElement* pipeline_ = nullptr;
  std::thread bus_thread_;
  std::atomic<bool> running_{false};
  Profile profile_ = Profile::Default;
  std::vector<TrackBinding> track_bindings_;
  std::vector<ActiveOutput> outputs_;
  std::mutex mutex_;
};

}  // namespace demo
