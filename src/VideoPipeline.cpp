#include "VideoPipeline.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace demo {

namespace {

std::string makePipelineDescription() {
#if defined(__APPLE__)
  constexpr char kVideoSource[] = "avfvideosrc";
#else
  constexpr char kVideoSource[] = "autovideosrc";
#endif

  return std::string(kVideoSource) +
         " ! "
         "videoconvert ! "
         "video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! "
         "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=4000 ! "
         "h264parse config-interval=-1 ! "
         "rtph264pay pt=96 ssrc=42 mtu=1200 config-interval=-1 aggregate-mode=zero-latency ! "
         "appsink name=rtpsink emit-signals=true sync=false max-buffers=200 drop=false";
}

std::mutex& gstreamerInitMutex() {
  static std::mutex mutex;
  return mutex;
}

bool& gstreamerInitialized() {
  static bool initialized = false;
  return initialized;
}

}  // namespace

VideoPipeline::VideoPipeline() = default;

VideoPipeline::~VideoPipeline() {
  stop();
}

void VideoPipeline::setTrack(std::shared_ptr<rtc::Track> track) {
  std::lock_guard<std::mutex> lock(mutex_);
  track_ = std::move(track);
}

void VideoPipeline::start() {
  if (running_) {
    return;
  }

  ensureGStreamerInitialized();

  GError* error = nullptr;
  const std::string pipeline_description = makePipelineDescription();
  pipeline_ = gst_parse_launch(pipeline_description.c_str(), &error);
  if (!pipeline_) {
    const std::string message =
        error ? error->message : "gst_parse_launch returned a null pipeline";
    if (error) {
      g_error_free(error);
    }
    throw std::runtime_error("Failed to create GStreamer pipeline: " + message);
  }

  appsink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "rtpsink"));
  if (!appsink_) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw std::runtime_error("Failed to find appsink named rtpsink in pipeline");
  }

  gst_app_sink_set_emit_signals(appsink_, TRUE);
  gst_app_sink_set_max_buffers(appsink_, 200);
#if GST_CHECK_VERSION(1, 28, 0)
  gst_app_sink_set_leaky_type(appsink_, GST_APP_LEAKY_TYPE_NONE);
#else
  gst_app_sink_set_drop(appsink_, FALSE);
#endif
  g_signal_connect(appsink_, "new-sample", G_CALLBACK(&VideoPipeline::onNewRtpSample), this);

  const GstStateChangeReturn state_change =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (state_change == GST_STATE_CHANGE_FAILURE) {
    stop();
    throw std::runtime_error("Failed to set GStreamer pipeline to PLAYING");
  }

  running_ = true;
  bus_thread_ = std::thread([this]() {
    busLoop();
  });

  std::cout << "video pipeline started: " << pipeline_description << '\n';
}

void VideoPipeline::stop() {
  const bool was_running = running_.exchange(false);

  if (pipeline_) {
    if (appsink_) {
      g_signal_handlers_disconnect_by_data(appsink_, this);
    }
    gst_element_set_state(pipeline_, GST_STATE_NULL);
  }

  if (was_running && bus_thread_.joinable()) {
    bus_thread_.join();
  } else if (bus_thread_.joinable()) {
    bus_thread_.join();
  }

  if (appsink_) {
    gst_object_unref(appsink_);
    appsink_ = nullptr;
  }

  if (pipeline_) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
}

bool VideoPipeline::isRunning() const {
  return running_;
}

void VideoPipeline::ensureGStreamerInitialized() {
  std::lock_guard<std::mutex> lock(gstreamerInitMutex());
  if (gstreamerInitialized()) {
    return;
  }

  GError* error = nullptr;
  if (!gst_init_check(nullptr, nullptr, &error)) {
    const std::string message =
        error ? error->message : "gst_init_check returned false";
    if (error) {
      g_error_free(error);
    }
    throw std::runtime_error("Failed to initialize GStreamer: " + message);
  }

  gstreamerInitialized() = true;
}

GstFlowReturn VideoPipeline::onNewRtpSample(GstAppSink* sink, gpointer user_data) {
  return static_cast<VideoPipeline*>(user_data)->handleNewRtpSample(sink);
}

GstFlowReturn VideoPipeline::handleNewRtpSample(GstAppSink* sink) {
  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) {
    return GST_FLOW_EOS;
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  std::shared_ptr<rtc::Track> track;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    track = track_;
  }

  const bool track_open = track && track->isOpen();
  const bool sent = track_open
      ? track->send(reinterpret_cast<const rtc::byte*>(map.data), map.size)
      : false;

  ++rtp_packet_count_;
  if (rtp_packet_count_ == 1 || rtp_packet_count_ % 30 == 0) {
    gchar* caps_text = caps ? gst_caps_to_string(caps) : nullptr;
    if (map.size >= sizeof(rtc::RtpHeader)) {
      const auto* header = reinterpret_cast<const rtc::RtpHeader*>(map.data);
      std::cout << "video pipeline produced RTP packet #" << rtp_packet_count_
                << " bytes=" << map.size
                << " payloadType=" << static_cast<int>(header->payloadType())
                << " seq=" << header->seqNumber()
                << " timestamp=" << header->timestamp()
                << " ssrc=" << header->ssrc()
                << " trackOpen=" << (track_open ? "true" : "false")
                << " forwarded=" << (sent ? "true" : "buffered-or-skipped")
                << " caps=" << (caps_text ? caps_text : "<unknown>") << '\n';
    } else {
      std::cout << "video pipeline produced short RTP packet #" << rtp_packet_count_
                << " bytes=" << map.size << '\n';
    }

    if (caps_text) {
      g_free(caps_text);
    }
  }

  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

void VideoPipeline::busLoop() {
  while (running_) {
    logBusMessages();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  logBusMessages();
}

void VideoPipeline::logBusMessages() {
  if (!pipeline_) {
    return;
  }

  GstBus* bus = gst_element_get_bus(pipeline_);
  if (!bus) {
    return;
  }

  for (;;) {
    GstMessage* message = gst_bus_pop_filtered(
        bus,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING |
                                    GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED));
    if (!message) {
      break;
    }

    switch (GST_MESSAGE_TYPE(message)) {
      case GST_MESSAGE_ERROR: {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::cerr << "video pipeline error: "
                  << (error ? error->message : "unknown error");
        if (debug) {
          std::cerr << " (" << debug << ")";
        }
        std::cerr << '\n';
        if (error) {
          g_error_free(error);
        }
        if (debug) {
          g_free(debug);
        }
        break;
      }
      case GST_MESSAGE_WARNING: {
        GError* warning = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_warning(message, &warning, &debug);
        std::cerr << "video pipeline warning: "
                  << (warning ? warning->message : "unknown warning");
        if (debug) {
          std::cerr << " (" << debug << ")";
        }
        std::cerr << '\n';
        if (warning) {
          g_error_free(warning);
        }
        if (debug) {
          g_free(debug);
        }
        break;
      }
      case GST_MESSAGE_EOS:
        std::cout << "video pipeline reached end-of-stream\n";
        break;
      case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline_)) {
          GstState old_state;
          GstState new_state;
          GstState pending_state;
          gst_message_parse_state_changed(
              message, &old_state, &new_state, &pending_state);
          std::cout << "video pipeline state changed: "
                    << gst_element_state_get_name(old_state) << " -> "
                    << gst_element_state_get_name(new_state) << '\n';
        }
        break;
      default:
        break;
    }

    gst_message_unref(message);
  }

  gst_object_unref(bus);
}

}  // namespace demo
