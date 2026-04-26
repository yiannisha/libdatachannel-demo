#include "InMemorySignaler.hpp"

#include <utility>

namespace demo {

InMemorySignaler::InMemorySignaler(std::shared_ptr<rtc::PeerConnection> target)
    : target_(std::move(target)) {}

void InMemorySignaler::deliverDescription(const rtc::Description& description) {
  target_->setRemoteDescription(description);

  std::vector<PendingCandidate> pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_description_set_ = true;
    pending.swap(pending_candidates_);
  }

  // Deliver any candidates that were queued while waiting for the remote
  // description to be set.
  for (const auto& candidate : pending) {
    target_->addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
  }
}

void InMemorySignaler::deliverCandidate(const rtc::Candidate& candidate) {
  if (candidate.candidate().empty()) {
    return;
  }

  bool queue_candidate = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // If the remote description has not been set yet, queue candidates until it
    // is available so the peer connection does not reject them.
    if (!remote_description_set_) {
      pending_candidates_.push_back(
          PendingCandidate{candidate.candidate(), candidate.mid()});
      queue_candidate = true;
    }
  }

  if (!queue_candidate) {
    target_->addRemoteCandidate(rtc::Candidate(candidate.candidate(), candidate.mid()));
  }
}

}  // namespace demo
