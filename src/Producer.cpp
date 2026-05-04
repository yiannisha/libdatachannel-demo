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

rtc::Configuration makePeerConfiguration() {
  rtc::Configuration config;
  config.iceServers.emplace_back("stun:stun.l.google.com:19302");
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
                   VideoPipeline::Profile video_pipeline_profile)
    : peer_connection_(makePeerConfiguration()),
      signaling_transport_(std::move(signaling_config)),
      video_pipeline_(video_pipeline_profile) {
  setupPeerConnection();
  setupVideoTracks(video_pipeline_profile);
  video_pipeline_.setTrackBindings(
      makeTrackBindings(makeVideoTrackSpecs(video_pipeline_profile), video_tracks_));
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

void Producer::setupPeerConnection() {
  peer_connection_.onStateChange([](rtc::PeerConnection::State state) {
    std::cout << "producer peer state: " << state << '\n';
  });

  peer_connection_.onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
    std::cout << "producer gathering state: " << state << '\n';
  });

  peer_connection_.onLocalDescription([this](const rtc::Description& description) {
    std::cout << "producer generated local description\n";
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalDescriptionMessage(description)));
  });

  peer_connection_.onLocalCandidate([this](const rtc::Candidate& candidate) {
    signaling_transport_.send(
        serializeSignalingMessage(makeLocalCandidateMessage(candidate)));
  });
}

void Producer::setupVideoTracks(VideoPipeline::Profile video_pipeline_profile) {
  const std::vector<VideoTrackSpec> specs = makeVideoTrackSpecs(video_pipeline_profile);
  video_tracks_.reserve(specs.size());

  for (const VideoTrackSpec& spec : specs) {
    rtc::Description::Video video(spec.mid, rtc::Description::Direction::SendOnly);
    video.addH264Codec(spec.payload_type);
    video.addSSRC(spec.ssrc, spec.cname, spec.stream_id, spec.track_id);

    std::shared_ptr<rtc::Track> track = peer_connection_.addTrack(video);
    track->onOpen([track, mid = spec.mid]() {
      std::cout << "producer video track open (" << mid
                << ", negotiated-mid=" << track->mid() << ")\n";
    });
    track->onClosed([mid = spec.mid]() {
      std::cout << "producer video track closed (" << mid << ")\n";
    });
    track->onError([mid = spec.mid](const std::string& error) {
      std::cerr << "producer video track error (" << mid << "): " << error << '\n';
    });

    video_tracks_.push_back(std::move(track));
  }
}

void Producer::setupSignalingTransport() {
  signaling_transport_.setOnConnected([this]() {
    if (signaling_transport_.isServer()) {
      std::cout << "producer signaling websocket client connected\n";
    } else {
      std::cout << "producer signaling websocket connected to "
                << signaling_transport_.endpointDescription() << '\n';
    }

    startDataChannel();
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

void Producer::handleSignalingMessage(const std::string& payload) {
  try {
    const SignalingMessage message = parseSignalingMessage(payload);
    switch (message.command) {
      case SignalingCommand::LocalDescription:
        handleRemoteDescription(
            parseRemoteDescription(message, peer_connection_.signalingState()));
        break;
      case SignalingCommand::LocalCandidate:
        handleRemoteCandidate(parseRemoteCandidate(message));
        break;
    }
  } catch (const std::exception& error) {
    std::cerr << "producer failed to handle websocket message: " << error.what() << '\n';
  }
}

void Producer::handleRemoteDescription(const rtc::Description& description) {
  peer_connection_.setRemoteDescription(description);

  std::vector<rtc::Candidate> pending_candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending_candidates.swap(pending_candidates_);
  }

  for (auto& candidate : pending_candidates) {
    peer_connection_.addRemoteCandidate(std::move(candidate));
  }
}

void Producer::handleRemoteCandidate(const rtc::Candidate& candidate) {
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
    peer_connection_.addRemoteCandidate(candidate);
  }
}

void Producer::startDataChannel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (data_channel_started_) {
    return;
  }

  data_channel_started_ = true;
  data_channel_ = peer_connection_.createDataChannel("producer-demo");
  data_channel_->onOpen([channel = data_channel_]() {
    std::cout << "producer data channel open (" << channel->label() << ")\n";
    channel->send("hello from producer");
    channel->send("second message from producer");
    channel->send("third message from producer");
  });

  data_channel_->onClosed([]() {
    std::cout << "producer data channel closed\n";
  });

  data_channel_->onMessage([](const auto& message) {
    std::cout << "producer received on data channel: " << formatMessage(message) << '\n';
  });
}

}  // namespace demo
