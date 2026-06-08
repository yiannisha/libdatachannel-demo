#include "Producer.hpp"

#include "SignalingProtocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace demo {

namespace {

struct VideoTrackSpec {
  std::string sink_name;
  std::string mid;
  uint8_t payload_type;
  rtc::SSRC ssrc;
  std::string cname;
  std::string stream_id;
  std::string track_id;
};

rtc::Configuration makePeerConfiguration(const std::string& bind_address) {
  rtc::Configuration config;
  config.iceServers.emplace_back("stun:stun.l.google.com:19302");
  if (!bind_address.empty()) {
    // Pin ICE to a single local interface (e.g. the direct Ethernet link)
    // instead of gathering candidates on every interface.
    config.bindAddress = bind_address;
  }
  return config;
}

std::string formatMessage(const std::variant<rtc::binary, rtc::string>& message) {
  if (std::holds_alternative<rtc::string>(message)) {
    return std::get<rtc::string>(message);
  }

  return "<binary payload: " +
         std::to_string(std::get<rtc::binary>(message).size()) + " bytes>";
}

std::vector<VideoTrackSpec> makeVideoTrackSpecs(
    VideoPipeline::Profile video_pipeline_profile) {
  switch (video_pipeline_profile) {
    case VideoPipeline::Profile::Default:
    case VideoPipeline::Profile::ZedAppsink:
      return {VideoTrackSpec{
          "rtpsink",
          "video",
          96,
          42,
          "video",
          "stream1",
          "track1",
      }};
    case VideoPipeline::Profile::ZedTwoStreamAppsink:
      return {
          VideoTrackSpec{
              "rtpsink_left",
              "video-left",
              96,
              42,
              "video-left",
              "stream-left",
              "track-left",
          },
          VideoTrackSpec{
              "rtpsink_right",
              "video-right",
              97,
              43,
              "video-right",
              "stream-right",
              "track-right",
          },
      };
    case VideoPipeline::Profile::ZedXOneMonoAppsink:
      return {VideoTrackSpec{
          "rtpsink_mono",
          "video-mono",
          98,
          44,
          "video-mono",
          "stream-mono",
          "track-mono",
      }};
  }

  throw std::runtime_error("Unsupported video pipeline profile");
}

std::vector<VideoPipeline::TrackBinding> makeTrackBindings(
    const std::vector<VideoTrackSpec>& specs,
    const std::vector<std::shared_ptr<rtc::Track>>& tracks) {
  if (specs.size() != tracks.size()) {
    throw std::runtime_error("Video track spec count does not match track count");
  }

  std::vector<VideoPipeline::TrackBinding> bindings;
  bindings.reserve(specs.size());
  for (std::size_t index = 0; index < specs.size(); ++index) {
    bindings.push_back(VideoPipeline::TrackBinding{
        specs[index].sink_name,
        tracks[index],
    });
  }
  return bindings;
}

}  // namespace

Producer::Producer(WebSocketSignalTransportConfig signaling_config,
                   VideoPipeline::Profile video_pipeline_profile,
                   VideoPipeline::Config video_pipeline_config,
                   std::string bind_address)
    : peer_config_(makePeerConfiguration(bind_address)),
      video_pipeline_profile_(video_pipeline_profile),
      signaling_transport_(std::move(signaling_config)),
      video_pipeline_(video_pipeline_profile, video_pipeline_config) {
  // Builds the peer connection, adds the video tracks, and binds them to the
  // (not-yet-started) pipeline.
  createPeerConnection();
  video_pipeline_.start();
  setupSignalingTransport();
  signaling_transport_.start();
}

void Producer::wait() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

uint16_t Producer::port() const {
  return signaling_transport_.port();
}

bool Producer::isSignalingServer() const {
  return signaling_transport_.isServer();
}

std::string Producer::signalingEndpoint() const {
  return signaling_transport_.endpointDescription();
}

std::vector<VideoPipeline::OutputStat> Producer::videoOutputStats() const {
  return video_pipeline_.snapshotOutputStats();
}

void Producer::createPeerConnection() {
  const std::vector<VideoTrackSpec> specs =
      makeVideoTrackSpecs(video_pipeline_profile_);

  auto new_pc = std::make_shared<rtc::PeerConnection>(peer_config_);
  wirePeerCallbacks(new_pc);

  std::vector<std::shared_ptr<rtc::Track>> new_tracks;
  new_tracks.reserve(specs.size());
  for (const VideoTrackSpec& spec : specs) {
    rtc::Description::Video video(spec.mid,
                                  rtc::Description::Direction::SendOnly);
    video.addH264Codec(spec.payload_type);
    video.addSSRC(spec.ssrc, spec.cname, spec.stream_id, spec.track_id);

    std::shared_ptr<rtc::Track> track = new_pc->addTrack(video);
    track->onOpen([track, mid = spec.mid]() {
      std::cout << "producer video track open (" << mid
                << ", negotiated-mid=" << track->mid() << ")\n";
    });
    track->onClosed([mid = spec.mid]() {
      std::cout << "producer video track closed (" << mid << ")\n";
    });
    track->onError([mid = spec.mid](const std::string& error) {
      std::cerr << "producer video track error (" << mid << "): " << error
                << '\n';
    });

    new_tracks.push_back(std::move(track));
  }

  const std::vector<VideoPipeline::TrackBinding> bindings =
      makeTrackBindings(specs, new_tracks);

  std::shared_ptr<rtc::PeerConnection> old_pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old_pc = std::move(peer_connection_);
    peer_connection_ = new_pc;
    video_tracks_ = std::move(new_tracks);
    remote_description_set_ = false;
    pending_candidates_.clear();
    data_channel_started_ = false;
    data_channel_.reset();
  }

  // Point the pipeline at the new tracks. setTrackBindings covers the initial
  // (not-yet-started) case; updateTracks re-points a live pipeline on reconnect.
  video_pipeline_.setTrackBindings(bindings);
  if (video_pipeline_.isRunning()) {
    video_pipeline_.updateTracks(bindings);
  }
  // old_pc (if any) is destroyed here, outside the lock, so its in-flight
  // callbacks can drain without deadlocking against mutex_.
}

void Producer::wirePeerCallbacks(
    const std::shared_ptr<rtc::PeerConnection>& pc) {
  pc->onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "producer peer state: " << state << '\n';
  });

  pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "producer gathering state: " << state << '\n';
  });

  pc->onLocalDescription([this](const rtc::Description& description) {
    std::cout << "producer generated local description\n";
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  pc->onLocalCandidate([this](const rtc::Candidate& candidate) {
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });
}

void Producer::setupSignalingTransport() {
  signaling_transport_.setOnConnected([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "producer signaling websocket client connected\n";
    } else {
      std::cout << "producer signaling websocket connected to "
                << signaling_transport_.endpointDescription() << '\n';
    }

    onSignalingConnected();
  });

  signaling_transport_.setOnClosed([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "producer signaling websocket client disconnected\n";
    } else {
      std::cout << "producer signaling websocket closed\n";
    }
  });

  signaling_transport_.setOnError([](const std::string& error) {
    std::cerr << "producer websocket error: " << error << '\n';
  });

  signaling_transport_.setOnMessage([this](const std::string& payload) {
    handleSignalingMessage(payload);
  });
}

void Producer::onSignalingConnected() {
  bool reconnect;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect = signaling_connected_before_;
    signaling_connected_before_ = true;
  }

  if (reconnect) {
    std::cout << "producer signaling reconnected; rebuilding peer connection\n";
    createPeerConnection();
  }

  startDataChannel();
}

void Producer::handleSignalingMessage(const std::string& payload) {
  std::shared_ptr<rtc::PeerConnection> pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pc = peer_connection_;
  }
  if (!pc) {
    return;
  }

  try {
    const SignalingMessage message = parseSignalingMessage(payload);
    switch (message.command) {
      case SignalingCommand::LocalDescription:
        handleRemoteDescription(
            pc, parseRemoteDescription(message, pc->signalingState()));
        break;
      case SignalingCommand::LocalCandidate:
        handleRemoteCandidate(pc, parseRemoteCandidate(message));
        break;
    }
  } catch (const std::exception& error) {
    std::cerr << "producer failed to handle websocket message: " << error.what() << '\n';
  }
}

void Producer::handleRemoteDescription(
    const std::shared_ptr<rtc::PeerConnection>& pc,
    const rtc::Description& description) {
  pc->setRemoteDescription(description);

  std::vector<rtc::Candidate> pending_candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending_candidates.swap(pending_candidates_);
  }

  for (auto& candidate : pending_candidates) {
    pc->addRemoteCandidate(std::move(candidate));
  }
}

void Producer::handleRemoteCandidate(
    const std::shared_ptr<rtc::PeerConnection>& pc,
    const rtc::Candidate& candidate) {
  if (candidate.candidate().empty()) {
    return;
  }

  bool should_queue = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!remote_description_set_) {
      pending_candidates_.push_back(candidate);
      should_queue = true;
    }
  }

  if (!should_queue) {
    pc->addRemoteCandidate(candidate);
  }
}

void Producer::startDataChannel() {
  std::shared_ptr<rtc::PeerConnection> pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_channel_started_) {
      return;
    }
    data_channel_started_ = true;
    pc = peer_connection_;
  }
  if (!pc) {
    return;
  }

  std::shared_ptr<rtc::DataChannel> channel =
      pc->createDataChannel("producer-demo");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data_channel_ = channel;
  }

  channel->onOpen([channel]() {
    std::cout << "producer data channel open (" << channel->label() << ")\n";
    channel->send("hello from producer");
    channel->send("second message from producer");
    channel->send("third message from producer");
  });

  channel->onClosed([]() {
    std::cout << "producer data channel closed\n";
  });

  channel->onMessage([](const auto& message) {
    std::cout << "producer received on data channel: " << formatMessage(message) << '\n';
  });
}

}  // namespace demo
