#pragma once

#include "rtc/rtc.hpp"

namespace demo {

class Signaler {
 public:
  virtual ~Signaler() = default;

  virtual void deliverDescription(const rtc::Description& description) = 0;
  virtual void deliverCandidate(const rtc::Candidate& candidate) = 0;
};

}  // namespace demo
