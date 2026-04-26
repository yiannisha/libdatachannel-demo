#pragma once

#include "rtc/rtc.hpp"

#include <string>
#include <string_view>

namespace demo {

enum class SignalingCommand {
  LocalDescription,
  LocalCandidate,
};

struct SignalingMessage {
  SignalingCommand command;
  std::string description;
  std::string type;
  std::string mid;
};

std::string serializeSignalingMessage(const SignalingMessage& message);
SignalingMessage parseSignalingMessage(std::string_view payload);

SignalingMessage makeLocalDescriptionMessage(const rtc::Description& description);
SignalingMessage makeLocalCandidateMessage(const rtc::Candidate& candidate);

rtc::Description parseRemoteDescription(const SignalingMessage& message,
                                       rtc::PeerConnection::SignalingState signaling_state);
rtc::Candidate parseRemoteCandidate(const SignalingMessage& message);

}  // namespace demo
