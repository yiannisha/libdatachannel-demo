#include "VideoPipeline.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace demo {

namespace {

struct OutputSpec {
  const char* sink_name;
  guint max_buffers;
  bool drop;
};

std::string makeDefaultPipelineDescription() {
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

/*
std::string makeZedAppsinkPipelineDescription() {
  return "zedsrc ! "
         "queue ! "
         "videoconvert ! "
         "video/x-raw,format=I420 ! "
         "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=4000 ! "
         "h264parse config-interval=-1 ! "
         "rtph264pay pt=96 ssrc=42 mtu=1200 config-interval=-1 aggregate-mode=zero-latency ! "
         "appsink name=rtpsink emit-signals=true sync=false max-buffers=200 drop=false";
}
*/

/*
std::string makeZedAppsinkPipelineDescription() {
  return "zedsrc ! "
         "queue max-size-buffers=2 leaky=downstream ! "
         "nvvidconv ! "
         "video/x-raw(memory:NVMM),format=NV12 ! "
         "nvv4l2h264enc "
           "maxperf-enable=1 "
           "bitrate=4000000 "
           "control-rate=1 "
           "iframeinterval=30 "
           "idrinterval=30 "
           "insert-sps-pps=true ! "
         "h264parse config-interval=-1 ! "
         "rtph264pay pt=96 ssrc=42 mtu=1200 config-interval=-1 aggregate-mode=zero-latency ! "
         "appsink name=rtpsink emit-signals=true sync=false max-buffers=200 drop=false";
}
*/

// std::string makeZedAppsinkPipelineDescription() {
//   return "zedsrc stream-type=6 camera-resolution=2 camera-fps=30 ! "
//          "video/x-raw(memory:NVMM),format=NV12 ! "
//          "queue max-size-buffers=1 leaky=downstream ! "
//          "nvv4l2h264enc "
//            "maxperf-enable=1 "
//            "preset-level=1 "
//            "insert-sps-pps=true "
//            "iframeinterval=30 "
//            "idrinterval=30 "
//            "bitrate=4000000 "
//            "control-rate=1 "
//            "num-B-Frames=0 ! "
//          "h264parse config-interval=-1 ! "
//          "rtph264pay "
//            "pt=96 "
//            "ssrc=42 "
//            "mtu=1200 "
//            "config-interval=-1 "
//            "aggregate-mode=zero-latency ! "
//          "appsink "
//            "name=rtpsink "
//            "emit-signals=true "
//            "sync=false "
//            "async=false "
//            "max-buffers=1 "
//            "drop=true "
//            "wait-on-eos=false";
// }

std::string makeZedAppsinkPipelineDescription() {
  return "zedsrc "
           "stream-type=7 "
           "camera-resolution=2 "
           "camera-fps=30 "

           // Disable ZED SDK processing modules
           "depth-mode=0 "
           "depth-stabilization=0 "
           "fill-mode=false "
           "enable-positional-tracking=false "
           "enable-area-memory=false "
           "enable-imu-fusion=false "
           "enable-pose-smoothing=false "
           "bt-enabled=false "
           "bt-body-tracking=false "
           "bt-body-fitting=false "
           "od-enabled=false "
           "od-enable-tracking=false "
           "roi=false "
           "sdk-verbose=0 "

         "! video/x-raw(memory:NVMM),format=NV12 "
         "! queue max-size-buffers=1 leaky=downstream "
         "! nvv4l2h264enc "
           "maxperf-enable=1 "
           "preset-level=1 "
           "insert-sps-pps=true "
           "iframeinterval=30 "
           "idrinterval=30 "
           "bitrate=4000000 "
           "control-rate=1 "
           "num-B-Frames=0 "
         "! h264parse config-interval=-1 "
         "! rtph264pay "
           "pt=96 "
           "ssrc=42 "
           "mtu=1200 "
           "config-interval=-1 "
           "aggregate-mode=zero-latency "
         "! appsink "
           "name=rtpsink "
           "emit-signals=true "
           "sync=false "
           "async=false "
           "max-buffers=1 "
           "drop=true "
           "wait-on-eos=false";
}

std::string makeZedTwoStreamAppsinkPipelineDescription() {
  return "zedsrc stream-type=2 camera-resolution=2 camera-fps=30 ! "
         "queue max-size-buffers=1 leaky=downstream ! "
         "zeddemux is-depth=false is-mono=false name=demux "

         "demux.src_left ! "
         "queue max-size-buffers=1 leaky=downstream ! "
         "videoconvert ! video/x-raw,format=RGBA ! "
         "nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
         "nvv4l2h264enc "
           "maxperf-enable=1 "
           "preset-level=1 "
           "insert-sps-pps=true "
           "iframeinterval=30 "
           "idrinterval=30 "
           "bitrate=4000000 "
           "control-rate=1 "
           "num-B-Frames=0 ! "
         "h264parse config-interval=-1 ! "
         "rtph264pay pt=96 ssrc=42 mtu=1200 config-interval=-1 aggregate-mode=zero-latency ! "
         "appsink name=rtpsink_left emit-signals=true sync=false async=false max-buffers=1 drop=true wait-on-eos=false "

         "demux.src_aux ! "
         "queue max-size-buffers=1 leaky=downstream ! "
         "videoconvert ! video/x-raw,format=RGBA ! "
         "nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
         "nvv4l2h264enc "
           "maxperf-enable=1 "
           "preset-level=1 "
           "insert-sps-pps=true "
           "iframeinterval=30 "
           "idrinterval=30 "
           "bitrate=4000000 "
           "control-rate=1 "
           "num-B-Frames=0 ! "
         "h264parse config-interval=-1 ! "
         "rtph264pay pt=97 ssrc=43 mtu=1200 config-interval=-1 aggregate-mode=zero-latency ! "
         "appsink name=rtpsink_right emit-signals=true sync=false async=false max-buffers=1 drop=true wait-on-eos=false";
}

std::string makePipelineDescription(VideoPipeline::Profile profile) {
  switch (profile) {
    case VideoPipeline::Profile::Default:
      return makeDefaultPipelineDescription();
    case VideoPipeline::Profile::ZedAppsink:
      return makeZedAppsinkPipelineDescription();
    case VideoPipeline::Profile::ZedTwoStreamAppsink:
      return makeZedTwoStreamAppsinkPipelineDescription();
  }

  throw std::runtime_error("Unsupported video pipeline profile");
}

std::vector<OutputSpec> makeOutputSpecs(VideoPipeline::Profile profile) {
  switch (profile) {
    case VideoPipeline::Profile::Default:
      return {OutputSpec{"rtpsink", 200, false}};
    case VideoPipeline::Profile::ZedAppsink:
      return {OutputSpec{"rtpsink", 1, true}};
    case VideoPipeline::Profile::ZedTwoStreamAppsink:
      return {
          OutputSpec{"rtpsink_left", 1, true},
          OutputSpec{"rtpsink_right", 1, true},
      };
  }

  throw std::runtime_error("Unsupported video pipeline profile");
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

VideoPipeline::VideoPipeline(Profile profile)
    : profile_(profile) {}

VideoPipeline::VideoPipeline()
    : VideoPipeline(Profile::Default) {}

VideoPipeline::~VideoPipeline() {
  stop();
}

void VideoPipeline::setTrackBindings(std::vector<TrackBinding> bindings) {
  std::lock_guard<std::mutex> lock(mutex_);
  track_bindings_ = std::move(bindings);
}

void VideoPipeline::start() {
  if (running_) {
    return;
  }

  ensureGStreamerInitialized();

  GError* error = nullptr;
  const std::string pipeline_description = makePipelineDescription(profile_);
  pipeline_ = gst_parse_launch(pipeline_description.c_str(), &error);
  if (!pipeline_) {
    const std::string message =
        error ? error->message : "gst_parse_launch returned a null pipeline";
    if (error) {
      g_error_free(error);
    }
    throw std::runtime_error("Failed to create GStreamer pipeline: " + message);
  }

  std::vector<TrackBinding> track_bindings;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    track_bindings = track_bindings_;
  }

  std::vector<ActiveOutput> outputs;
  const std::vector<OutputSpec> output_specs = makeOutputSpecs(profile_);
  outputs.reserve(output_specs.size());

  try {
    for (const OutputSpec& spec : output_specs) {
      GstAppSink* appsink =
          GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline_), spec.sink_name));
      if (!appsink) {
        throw std::runtime_error(
            "Failed to find appsink named " + std::string(spec.sink_name) +
            " in pipeline");
      }

      const auto binding_it = std::find_if(
          track_bindings.begin(),
          track_bindings.end(),
          [&spec](const TrackBinding& binding) {
            return binding.sink_name == spec.sink_name;
          });
      if (binding_it == track_bindings.end() || !binding_it->track) {
        gst_object_unref(appsink);
        throw std::runtime_error(
            "Missing track binding for appsink " + std::string(spec.sink_name));
      }

      gst_app_sink_set_emit_signals(appsink, TRUE);
      gst_app_sink_set_max_buffers(appsink, spec.max_buffers);
#if GST_CHECK_VERSION(1, 28, 0)
      gst_app_sink_set_leaky_type(
          appsink,
          spec.drop ? GST_APP_LEAKY_TYPE_DOWNSTREAM : GST_APP_LEAKY_TYPE_NONE);
#else
      gst_app_sink_set_drop(appsink, spec.drop ? TRUE : FALSE);
#endif
      g_signal_connect(
          appsink, "new-sample", G_CALLBACK(&VideoPipeline::onNewRtpSample), this);

      outputs.push_back(ActiveOutput{
          std::string(spec.sink_name),
          appsink,
          binding_it->track,
          0,
      });
    }
  } catch (...) {
    for (const ActiveOutput& output : outputs) {
      gst_object_unref(output.appsink);
    }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    outputs_ = std::move(outputs);
  }

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
  std::vector<GstAppSink*> sinks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks.reserve(outputs_.size());
    for (const ActiveOutput& output : outputs_) {
      sinks.push_back(output.appsink);
    }
  }

  if (pipeline_) {
    for (GstAppSink* sink : sinks) {
      g_signal_handlers_disconnect_by_data(sink, this);
    }
    gst_element_set_state(pipeline_, GST_STATE_NULL);
  }

  if (was_running && bus_thread_.joinable()) {
    bus_thread_.join();
  } else if (bus_thread_.joinable()) {
    bus_thread_.join();
  }

  for (GstAppSink* sink : sinks) {
    gst_object_unref(sink);
  }

  if (pipeline_) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    outputs_.clear();
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
  std::string sink_name;
  std::uint64_t packet_count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto output_it = std::find_if(
        outputs_.begin(),
        outputs_.end(),
        [sink](const ActiveOutput& output) {
          return output.appsink == sink;
        });
    if (output_it == outputs_.end()) {
      gst_buffer_unmap(buffer, &map);
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }

    track = output_it->track;
    sink_name = output_it->sink_name;
    packet_count = ++output_it->rtp_packet_count;
  }

  const bool track_open = track && track->isOpen();
  const bool sent = track_open
      ? track->send(reinterpret_cast<const rtc::byte*>(map.data), map.size)
      : false;

  if (packet_count == 1 || packet_count % 30 == 0) {
    gchar* caps_text = caps ? gst_caps_to_string(caps) : nullptr;
    if (map.size >= sizeof(rtc::RtpHeader)) {
      const auto* header = reinterpret_cast<const rtc::RtpHeader*>(map.data);
      std::cout << "video pipeline output " << sink_name
                << " produced RTP packet #" << packet_count
                << " bytes=" << map.size
                << " payloadType=" << static_cast<int>(header->payloadType())
                << " seq=" << header->seqNumber()
                << " timestamp=" << header->timestamp()
                << " ssrc=" << header->ssrc()
                << " trackOpen=" << (track_open ? "true" : "false")
                << " forwarded=" << (sent ? "true" : "buffered-or-skipped")
                << " caps=" << (caps_text ? caps_text : "<unknown>") << '\n';
    } else {
      std::cout << "video pipeline output " << sink_name
                << " produced short RTP packet #" << packet_count
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
