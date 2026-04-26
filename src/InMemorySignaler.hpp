#pragma once

#include "Signaler.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo {

class InMemorySignaler : public Signaler {
 public:
  explicit InMemorySignaler(std::shared_ptr<rtc::PeerConnection> target);

  void deliverDescription(const rtc::Description& description) override;
  void deliverCandidate(const rtc::Candidate& candidate) override;

 private:
  struct PendingCandidate {
    std::string candidate;
    std::string mid;
  };

  std::shared_ptr<rtc::PeerConnection> target_;
  std::mutex mutex_;
  bool remote_description_set_ = false;
  std::vector<PendingCandidate> pending_candidates_;
};

}  // namespace demo
