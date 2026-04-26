#include "VideoPipeline.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace demo {

namespace {

constexpr char kPipelineDescription[] =
    "autovideosrc ! "
    "videoconvert ! "
    "video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! "
    "appsink name=video_sink emit-signals=false sync=false max-buffers=1 drop=true";

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

void VideoPipeline::start() {
  if (running_) {
    return;
  }

  ensureGStreamerInitialized();

  GError* error = nullptr;
  pipeline_ = gst_parse_launch(kPipelineDescription, &error);
  if (!pipeline_) {
    const std::string message =
        error ? error->message : "gst_parse_launch returned a null pipeline";
    if (error) {
      g_error_free(error);
    }
    throw std::runtime_error("Failed to create GStreamer pipeline: " + message);
  }

  appsink_ = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), "video_sink"));
  if (!appsink_) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw std::runtime_error("Failed to find appsink named video_sink in pipeline");
  }

  gst_app_sink_set_emit_signals(appsink_, FALSE);
  gst_app_sink_set_max_buffers(appsink_, 1);
#if GST_CHECK_VERSION(1, 28, 0)
  gst_app_sink_set_leaky_type(appsink_, GST_APP_LEAKY_TYPE_DOWNSTREAM);
#else
  gst_app_sink_set_drop(appsink_, TRUE);
#endif

  const GstStateChangeReturn state_change =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (state_change == GST_STATE_CHANGE_FAILURE) {
    stop();
    throw std::runtime_error("Failed to set GStreamer pipeline to PLAYING");
  }

  running_ = true;
  sample_thread_ = std::thread([this]() {
    sampleLoop();
  });

  std::cout << "video pipeline started with camera source feeding appsink\n";
}

void VideoPipeline::stop() {
  const bool was_running = running_.exchange(false);
  if (was_running && sample_thread_.joinable()) {
    sample_thread_.join();
  } else if (sample_thread_.joinable()) {
    sample_thread_.join();
  }

  if (pipeline_) {
    gst_element_send_event(pipeline_, gst_event_new_eos());
    gst_element_set_state(pipeline_, GST_STATE_NULL);
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

void VideoPipeline::sampleLoop() {
  while (running_) {
    GstSample* sample = gst_app_sink_try_pull_sample(
        appsink_, static_cast<GstClockTime>(250 * GST_MSECOND));
    if (!sample) {
      logBusMessages();
      continue;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);

    ++sample_count_;
    if (sample_count_ == 1 || sample_count_ % 30 == 0) {
      gchar* caps_text = caps ? gst_caps_to_string(caps) : nullptr;
      const gsize buffer_size = buffer ? gst_buffer_get_size(buffer) : 0;
      std::cout << "video pipeline received sample #" << sample_count_
                << " size=" << buffer_size
                << " caps=" << (caps_text ? caps_text : "<unknown>") << '\n';
      if (caps_text) {
        g_free(caps_text);
      }
    }

    gst_sample_unref(sample);
    logBusMessages();
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
