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
    ZedXOneMonoAppsink,
  };

  struct Config {
    explicit constexpr Config(int camera_id = -1)
        : zed_x_one_camera_id(camera_id) {}

    int zed_x_one_camera_id;
  };

  explicit VideoPipeline(Profile profile = Profile::Default,
                         Config config = Config{});
  VideoPipeline();
  ~VideoPipeline();

  VideoPipeline(const VideoPipeline&) = delete;
  VideoPipeline& operator=(const VideoPipeline&) = delete;

  struct OutputStat {
    std::string sink_name;
    std::uint64_t rtp_packets;
    std::uint64_t frames;
  };

  void setTrackBindings(std::vector<TrackBinding> bindings);
  void start();
  void stop();
  bool isRunning() const;

  // Snapshot of monotonic per-output counters (RTP packets and completed
  // frames). Cheap; intended to be polled from a low-rate stats timer.
  std::vector<OutputStat> snapshotOutputStats() const;

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
    std::uint64_t frame_count = 0;
  };

  GstElement* pipeline_ = nullptr;
  std::thread bus_thread_;
  std::atomic<bool> running_{false};
  Profile profile_ = Profile::Default;
  Config config_;
  std::vector<TrackBinding> track_bindings_;
  std::vector<ActiveOutput> outputs_;
  mutable std::mutex mutex_;
};

}  // namespace demo
