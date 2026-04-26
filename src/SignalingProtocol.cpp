#include "SignalingProtocol.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace demo {

namespace {

using json = nlohmann::json;

constexpr char kLocalDescriptionCommand[] = "localDescription";
constexpr char kLocalCandidateCommand[] = "localCandidate";

std::string commandToString(SignalingCommand command) {
  switch (command) {
    case SignalingCommand::LocalDescription:
      return kLocalDescriptionCommand;
    case SignalingCommand::LocalCandidate:
      return kLocalCandidateCommand;
  }

  throw std::invalid_argument("Unknown signaling command");
}

SignalingCommand commandFromString(const std::string& command) {
  if (command == kLocalDescriptionCommand) {
    return SignalingCommand::LocalDescription;
  }

  if (command == kLocalCandidateCommand) {
    return SignalingCommand::LocalCandidate;
  }

  throw std::invalid_argument("Unsupported signaling command: " + command);
}

rtc::Description::Type inferRemoteDescriptionType(
    rtc::PeerConnection::SignalingState signaling_state) {
  switch (signaling_state) {
    case rtc::PeerConnection::SignalingState::HaveLocalOffer:
      return rtc::Description::Type::Answer;
    default:
      return rtc::Description::Type::Offer;
  }
}

}  // namespace

std::string serializeSignalingMessage(const SignalingMessage& message) {
  return json{
      {"command", commandToString(message.command)},
      {"description", message.description},
  }
      .dump();
}

SignalingMessage parseSignalingMessage(std::string_view payload) {
  const json parsed = json::parse(payload);

  if (!parsed.contains("command") || !parsed.at("command").is_string()) {
    throw std::invalid_argument("Signaling message is missing a string command field");
  }

  if (!parsed.contains("description") || !parsed.at("description").is_string()) {
    throw std::invalid_argument("Signaling message is missing a string description field");
  }

  return SignalingMessage{
      commandFromString(parsed.at("command").get<std::string>()),
      parsed.at("description").get<std::string>(),
  };
}

SignalingMessage makeLocalDescriptionMessage(const rtc::Description& description) {
  return SignalingMessage{
      SignalingCommand::LocalDescription,
      std::string(description),
  };
}

SignalingMessage makeLocalCandidateMessage(const rtc::Candidate& candidate) {
  return SignalingMessage{
      SignalingCommand::LocalCandidate,
      std::string(candidate),
  };
}

rtc::Description parseRemoteDescription(
    const SignalingMessage& message,
    rtc::PeerConnection::SignalingState signaling_state) {
  if (message.command != SignalingCommand::LocalDescription) {
    throw std::invalid_argument("Expected a localDescription signaling message");
  }

  return rtc::Description(message.description, inferRemoteDescriptionType(signaling_state));
}

rtc::Candidate parseRemoteCandidate(const SignalingMessage& message) {
  if (message.command != SignalingCommand::LocalCandidate) {
    throw std::invalid_argument("Expected a localCandidate signaling message");
  }

  return rtc::Candidate(message.description);
}

}  // namespace demo
